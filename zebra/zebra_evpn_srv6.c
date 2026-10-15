// SPDX-License-Identifier: GPL-2.0-or-later
/* VPP backend for Zebra's existing EVPN dataplane contexts.
 * No BGP RIB traversal, DF election, or nexthop selection lives here. */
#include <zebra.h>
#include <sys/un.h>
#include <sys/eventfd.h>
#include <pthread.h>
#include "buffer.h"
#include "frrevent.h"
#include "memory.h"
#include "network.h"
#include "log.h"
#include "stream.h"
#include "zebra/zebra_router.h"
#include "zebra/zebra_vrf.h"
#include "zebra/zebra_evpn.h"
#include "zebra/zebra_evpn_mac.h"
#include "zebra/zebra_evpn_mh.h"
#include "zebra/zebra_vxlan.h"
#include "zebra/zebra_evpn_srv6.h"
#include "zebra/zebra_evpn_srv6_wire.h"
#include "zebra/zebra_srv6.h"

static int listener = -1, client_fd = -1, notify_fd = -1;
static struct event *accept_event, *read_event, *write_event, *notify_event;
static struct buffer *output;
static size_t queued, input_used;
static struct evpn_srv6_msg input;
struct owned {
	struct owned *next;
	struct evpn_srv6_msg msg;
};
static struct owned *owned;
struct sid_entry {
	struct sid_entry *next;
	vni_t vni;
	uint8_t mode;
	struct in6_addr peer, sid;
};
static struct sid_entry *sids;
struct update {
	struct update *next;
	struct evpn_srv6_msg msg;
};
static struct update *updates, **update_tail = &updates;
static pthread_mutex_t update_lock = PTHREAD_MUTEX_INITIALIZER;
static size_t update_bytes;
static uint32_t epoch;
static bool connected, overflow;
uint32_t zebra_evpn_srv6_epoch(void)
{
	uint32_t value;
	pthread_mutex_lock(&update_lock);
	value = epoch;
	pthread_mutex_unlock(&update_lock);
	return value;
}
static void agent_send(const struct evpn_srv6_msg *message);

