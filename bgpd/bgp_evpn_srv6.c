// SPDX-License-Identifier: GPL-2.0-or-later
/* Direct native control interface to the VPP Ethernet dataplane.
 * One plugin connection per bgpd; ordered snapshot and deltas on a Unix stream.
 * A slow/disconnected consumer reconnects and receives a fresh snapshot.
 */
#include <zebra.h>
#include <sys/un.h>
#include "buffer.h"
#include "frrevent.h"
#include "hash.h"
#include "memory.h"
#include "bgpd/bgp_evpn_srv6_wire.h"
#include "log.h"
#include "network.h"
#include "prefix.h"
#include "bgpd/bgpd.h"
#include "bgpd/bgp_attr.h"
#include "bgpd/bgp_attr_srv6.h"
#include "bgpd/bgp_evpn.h"
#include "bgpd/bgp_evpn_private.h"
#include "bgpd/bgp_evpn_mh.h"
#include "bgpd/bgp_route.h"
#include "bgpd/bgp_table.h"

static int listener = -1, client = -1;
static struct event *accept_event, *read_event, *write_event;
static struct buffer *output;
static size_t queued;
static struct evpn_srv6_msg input;
static size_t input_used;
struct local_mac {
	struct local_mac *next;
	vni_t vni;
	struct ethaddr mac;
	esi_t esi;
	uint32_t sw_if_index;
};
static struct local_mac *local_macs, *pending_withdraw;
static struct event *withdraw_event;
struct local_es {
	struct local_es *next;
	vni_t vni;
	esi_t esi;
	uint32_t sw_if_index;
};
static struct local_es *local_segments, *pending_segments;

static bool segment_owned(esi_t *esi)
{
	for (struct local_es *entry = local_segments; entry; entry = entry->next)
		if (!memcmp(&entry->esi, esi, sizeof(*esi)))
			return true;
	return false;
}

static void segment_withdraw(struct local_es *entry)
{
	struct bgp *bgp = bgp_get_evpn();
	if (!bgp || !bgp_evpn_es_find(&entry->esi))
		return;
	bgp_evpn_local_es_evi_del(bgp, &entry->esi, entry->vni);
	bool pending = false;
	for (struct local_es *other = pending_segments; other; other = other->next)
		if (!memcmp(&other->esi, &entry->esi, sizeof(entry->esi)))
			pending = true;
	if (!pending && !segment_owned(&entry->esi))
		bgp_evpn_local_es_del(bgp, &entry->esi);
}

static void local_withdraw(struct local_mac *entry)
{
	struct bgp *bgp = bgp_get_evpn();
	struct bgpevpn *vpn = bgp ? bgp_evpn_lookup_vni(bgp, entry->vni) : NULL;
	struct ipaddr ip = {};

	if (vpn && vpn->srv6_l2)
		bgp_evpn_local_macip_del(bgp, entry->vni, &entry->mac, &ip,
				       ZEBRA_NEIGH_ACTIVE);
}

/* Socket errors can occur inside EVPN route callbacks. Defer withdrawals
 * until their stack has unwound; never re-enter route selection from send. */
static void withdraw_pending(struct event *event)
{
	struct local_mac *entries = pending_withdraw;
	pending_withdraw = NULL;
	while (entries) {
		struct local_mac *entry = entries;
		entries = entry->next;
		local_withdraw(entry);
		XFREE(MTYPE_TMP, entry);
	}
	/* MAC withdrawals precede removal of their parent ES/EVI references. */
	while (pending_segments) {
		struct local_es *entry = pending_segments;
		pending_segments = entry->next;
		segment_withdraw(entry);
		XFREE(MTYPE_TMP, entry);
	}
}

static void agent_close(void)
{
	event_cancel(&read_event);
	event_cancel(&write_event);
	if (client >= 0)
		close(client);
	client = -1;
	input_used = 0;
	/* Local advertisements belong to this connection, not persistent CLI
	 * configuration. Never retain stale MACs after VPP disappears. */
	while (local_macs) {
		struct local_mac *entry = local_macs;
		local_macs = entry->next;
		entry->next = pending_withdraw;
		pending_withdraw = entry;
	}
	while (local_segments) {
		struct local_es *entry = local_segments;
		local_segments = entry->next;
		entry->next = pending_segments;
		pending_segments = entry;
	}
	if ((pending_withdraw || pending_segments) && !withdraw_event)
		event_add_event(bm->master, withdraw_pending, NULL, 0, &withdraw_event);
	if (output)
		buffer_free(output);
	output = NULL;
	queued = 0;
}

