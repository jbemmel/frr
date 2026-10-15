// SPDX-License-Identifier: GPL-2.0-or-later
/* SRv6 service metadata for the existing EVPN bgpd -> zebra path.
 * Route selection, MAC ownership and MH nexthops remain native EVPN. */
#include <zebra.h>
#include "stream.h"
#include "zclient.h"
#include "bgpd/bgpd.h"
#include "bgpd/bgp_attr.h"
#include "bgpd/bgp_attr_srv6.h"
#include "bgpd/bgp_evpn.h"
#include "bgpd/bgp_evpn_private.h"
#include "bgpd/bgp_route.h"
#include "bgpd/bgp_label.h"
#include "bgpd/bgp_zebra.h"
#include "bgpd/bgp_evpn_srv6_sid.h"

void bgp_evpn_srv6_notify(struct bgpevpn *vpn, const struct prefix_evpn *p,
			  struct bgp_path_info *pi, bool add)
{
	struct bgp_attr_srv6_l3service *service;
	uint8_t mode;
	struct in6_addr sid;
	const uint8_t *label = NULL;
	if (!add || !vpn->srv6_l2 || !pi || !bgp_zclient || bgp_zclient->sock < 0)
		return;
	service = bgp_attr_get_srv6_l3service(pi->attr);
	if (!service || service->service_type != BGP_PREFIX_SID_SRV6_L2_SERVICE ||
	    IN6_IS_ADDR_UNSPECIFIED(&service->sid) ||
	    IN6_IS_ADDR_MULTICAST(&service->sid))
		return;
	if (p->prefix.route_type == BGP_EVPN_IMET_ROUTE &&
	    (service->endpoint_behavior == SRV6_ENDPOINT_BEHAVIOR_END_DT2M ||
	     service->endpoint_behavior == SRV6_ENDPOINT_BEHAVIOR_END_DT2M_USID))
		mode = 3;
	else if ((p->prefix.route_type == BGP_EVPN_MAC_IP_ROUTE ||
		  p->prefix.route_type == BGP_EVPN_AD_ROUTE) &&
		 (service->endpoint_behavior == SRV6_ENDPOINT_BEHAVIOR_END_DT2U ||
		  service->endpoint_behavior == SRV6_ENDPOINT_BEHAVIOR_END_DT2U_USID))
		mode = 2;
	else
		return;
	if (NEXTHOP_FAMILY(pi->attr->mp_nexthop_len) != AF_INET6)
		return;
	if (mode == 3)
		label = (const uint8_t *)&pi->attr->label;
	else if (BGP_PATH_INFO_NUM_LABELS(pi))
		label = (const uint8_t *)&pi->extra->labels->label[0];
	if (!bgp_evpn_srv6_sid_decode(&sid, &service->sid, label,
				    service->loc_block_len, service->loc_node_len,
				    service->func_len, service->arg_len,
				    service->transposition_len, service->transposition_offset))
		return;
	struct stream *s = bgp_zclient->obuf;
	stream_reset(s);
	zclient_create_header(s, ZEBRA_EVPN_SRV6_SID, bgp_get_evpn()->vrf_id);
	stream_putl(s, vpn->vni);
	stream_putc(s, mode);
	stream_put(s, &pi->attr->mp_nexthop_global, 16);
	stream_put(s, &sid, 16);
	stream_putw_at(s, 0, stream_get_endp(s));
	zclient_send_message(bgp_zclient);
}

bool bgp_evpn_srv6_sids_ready(const struct bgpevpn *vpn)
{
	return !vpn->srv6_l2 || (vpn->srv6_l2_locator[0][0] && vpn->srv6_l2_locator[1][0]);
}
void bgp_evpn_srv6_local_sids(struct bgpevpn *vpn, bool add)
{
	if (!vpn->srv6_l2 || !bgp_zclient || bgp_zclient->sock < 0)
		return;
	for (int i = 0; i < 2; i++) {
		struct srv6_sid_ctx ctx = {
			.behavior = i ? ZEBRA_SEG6_LOCAL_ACTION_END_DT2M
				      : ZEBRA_SEG6_LOCAL_ACTION_END_DT2U,
			.vni = vpn->vni, .nh6 = vpn->originator_ip.ipaddr_v6,
		};
		if (add) {
			struct in6_addr *sid = i ? &vpn->srv6_dt2m_sid : &vpn->srv6_dt2u_sid;
			srv6_manager_get_sid(bgp_zclient, &ctx, sid, NULL, NULL, false);
		} else {
			srv6_manager_release_sid(bgp_zclient, &ctx, "", false);
			vpn->srv6_l2_locator[i][0] = '\0';
		}
	}
}
static void replay_local_sids(struct hash_bucket *bucket, void *arg)
{
	struct bgpevpn *vpn = bucket->data;
	if (vpn->srv6_l2) {
		memset(vpn->srv6_l2_locator, 0, sizeof(vpn->srv6_l2_locator));
		bgp_evpn_handle_rd_change(bgp_get_evpn(), vpn, 1);
		bgp_evpn_srv6_local_sids(vpn, true);
	}
}
void bgp_evpn_srv6_reconnect(void)
{
	struct bgp *bgp = bgp_get_evpn();
	if (bgp && bgp->vnihash)
		hash_iterate(bgp->vnihash, replay_local_sids, NULL);
}

struct evpn_locator_change {
	struct srv6_locator *locator;
	bool add;
};
static void local_locator_change(struct hash_bucket *bucket, void *arg)
{
	struct bgpevpn *vpn = bucket->data;
	struct evpn_locator_change *change = arg;
	struct prefix_ipv6 p = { .family = AF_INET6, .prefixlen = 128 };
	if (!vpn->srv6_l2)
		return;
	p.prefix = vpn->srv6_dt2u_sid;
	bool match = prefix_match((struct prefix *)&change->locator->prefix, (struct prefix *)&p);
	p.prefix = vpn->srv6_dt2m_sid;
	match |= prefix_match((struct prefix *)&change->locator->prefix, (struct prefix *)&p);
	if (!match)
		return;
	memset(vpn->srv6_l2_locator, 0, sizeof(vpn->srv6_l2_locator));
	bgp_evpn_handle_rd_change(bgp_get_evpn(), vpn, 1);
	if (change->add)
		bgp_evpn_srv6_local_sids(vpn, true);
}
void bgp_evpn_srv6_locator_change(struct srv6_locator *locator, bool add)
{
	struct bgp *bgp = bgp_get_evpn();
	struct evpn_locator_change change = { .locator = locator, .add = add };
	if (bgp && bgp->vnihash)
		hash_iterate(bgp->vnihash, local_locator_change, &change);
}
