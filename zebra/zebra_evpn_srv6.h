// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef ZEBRA_EVPN_SRV6_H
#define ZEBRA_EVPN_SRV6_H
#include "zebra/zserv.h"
#include "zebra/zebra_dplane.h"
void zebra_evpn_srv6_init(void);
void zebra_evpn_srv6_sid_zapi(ZAPI_HANDLER_ARGS);
void zebra_evpn_srv6_local_sid(const struct srv6_sid_ctx *ctx,
                              const struct in6_addr *sid, bool add);
void zebra_evpn_srv6_local_sid_replay(vni_t vni);
uint8_t zebra_evpn_transport(vni_t vni);
bool zebra_evpn_srv6_native(vni_t vni);
bool zebra_evpn_srv6_native_if(ifindex_t index);
bool zebra_evpn_srv6_active(void);
bool zebra_evpn_srv6_oper_state(ifindex_t index, bool kernel_up);
uint32_t zebra_evpn_srv6_epoch(void);
const struct in6_addr *zebra_evpn_srv6_sid(vni_t vni, const struct ipaddr *peer, uint8_t mode);
void zebra_evpn_mh_dplane_resync(void);
#endif