bool zebra_evpn_srv6_native(vni_t vni)
{
	for (struct owned *o = owned; o; o = o->next)
		if (o->msg.op == EVPN_SRV6_SERVICE && ntohl(o->msg.vni) == vni)
			return true;
	return false;
}
bool zebra_evpn_srv6_native_if(ifindex_t index)
{
	for (struct owned *o = owned; o; o = o->next)
		if (o->msg.op == EVPN_SRV6_ES && ntohl(o->msg.sw_if_index) == (uint32_t)index)
			return true;
	return false;
}
bool zebra_evpn_srv6_oper_state(ifindex_t index, bool kernel_up)
{
	for (struct owned *o = owned; o; o = o->next)
		if (o->msg.op == EVPN_SRV6_ES && ntohl(o->msg.sw_if_index) == (uint32_t)index)
			return !!(o->msg.flags & EVPN_SRV6_ES_OPER_UP);
	return kernel_up;
}
bool zebra_evpn_srv6_active(void)
{
	return owned != NULL;
}
const struct in6_addr *zebra_evpn_srv6_sid(vni_t vni, const struct ipaddr *peer, uint8_t mode)
{
	if (!IS_IPADDR_V6(peer))
		return NULL;
	for (struct sid_entry *e = sids; e; e = e->next)
		if (e->vni == vni && e->mode == mode && !memcmp(&e->peer, &peer->ipaddr_v6, 16))
			return &e->sid;
	return NULL;
}
void zebra_evpn_srv6_local_sid(const struct srv6_sid_ctx *ctx,
                              const struct in6_addr *sid, bool add)
{
	if (!ctx->vni || !zebra_evpn_srv6_native(ctx->vni) ||
	    (ctx->behavior != ZEBRA_SEG6_LOCAL_ACTION_END_DT2U &&
	     ctx->behavior != ZEBRA_SEG6_LOCAL_ACTION_END_DT2M))
		return;
	struct evpn_srv6_msg m = {
		.magic = htonl(EVPN_SRV6_MAGIC), .version = EVPN_SRV6_VERSION,
		.op = EVPN_SRV6_LOCAL_SID, .add = add, .vni = htonl(ctx->vni),
		.type = ctx->behavior == ZEBRA_SEG6_LOCAL_ACTION_END_DT2U ? 2 : 3,
	};
	memcpy(m.key, &ctx->nh6, 16);
	memcpy(m.sid, sid, 16);
	agent_send(&m);
}
void zebra_evpn_srv6_local_sid_replay(vni_t vni)
{
	struct zebra_srv6 *srv6 = zebra_srv6_get_default();
	struct listnode *node;
	struct zebra_srv6_sid_block *block;
	struct zebra_srv6_sid_ctx *ctx;
	struct zebra_srv6_sid_entry *entry;
	if (!vni || !zebra_evpn_srv6_native(vni))
		return;
	for (ALL_LIST_ELEMENTS_RO(srv6->sid_blocks, node, block))
		frr_each (zebra_srv6_sid_ctx_list, &block->sids, ctx)
			if (ctx->ctx.vni == vni)
				frr_each (zebra_srv6_sid_entry_list, &ctx->sid->entries, entry)
					zebra_evpn_srv6_local_sid(&ctx->ctx, &entry->sid_value, true);
}
static void owned_delete(struct owned *o)
{
	vni_t vni = ntohl(o->msg.vni);
	struct zebra_evpn *zevpn = zebra_evpn_lookup(vni);
	struct interface *ifp = if_lookup_by_index(ntohl(o->msg.sw_if_index), VRF_DEFAULT);
	if (o->msg.op == EVPN_SRV6_LOCAL && zevpn) {
		struct ethaddr address;
		memcpy(&address, o->msg.key, 6);
		struct zebra_mac *mac = zebra_evpn_mac_lookup(zevpn, &address);
		if (mac && (mac->flags & ZEBRA_MAC_LOCAL))
			zebra_evpn_del_local_mac(zevpn, mac, true);
	} else if (o->msg.op == EVPN_SRV6_ES && ifp) {
		if (zebra_evpn_srv6_native_if(ifp->ifindex))
			return;
		struct zebra_if *zif = ifp->info;
		if (zif->es_info.es)
			zebra_evpn_es_if_oper_state_change(zif, false);
		zebra_evpn_es_type0_esi_update(zif, NULL);
	} else if (o->msg.op == EVPN_SRV6_SERVICE && zevpn) {
		zebra_evpn_send_del_to_client(zevpn);
	}
}
static void agent_close(void)
{
	event_cancel(&read_event);
	event_cancel(&write_event);
	if (client_fd >= 0)
		close(client_fd);
	client_fd = -1;
	pthread_mutex_lock(&update_lock);
	connected = false;
	epoch++;
	overflow = false;
	pthread_mutex_unlock(&update_lock);
	input_used = queued = 0;
	if (output)
		buffer_free(output);
	output = NULL;
	/* Learned MACs precede ES and VNI teardown. */
	for (int op = EVPN_SRV6_LOCAL; op <= EVPN_SRV6_SERVICE; op++) {
		struct owned **slot = &owned;
		while (*slot) {
			struct owned *o = *slot;
			if (o->msg.op != op) {
				slot = &o->next;
				continue;
			}
			*slot = o->next;
			owned_delete(o);
			XFREE(MTYPE_TMP, o);
		}
	}
	pthread_mutex_lock(&update_lock);
	while (updates) {
		struct update *u = updates;
		updates = u->next;
		XFREE(MTYPE_TMP, u);
	}
	update_tail = &updates;
	update_bytes = 0;
	pthread_mutex_unlock(&update_lock);
}
static void agent_write(struct event *event)
{
	buffer_status_t status = buffer_flush_available(output, client_fd);

	if (status == BUFFER_ERROR)
		agent_close();
	else if (status == BUFFER_PENDING)
		event_add_write(zrouter.master, agent_write, NULL, client_fd, &write_event);
	else
		queued = 0;
}