static void agent_write(struct event *event)
{
	buffer_status_t status = buffer_flush_available(output, client);

	if (status == BUFFER_ERROR)
		agent_close();
	else if (status == BUFFER_PENDING)
		event_add_write(bm->master, agent_write, NULL, client, &write_event);
	else
		queued = 0;
}

static void agent_send(const struct evpn_srv6_msg *message)
{
	size_t length = sizeof(*message);
	buffer_status_t status;

	if (client < 0)
		return;
	if (queued + length > EVPN_SRV6_QUEUE_LIMIT) {
		zlog_warn("SRv6 EVPN control output overflow; disconnecting for resnapshot");
		agent_close();
		return;
	}
	queued += length;
	status = buffer_write(output, client, message, length);
	if (status == BUFFER_ERROR)
		agent_close();
	else if (status == BUFFER_PENDING) {
		if (!write_event)
			event_add_write(bm->master, agent_write, NULL, client, &write_event);
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

void bgp_evpn_srv6_notify(struct bgpevpn *vpn, const struct prefix_evpn *p,
			  struct bgp_path_info *pi, bool add)
{
	struct bgp_attr_srv6_l3service *service;
	uint8_t type = p->prefix.route_type;
	struct evpn_srv6_msg message = message_new(EVPN_SRV6_ROUTE, vpn->vni);

	if (client < 0 || !vpn->srv6_l2 ||
	    (type != BGP_EVPN_MAC_IP_ROUTE && type != BGP_EVPN_IMET_ROUTE))
		return;
	service = pi ? bgp_attr_get_srv6_l3service(pi->attr) : NULL;
	if (add && (!pi || pi->sub_type != BGP_ROUTE_IMPORTED || !service ||
		    service->service_type != BGP_PREFIX_SID_SRV6_L2_SERVICE ||
		    service->transposition_len ||
		    service->endpoint_behavior != (type == BGP_EVPN_MAC_IP_ROUTE ? 23 : 24)))
		add = false;
	message.type = type;
	message.add = add;
	if (type == BGP_EVPN_MAC_IP_ROUTE)
		memcpy(message.key, &p->prefix.macip_addr.mac, ETH_ALEN);
	else if (IS_IPADDR_V6(&p->prefix.imet_addr.ip))
		memcpy(message.key, &p->prefix.imet_addr.ip.ipaddr_v6, 16);
	else {
		message.key[10] = message.key[11] = 0xff;
		memcpy(message.key + 12, &p->prefix.imet_addr.ip.ipaddr_v4, 4);
	}
	if (add)
		memcpy(message.sid, &service->sid, sizeof(message.sid));
	agent_send(&message);
}

void bgp_evpn_srv6_local_sync(struct bgpevpn *vpn)
{
	struct evpn_srv6_msg message = message_new(EVPN_SRV6_SYNC, vpn->vni);
	agent_send(&message);
}

static void snapshot_vni(struct hash_bucket *bucket, void *arg)
{
	struct bgpevpn *vpn = bucket->data;
	struct bgp_table *tables[] = { vpn->mac_table, vpn->ip_table };
	struct bgp_dest *dest;
	struct bgp_path_info *pi;

	if (!vpn->srv6_l2 || !is_vni_live(vpn))
		return;
	for (size_t i = 0; i < array_size(tables); i++) {
		if (!tables[i])
			continue;
		for (dest = bgp_table_top(tables[i]); dest; dest = bgp_route_next(dest))
			for (pi = bgp_dest_get_bgp_path_info(dest); pi; pi = pi->next)
				if (CHECK_FLAG(pi->flags, BGP_PATH_SELECTED) &&
				    pi->sub_type == BGP_ROUTE_IMPORTED &&
				    !CHECK_FLAG(pi->flags, BGP_PATH_REMOVED))
					bgp_evpn_srv6_notify(vpn,
							     (const struct prefix_evpn *)
								     bgp_dest_get_prefix(dest),
							     pi, true);
	}
}

/* A down ES retains its association but owns no advertised local MACs. */
static void segment_mac_withdraw(vni_t vni, esi_t *esi)
{
	struct local_mac **slot = &local_macs;
	while (*slot) {
		struct local_mac *entry = *slot;
		if (entry->vni != vni || memcmp(&entry->esi, esi, sizeof(*esi))) {
			slot = &entry->next;
			continue;
		}
		*slot = entry->next;
		local_withdraw(entry);
		XFREE(MTYPE_TMP, entry);
		if (client < 0)
			return;
	}
}

static bool segment_update(const struct evpn_srv6_msg *message)
{
	struct bgp *bgp = bgp_get_evpn();
	vni_t vni = ntohl(message->vni);
	struct bgpevpn *vpn = bgp ? bgp_evpn_lookup_vni(bgp, vni) : NULL;
	esi_t esi;
	struct ipaddr originator = {};
	struct local_es **slot = &local_segments, *entry;
	uint32_t sw_if_index = ntohl(message->sw_if_index);
	memcpy(&esi, message->esi, sizeof(esi));
	if (message->type != BGP_EVPN_ES_ROUTE ||
	    !memcmp(&esi, zero_esi, sizeof(esi)) ||
	    (message->flags & ~EVPN_SRV6_ES_OPER_UP))
		return false;
	if (!vpn || !vpn->srv6_l2 || !is_vni_live(vpn))
		return true;
	while (*slot && ((*slot)->vni != vni || memcmp(&(*slot)->esi, &esi, sizeof(esi))))
		slot = &(*slot)->next;
	entry = *slot;
	if (entry && entry->sw_if_index != sw_if_index)
		return false; /* One attachment per ESI/VNI on this client. */
	if (!message->add || !(message->flags & EVPN_SRV6_ES_OPER_UP)) {
		segment_mac_withdraw(vni, &esi);
		if (client < 0)
			return false;
	}
	if (!message->add) {
		if (entry) {
			*slot = entry->next;
			segment_withdraw(entry);
			XFREE(MTYPE_TMP, entry);
		}
		return client >= 0;
	}
	struct in6_addr source;
	memcpy(&source, message->sid, sizeof(source));
	if (IN6_IS_ADDR_UNSPECIFIED(&source) || IN6_IS_ADDR_MULTICAST(&source) ||
	    !IPV6_ADDR_SAME(&source, &vpn->originator_ip.ipaddr_v6))
		return false;
	struct bgp_evpn_es *existing = bgp_evpn_es_find(&esi);
	if (!segment_owned(&esi) && existing && CHECK_FLAG(existing->flags, BGP_EVPNES_LOCAL))
		return false; /* Never take ownership from zebra's Linux dataplane. */
	if (!entry) {
		entry = XCALLOC(MTYPE_TMP, sizeof(*entry));
		entry->vni = vni;
		entry->esi = esi;
		entry->sw_if_index = sw_if_index;
		*slot = entry;
	}
	ipaddr_set_v6(&originator, &source);
	if (bgp_evpn_local_es_add(bgp, &esi, originator,
				  !!(message->flags & EVPN_SRV6_ES_OPER_UP),
				  ntohs(message->df_pref), false) ||
	    bgp_evpn_local_es_evi_add(bgp, &esi, vni))
		return false;
	return client >= 0;
}

static bool local_update(const struct evpn_srv6_msg *message)
{
	struct bgp *bgp = bgp_get_evpn();
	vni_t vni = ntohl(message->vni);
	struct bgpevpn *vpn = bgp ? bgp_evpn_lookup_vni(bgp, vni) : NULL;
	struct local_mac **slot, *entry;
	struct ethaddr mac;
	struct ipaddr ip = {};
	esi_t esi;

	if (ntohl(message->magic) != EVPN_SRV6_MAGIC ||
	    message->version != EVPN_SRV6_VERSION ||
	    message->add > 1)
		return false;
	if (message->op == EVPN_SRV6_ES)
		return segment_update(message);
	if (message->op != EVPN_SRV6_LOCAL || message->flags ||
	    message->type != BGP_EVPN_MAC_IP_ROUTE ||
	    (message->key[0] & 1) ||
	    !memcmp(message->key, "\0\0\0\0\0\0", ETH_ALEN))
		return false;
	if (!vpn || !vpn->srv6_l2 || !is_vni_live(vpn))
		return true; /* VNI configuration can race local events. SYNC replays. */
	memcpy(&mac, message->key, ETH_ALEN);
	memcpy(&esi, message->esi, sizeof(esi));
	if (message->add && memcmp(&esi, zero_esi, sizeof(esi))) {
		struct local_es *segment;
		for (segment = local_segments; segment; segment = segment->next)
			if (segment->vni == vni && !memcmp(&segment->esi, &esi, sizeof(esi)))
				break;
		struct bgp_evpn_es *es = bgp_evpn_es_find(&esi);
		if (!segment || !es || !bgp_evpn_local_es_is_active(es))
			return false; /* ES registration/up precedes its MAC adds. */
	}
	/* Register ownership before invoking route callbacks: a slow-client
	 * disconnect may withdraw and free this list during the add. */
	slot = &local_macs;
	while (*slot && ((*slot)->vni != vni || memcmp(&(*slot)->mac, &mac, ETH_ALEN)))
		slot = &(*slot)->next;
	entry = *slot;
	if (message->add) {
		if (!entry) {
			entry = XCALLOC(MTYPE_TMP, sizeof(*entry));
			entry->vni = vni;
			entry->mac = mac;
			*slot = entry;
		}
		entry->esi = esi;
		entry->sw_if_index = ntohl(message->sw_if_index);
		if (bgp_evpn_local_macip_add(bgp, vni, &mac, &ip, 0, 0, &esi))
			return false;
	} else if (entry && entry->sw_if_index == ntohl(message->sw_if_index) &&
		   !memcmp(&entry->esi, &esi, sizeof(esi))) {
		*slot = entry->next;
		local_withdraw(entry);
		XFREE(MTYPE_TMP, entry);
	}
	return client >= 0;
}

static void agent_read(struct event *event)
{
	/* Stream framing tolerates partial reads and coalesced frames. Limit
	 * each dispatch so MAC churn cannot starve BGP's event loop. */
	for (unsigned int frames = 0; frames < 256; frames++) {
		ssize_t count = recv(client, (char *)&input + input_used,
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
	event_add_read(bm->master, agent_read, NULL, client, &read_event);
}

static void agent_accept(struct event *event)
{
	int fd;
	struct bgp *bgp;

	event_add_read(bm->master, agent_accept, NULL, listener, &accept_event);
	fd = accept(listener, NULL, NULL);
	if (fd < 0)
		return;
	#ifdef SO_PEERCRED
	struct ucred credentials;
	socklen_t credentials_len = sizeof(credentials);
	if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &credentials_len) < 0 ||
	    (credentials.uid != 0 && credentials.uid != geteuid())) {
		close(fd);
		return;
	}
#endif
	set_nonblocking(fd);
	set_cloexec(fd);
	agent_close();
	/* Clear previous-client advertisements before a new snapshot/replay. */
	event_cancel(&withdraw_event);
	withdraw_pending(NULL);
	client = fd;
	output = buffer_new(0);
	event_add_read(bm->master, agent_read, NULL, client, &read_event);
	/* Snapshot and subscription execute atomically in bgpd's event loop. */
	struct evpn_srv6_msg message = message_new(EVPN_SRV6_BEGIN, 0);
	agent_send(&message);
	bgp = bgp_get_evpn();
	if (bgp)
		hash_iterate(bgp->vnihash, snapshot_vni, NULL);
	message.op = EVPN_SRV6_END;
	agent_send(&message);
}

int bgp_evpn_srv6_agent_start(void)
{
	struct sockaddr_un address = { .sun_family = AF_UNIX };

	if (listener >= 0)
		return 0;
	/* Linux abstract sockets are isolated by network namespace. Authenticate
	 * the local VPP plugin using SO_PEERCRED rather than filesystem permissions.
	 * This also works with the stock FRR AppArmor policy's network rule.
	 */
	strlcpy(address.sun_path + 1, EVPN_SRV6_SOCKET, sizeof(address.sun_path) - 1);
	listener = socket(AF_UNIX, SOCK_STREAM, 0);
	if (listener < 0)
		return -1;
	set_cloexec(listener);
	if (bind(listener, (struct sockaddr *)&address,
		 offsetof(struct sockaddr_un, sun_path) + 1 + strlen(EVPN_SRV6_SOCKET)) < 0 ||
	    listen(listener, 1) < 0) {
		close(listener);
		listener = -1;
		return -1;
	}
	set_nonblocking(listener);
	event_add_read(bm->master, agent_accept, NULL, listener, &accept_event);
	return 0;
}
