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
#include "bgpd/bgp_zebra.h"

void bgp_evpn_srv6_notify(struct bgpevpn *vpn, const struct prefix_evpn *p,
			  struct bgp_path_info *pi, bool add)
{
	struct bgp_attr_srv6_l3service *service;
	uint8_t mode;
	if (!add || !vpn->srv6_l2 || !pi || !bgp_zclient || bgp_zclient->sock < 0)
		return;
	service = bgp_attr_get_srv6_l3service(pi->attr);
	if (!service || service->service_type != BGP_PREFIX_SID_SRV6_L2_SERVICE ||
	    service->transposition_len || IN6_IS_ADDR_UNSPECIFIED(&service->sid) ||
	    IN6_IS_ADDR_MULTICAST(&service->sid))
		return;
	if (p->prefix.route_type == BGP_EVPN_IMET_ROUTE && service->endpoint_behavior == 24)
		mode = 3;
	else if ((p->prefix.route_type == BGP_EVPN_MAC_IP_ROUTE ||
		  p->prefix.route_type == BGP_EVPN_AD_ROUTE) &&
		 service->endpoint_behavior == 23)
		mode = 2;
	else
		return;
	if (NEXTHOP_FAMILY(pi->attr->mp_nexthop_len) != AF_INET6)
		return;
	struct stream *s = bgp_zclient->obuf;
	stream_reset(s);
	zclient_create_header(s, ZEBRA_EVPN_SRV6_SID, bgp_get_evpn()->vrf_id);
	stream_putl(s, vpn->vni);
	stream_putc(s, mode);
	stream_put(s, &pi->attr->mp_nexthop_global, 16);
	stream_put(s, &service->sid, 16);
	stream_putw_at(s, 0, stream_get_endp(s));
	zclient_send_message(bgp_zclient);
}