static void agent_send(const struct evpn_srv6_msg *message)
{
	size_t length = sizeof(*message);
	buffer_status_t status;

	if (client_fd < 0)
		return;
	if (queued + length > EVPN_SRV6_QUEUE_LIMIT) {
		zlog_warn("SRv6 EVPN control output overflow; disconnecting for resnapshot");
		agent_close();
		return;
	}
	queued += length;
	status = buffer_write(output, client_fd, message, length);
	if (status == BUFFER_ERROR)
		agent_close();
	else if (status == BUFFER_PENDING) {
		if (!write_event)
			event_add_write(zrouter.master, agent_write, NULL, client_fd, &write_event);
	} else
		queued = 0;
}

static struct evpn_srv6_msg message_new(uint8_t op, vni_t vni)
{
	struct evpn_srv6_msg message = {
		.magic = htonl(EVPN_SRV6_MAGIC),
		.version = EVPN_SRV6_VERSION,
		.op = op,
		.vni = htonl(vni),
	};
	return message;
}


static void send_sid(struct sid_entry *e)
{
	struct evpn_srv6_msg m = message_new(EVPN_SRV6_SID, e->vni);
	m.type = e->mode;
	m.add = 1;
	memcpy(m.key, &e->peer, 16);
	memcpy(m.sid, &e->sid, 16);
	agent_send(&m);
}
void zebra_evpn_srv6_sid_zapi(ZAPI_HANDLER_ARGS)
{
	vni_t vni;
	uint8_t mode;
	struct in6_addr peer, sid;
	STREAM_GETL(msg, vni);
	STREAM_GETC(msg, mode);
	STREAM_GET(&peer, msg, 16);
	STREAM_GET(&sid, msg, 16);
	if ((mode != 2 && mode != 3) || IN6_IS_ADDR_UNSPECIFIED(&sid) ||
	    IN6_IS_ADDR_MULTICAST(&sid))
		return;
	struct sid_entry *e;
	for (e = sids; e; e = e->next)
		if (e->vni == vni && e->mode == mode && !memcmp(&e->peer, &peer, 16))
			break;
	if (!e) {
		e = XCALLOC(MTYPE_TMP, sizeof(*e));
		e->vni = vni;
		e->mode = mode;
		e->peer = peer;
		e->next = sids;
		sids = e;
	}
	e->sid = sid;
	if (zebra_evpn_srv6_native(vni))
		send_sid(e);
stream_failure:
	return;
}
static void sync_mac_resync(struct hash_bucket *bucket, void *arg)
{
	struct zebra_mac *mac = bucket->data;
	if ((mac->flags & ZEBRA_MAC_LOCAL) && zebra_evpn_mac_is_static(mac))
		zebra_evpn_sync_mac_dp_install(mac, !!(mac->flags & ZEBRA_MAC_LOCAL_INACTIVE),
					       false, __func__);
}
static void resync(struct zebra_evpn *zevpn)
{
	vni_t vni = zevpn->vni;
	zebra_evpn_srv6_local_sid_replay(vni);
	for (struct sid_entry *e = sids; e; e = e->next)
		if (e->vni == vni)
			send_sid(e);
	zebra_evpn_mh_dplane_resync();
	zebra_evpn_rem_mac_install_all(zevpn);
	hash_iterate(zevpn->mac_table, sync_mac_resync, NULL);
	for (struct zebra_vtep *vtep = zevpn->vteps; vtep; vtep = vtep->next)
		zebra_evpn_vtep_install(zevpn, vtep);
}
static bool local_update(const struct evpn_srv6_msg *m)
{
	if (ntohl(m->magic) != EVPN_SRV6_MAGIC || m->version != EVPN_SRV6_VERSION || m->add > 1)
		return false;
	if (m->op != EVPN_SRV6_SERVICE && m->op != EVPN_SRV6_ES && m->op != EVPN_SRV6_LOCAL)
		return false;
	vni_t vni = ntohl(m->vni);
	struct zebra_evpn *zevpn = zebra_evpn_lookup(vni);
	struct interface *ifp = if_lookup_by_index(ntohl(m->sw_if_index), VRF_DEFAULT);
	if (!zevpn || !ifp || !zevpn->vxlan_if)
		return false;
	struct zebra_if *vxl = zevpn->vxlan_if->info;
	struct zebra_if *zif = ifp->info;
	if (m->op == EVPN_SRV6_SERVICE) {
		if (vxl->brslave_info.br_if != ifp)
			return false;
	} else {
		if (zif->brslave_info.br_if != vxl->brslave_info.br_if)
			return false;
		if (m->op == EVPN_SRV6_ES && !zebra_evpn_is_if_es_capable(zif))
			return false;
		if (m->op == EVPN_SRV6_LOCAL &&
		    ((m->key[0] & 1) || !memcmp(m->key, (uint8_t[6]){ 0 }, 6)))
			return false;
	}
	struct owned **slot = &owned;
	while (*slot && ((*slot)->msg.op != m->op || (*slot)->msg.vni != m->vni ||
			 (m->op == EVPN_SRV6_LOCAL ? memcmp((*slot)->msg.key, m->key, 6)
						   : (*slot)->msg.sw_if_index != m->sw_if_index)))
		slot = &(*slot)->next;
	struct owned *o = *slot;
	if (!m->add) {
		if (o && o->msg.sw_if_index == m->sw_if_index) {
			*slot = o->next;
			owned_delete(o);
			XFREE(MTYPE_TMP, o);
		}
		return true;
	}
	if (!o) {
		o = XCALLOC(MTYPE_TMP, sizeof(*o));
		*slot = o;
	}
	o->msg = *m;
	if (m->op == EVPN_SRV6_SERVICE) {
		zebra_evpn_send_add_to_client(zevpn);
		resync(zevpn);
	} else if (m->op == EVPN_SRV6_ES) {
		esi_t esi;
		memcpy(&esi, m->esi, 10);
		zebra_evpn_es_df_pref_update(zif, ntohs(m->df_pref));
		zebra_evpn_es_type0_esi_update(zif, &esi);
		if (zif->es_info.es)
			zebra_evpn_es_if_oper_state_change(zif,
							   !!(m->flags & EVPN_SRV6_ES_OPER_UP));
		resync(zevpn);
	} else {
		struct ethaddr address;
		memcpy(&address, m->key, 6);
		zebra_evpn_add_update_local_mac(zebra_vrf_get_evpn(), zevpn, ifp, &address, 0,
						false, false, false, NULL);
	}
	return true;
}
static void agent_read(struct event *event)
{
	/* Stream framing tolerates partial reads and coalesced frames. Limit
	 * each dispatch so MAC churn cannot starve Zebra's event loop. */
	for (unsigned int frames = 0; frames < 256; frames++) {
		ssize_t count = recv(client_fd, (char *)&input + input_used,
				     sizeof(input) - input_used, 0);
		if (count < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				break;
			agent_close();
			return;
		}
		if (!count) {
			agent_close();
			return;
		}
		input_used += count;
		if (input_used == sizeof(input)) {
			input_used = 0;
			if (!local_update(&input)) {
				agent_close();
				return;
			}
		}
	}
	event_add_read(zrouter.master, agent_read, NULL, client_fd, &read_event);
}

