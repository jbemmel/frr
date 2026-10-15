/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef BGP_EVPN_SRV6_SID_H
#define BGP_EVPN_SRV6_SID_H

#include <stdbool.h>
#include <stdint.h>
#include <netinet/in.h>

/* EVPN label fields carry 24 raw bits, unlike the 20-bit L3VPN label.
 * RFC 9252: transpose the high TL bits into the advertised function. */
static inline bool bgp_evpn_srv6_sid_decode(struct in6_addr *out,
					 const struct in6_addr *base,
					 const uint8_t *label, unsigned int block,
					 unsigned int node, unsigned int func,
					 unsigned int arg, unsigned int len,
					 unsigned int offset)
{
	unsigned int start = block + node;
	if (start + func + arg > 128 || len > 24 || len > func ||
	    (len && (!label || offset < start || offset + len > start + func)))
		return false;
	*out = *base;
	for (unsigned int i = 0; i < len; i++) {
		unsigned int bit = offset + i;
		uint8_t mask = 1U << (7 - bit % 8);
		out->s6_addr[bit / 8] &= ~mask;
		if (label[i / 8] & (1U << (7 - i % 8)))
			out->s6_addr[bit / 8] |= mask;
	}
	return !IN6_IS_ADDR_UNSPECIFIED(out) && !IN6_IS_ADDR_MULTICAST(out);
}

#endif
