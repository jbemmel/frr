// SPDX-License-Identifier: GPL-2.0-or-later
/* Selected native SRv6 MAC-VRF routes for an external Ethernet dataplane.
 * One local agent per bgpd; ordered snapshot and deltas on a Unix stream.
 * A slow/disconnected consumer reconnects and receives a fresh snapshot.
 */
#include <zebra.h>
#include <sys/un.h>
#include "buffer.h"
#include "frrevent.h"
#include "hash.h"
#include "json.h"
#include "log.h"
#include "network.h"
#include "prefix.h"
#include "bgpd/bgpd.h"
#include "bgpd/bgp_attr.h"
#include "bgpd/bgp_attr_srv6.h"
#include "bgpd/bgp_evpn.h"
#include "bgpd/bgp_evpn_private.h"
#include "bgpd/bgp_route.h"
#include "bgpd/bgp_table.h"

#define SRV6_AGENT_NAME "frr-evpn-srv6"
#define SRV6_AGENT_QUEUE_LIMIT (1024 * 1024)
static int listener = -1, client = -1;
static struct event *accept_event, *read_event, *write_event;
static struct buffer *output;
static size_t queued;

static void agent_close(void)
{
	event_cancel(&read_event);
	event_cancel(&write_event);
	if (client >= 0)
		close(client);
	client = -1;
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

static void agent_send(const char *line)
{
	size_t length = strlen(line);
	buffer_status_t status;

	if (client < 0)
		return;
	if (queued + length > SRV6_AGENT_QUEUE_LIMIT) {
		zlog_warn("SRv6 EVPN agent output overflow; disconnecting for resnapshot");
		agent_close();
		return;
	}
	queued += length;
	status = buffer_write(output, client, line, length);
	if (status == BUFFER_ERROR)
		agent_close();
	else if (status == BUFFER_PENDING) {
		if (!write_event)
			event_add_write(bm->master, agent_write, NULL, client, &write_event);
	} else
		queued = 0;
}

void bgp_evpn_srv6_notify(struct bgpevpn *vpn, const struct prefix_evpn *p,
			  struct bgp_path_info *pi, bool add)
{
	struct bgp_attr_srv6_l3service *service;
	char key[INET6_ADDRSTRLEN], sid[INET6_ADDRSTRLEN], line[256];
	uint8_t type = p->prefix.route_type;

	if (client < 0 || !vpn->srv6_l2 ||
	    (type != BGP_EVPN_MAC_IP_ROUTE && type != BGP_EVPN_IMET_ROUTE))
		return;
	/* Native single-homed ELAN: only remote selected full-SID paths. */
	service = pi ? bgp_attr_get_srv6_l3service(pi->attr) : NULL;
	if (add && (!pi || pi->sub_type != BGP_ROUTE_IMPORTED || !service ||
		    service->service_type != BGP_PREFIX_SID_SRV6_L2_SERVICE ||
		    service->transposition_len ||
		    service->endpoint_behavior != (type == BGP_EVPN_MAC_IP_ROUTE ? 23 : 24)))
		add = false;
	if (type == BGP_EVPN_MAC_IP_ROUTE)
		prefix_mac2str(&p->prefix.macip_addr.mac, key, sizeof(key));
	else
		ipaddr2str(&p->prefix.imet_addr.ip, key, sizeof(key));
	if (add)
		inet_ntop(AF_INET6, &service->sid, sid, sizeof(sid));
	else
		sid[0] = '\0';
	snprintf(line, sizeof(line),
		 "{\"event\":\"route\",\"vni\":%u,\"type\":%u,\"key\":\"%s\",\"sid\":\"%s\",\"add\":%s}\n",
		 vpn->vni, type, key, sid, add ? "true" : "false");
	agent_send(line);
}

/* Reconfiguration may clear FRR's local MAC routes while VPP retains its
 * learned FDB. Request an event replay after the MAC-VRF becomes live.
 */
void bgp_evpn_srv6_local_sync(struct bgpevpn *vpn)
{
	char line[96];

	snprintf(line, sizeof(line),
		 "{\"event\":\"local-sync\",\"vni\":%u}\n", vpn->vni);
	agent_send(line);
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

static void agent_read(struct event *event)
{
	char byte;
	ssize_t count = read(client, &byte, 1);

	/* Output-only socket. Reads detect EOF; unexpected input is rejected. */
	if (count == 0 || count > 0 || (count < 0 && errno != EAGAIN && errno != EINTR))
		agent_close();
	else
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
	client = fd;
	output = buffer_new(0);
	event_add_read(bm->master, agent_read, NULL, client, &read_event);
	/* Snapshot and subscription execute atomically in bgpd's event loop. */
	agent_send("{\"event\":\"begin\"}\n");
	bgp = bgp_get_evpn();
	if (bgp)
		hash_iterate(bgp->vnihash, snapshot_vni, NULL);
	agent_send("{\"event\":\"end\"}\n");
}

int bgp_evpn_srv6_agent_start(void)
{
	struct sockaddr_un address = { .sun_family = AF_UNIX };

	if (listener >= 0)
		return 0;
	/* Linux abstract sockets are isolated by network namespace. Authenticate
	 * the local agent using SO_PEERCRED rather than filesystem permissions.
	 * This also works with the stock FRR AppArmor policy's network rule.
	 */
	strlcpy(address.sun_path + 1, SRV6_AGENT_NAME, sizeof(address.sun_path) - 1);
	listener = socket(AF_UNIX, SOCK_STREAM, 0);
	if (listener < 0)
		return -1;
	set_cloexec(listener);
	if (bind(listener, (struct sockaddr *)&address,
		 offsetof(struct sockaddr_un, sun_path) + 1 + strlen(SRV6_AGENT_NAME)) < 0 ||
	    listen(listener, 1) < 0) {
		close(listener);
		listener = -1;
		return -1;
	}
	set_nonblocking(listener);
	event_add_read(bm->master, agent_accept, NULL, listener, &accept_event);
	return 0;
}