static void agent_accept(struct event *event)
{
	event_add_read(zrouter.master, agent_accept, NULL, listener, &accept_event);
	int fd = accept(listener, NULL, NULL);
	if (fd < 0)
		return;
	struct ucred credentials;
	socklen_t length = sizeof(credentials);
	if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &length) < 0 ||
	    (credentials.uid != 0 && credentials.uid != geteuid())) {
		close(fd);
		return;
	}
	set_nonblocking(fd);
	set_cloexec(fd);
	agent_close();
	client_fd = fd;
	output = buffer_new(0);
	pthread_mutex_lock(&update_lock);
	connected = true;
	pthread_mutex_unlock(&update_lock);
	event_add_read(zrouter.master, agent_read, NULL, client_fd, &read_event);
	struct evpn_srv6_msg m = message_new(EVPN_SRV6_BEGIN, 0);
	agent_send(&m);
	m.op = EVPN_SRV6_END;
	agent_send(&m);
}
/* Only immutable dplane contexts cross this thread boundary. */
static bool queue_update(const struct evpn_srv6_msg *m, uint32_t ctx_epoch)
{
	bool ok = false;
	pthread_mutex_lock(&update_lock);
	if (connected && epoch == ctx_epoch && update_bytes + sizeof(*m) <= EVPN_SRV6_QUEUE_LIMIT) {
		struct update *u = XCALLOC(MTYPE_TMP, sizeof(*u));
		u->msg = *m;
		*update_tail = u;
		update_tail = &u->next;
		update_bytes += sizeof(*m);
		ok = true;
	}
	if (connected && epoch == ctx_epoch && !ok)
		overflow = true;
	pthread_mutex_unlock(&update_lock);
	uint64_t one = 1;
	if (write(notify_fd, &one, sizeof(one)) < 0 && errno != EAGAIN)
		return false;
	return ok;
}
static void updates_ready(struct event *event)
{
	uint64_t count;
	while (read(notify_fd, &count, sizeof(count)) > 0) {
	}
	pthread_mutex_lock(&update_lock);
	bool close_client = overflow;
	struct update *list = updates;
	updates = NULL;
	update_tail = &updates;
	update_bytes = 0;
	pthread_mutex_unlock(&update_lock);
	if (close_client)
		agent_close();
	while (list) {
		struct update *u = list;
		list = u->next;
		if (!close_client)
			agent_send(&u->msg);
		XFREE(MTYPE_TMP, u);
	}
	event_add_read(zrouter.master, updates_ready, NULL, notify_fd, &notify_event);
}
static int provider_process(struct zebra_dplane_provider *prov)
{
	int limit = dplane_provider_get_work_limit(prov), count;
	for (count = 0; count < limit; count++) {
		struct zebra_dplane_ctx *ctx = dplane_provider_dequeue_in_ctx(prov);
		if (!ctx)
			break;
		if (!dplane_ctx_is_evpn_srv6(ctx)) {
			dplane_provider_enqueue_out_ctx(prov, ctx);
			continue;
		}
		uint32_t ctx_epoch = dplane_ctx_get_evpn_srv6_epoch(ctx);
		enum dplane_op_e op = dplane_ctx_get_op(ctx);
		struct evpn_srv6_msg m = message_new(EVPN_SRV6_ROUTE, dplane_ctx_mac_get_vni(ctx));
		bool ok = true;
		m.add = op != DPLANE_OP_MAC_DELETE && op != DPLANE_OP_VTEP_DELETE &&
			op != DPLANE_OP_NH_FDB_DELETE;
		if (op == DPLANE_OP_MAC_INSTALL || op == DPLANE_OP_MAC_DELETE) {
			m.type = 2;
			memcpy(m.key, dplane_ctx_mac_get_addr(ctx), 6);
			uint32_t nhg = dplane_ctx_mac_get_nhg_id(ctx);
			if (nhg) {
				m.flags = 2;
				m.sw_if_index = htonl(nhg);
			} else if (!(dplane_ctx_mac_get_update_flags(ctx) & DPLANE_MAC_REMOTE)) {
				m.flags = 1;
				m.sw_if_index = htonl(dplane_ctx_get_ifindex(ctx));
				if (dplane_ctx_mac_get_local_inactive(ctx))
					m.add = 0;
			} else
				memcpy(m.sid, dplane_ctx_get_evpn_srv6_sid(ctx), 16);
			ok = queue_update(&m, ctx_epoch);
		} else if (op == DPLANE_OP_VTEP_ADD || op == DPLANE_OP_VTEP_DELETE) {
			m.vni = htonl(dplane_ctx_neigh_get_vni(ctx));
			m.type = 3;
			memcpy(m.sid, dplane_ctx_get_evpn_srv6_sid(ctx), 16);
			const struct ipaddr *peer = dplane_ctx_neigh_get_ipaddr(ctx);
			if (IS_IPADDR_V6(peer))
				memcpy(m.key, &peer->ipaddr_v6, 16);
			ok = queue_update(&m, ctx_epoch);
		} else if (op == DPLANE_OP_BR_PORT_UPDATE) {
			m.op = EVPN_SRV6_DF;
			m.vni = 0;
			m.sw_if_index = htonl(dplane_ctx_get_ifindex(ctx));
			m.flags = 2;
			ok = queue_update(&m, ctx_epoch);
			const struct ipaddr *peers = dplane_ctx_get_br_port_sph_filters(ctx);
			uint32_t peer_count = dplane_ctx_get_br_port_sph_filter_cnt(ctx);
			for (uint32_t i = 0; i < peer_count; i++) {
				if (!IS_IPADDR_V6(&peers[i]))
					continue;
				m.op = EVPN_SRV6_PEER;
				m.flags = 0;
				m.add = 1;
				memcpy(m.sid, &peers[i].ipaddr_v6, 16);
				ok &= queue_update(&m, ctx_epoch);
			}
			m.op = EVPN_SRV6_DF;
			m.flags = !(dplane_ctx_get_br_port_flags(ctx) & DPLANE_BR_PORT_NON_DF);
			ok &= queue_update(&m, ctx_epoch);
		} else if (op == DPLANE_OP_NH_FDB_INSTALL || op == DPLANE_OP_NH_FDB_DELETE) {
			m.vni = 0;
			m.sw_if_index = htonl(dplane_ctx_get_fdb_nh_id(ctx));
			uint16_t n = dplane_ctx_get_fdb_nh_grp_count(ctx);
			if (!n) {
				m.op = EVPN_SRV6_NH;
				const struct ipaddr *peer = dplane_ctx_get_fdb_nh_vtep_ip(ctx);
				if (IS_IPADDR_V6(peer))
					memcpy(m.sid, &peer->ipaddr_v6, 16);
				ok = queue_update(&m, ctx_epoch);
			} else {
				m.op = EVPN_SRV6_NHG;
				m.flags = 2;
				ok = queue_update(&m, ctx_epoch);
				const struct nh_grp *members = dplane_ctx_get_fdb_nh_grp(ctx);
				for (uint16_t i = 0; i < n; i++) {
					m.flags = 1;
					m.vni = htonl(members[i].id);
					ok &= queue_update(&m, ctx_epoch);
				}
				m.vni = 0;
				m.flags = 0;
				ok &= queue_update(&m, ctx_epoch);
			}
		}
		/* FDB nexthops are shared across EVPN encapsulations. Mirror them
         * to VPP while allowing the kernel provider to serve VXLAN users. */
		if (op != DPLANE_OP_NH_FDB_INSTALL && op != DPLANE_OP_NH_FDB_DELETE)
			dplane_ctx_set_skip_kernel(ctx);
		dplane_ctx_set_status(ctx, ok ? ZEBRA_DPLANE_REQUEST_SUCCESS
					      : ZEBRA_DPLANE_REQUEST_FAILURE);
		dplane_provider_enqueue_out_ctx(prov, ctx);
	}
	if (count >= limit)
		dplane_provider_work_ready();
	return 0;
}
void zebra_evpn_srv6_init(void)
{
	struct sockaddr_un address = { .sun_family = AF_UNIX };
	strlcpy(address.sun_path + 1, EVPN_SRV6_SOCKET, sizeof(address.sun_path) - 1);
	listener = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (listener < 0 ||
	    bind(listener, (struct sockaddr *)&address,
		 offsetof(struct sockaddr_un, sun_path) + 1 + strlen(EVPN_SRV6_SOCKET)) < 0 ||
	    listen(listener, 1) < 0) {
		zlog_err("Cannot start Zebra SRv6 EVPN backend: %s", strerror(errno));
		return;
	}
	notify_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	if (notify_fd < 0) {
		close(listener);
		listener = -1;
		return;
	}
	dplane_provider_register("VPP SRv6 EVPN", DPLANE_PRIO_PRE_KERNEL, 0, NULL,
				 provider_process, NULL, NULL, NULL);
	event_add_read(zrouter.master, agent_accept, NULL, listener, &accept_event);
	event_add_read(zrouter.master, updates_ready, NULL, notify_fd, &notify_event);
}
