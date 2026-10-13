/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <assert.h>
#include <arpa/inet.h>
#include <string.h>
#include <stdio.h>
#include "bgpd/bgp_evpn_srv6_sid.h"

int main(void)
{
	struct in6_addr base, sid, expected;
	uint8_t label[] = {0xe0, 0x02, 0x00};
	inet_pton(AF_INET6, "5f00:0:3::", &base);
	inet_pton(AF_INET6, "5f00:0:3:e002::", &expected);
	assert(bgp_evpn_srv6_sid_decode(&sid, &base, label, 32, 16, 16, 16, 16, 48));
	assert(!memcmp(&sid, &expected, sizeof(sid)));
	label[1] = 1;
	inet_pton(AF_INET6, "5f00:0:3:e001::", &expected);
	assert(bgp_evpn_srv6_sid_decode(&sid, &base, label, 32, 16, 16, 0, 16, 48));
	assert(!memcmp(&sid, &expected, sizeof(sid)));
	/* Entire 24-bit EVPN label, and a transposition crossing byte boundaries. */
	label[0] = 0xab; label[1] = 0xcd; label[2] = 0xef;
	inet_pton(AF_INET6, "5f00:0:3:abcd:ef00::", &expected);
	assert(bgp_evpn_srv6_sid_decode(&sid, &base, label, 32, 16, 24, 0, 24, 48));
	assert(!memcmp(&sid, &expected, sizeof(sid)));
	inet_pton(AF_INET6, "5f00:0:3:1579:a000::", &expected);
	assert(bgp_evpn_srv6_sid_decode(&sid, &base, label, 32, 16, 32, 0, 16, 51));
	assert(!memcmp(&sid, &expected, sizeof(sid)));
	assert(bgp_evpn_srv6_sid_decode(&sid, &expected, NULL, 32, 16, 32, 16, 0, 0));
	assert(!memcmp(&sid, &expected, sizeof(sid)));
	assert(!bgp_evpn_srv6_sid_decode(&sid, &base, NULL, 32, 16, 16, 16, 16, 48));
	assert(!bgp_evpn_srv6_sid_decode(&sid, &base, label, 32, 16, 16, 16, 17, 48));
	assert(!bgp_evpn_srv6_sid_decode(&sid, &base, label, 32, 16, 32, 0, 25, 48));
	assert(!bgp_evpn_srv6_sid_decode(&sid, &base, label, 32, 16, 16, 16, 16, 47));
	assert(!bgp_evpn_srv6_sid_decode(&sid, &base, label, 32, 16, 16, 16, 16, 49));
	assert(!bgp_evpn_srv6_sid_decode(&sid, &base, label, 64, 64, 16, 16, 0, 0));
	memset(&base, 0, sizeof(base));
	assert(!bgp_evpn_srv6_sid_decode(&sid, &base, NULL, 32, 16, 16, 16, 0, 0));
	puts("EVPN SRv6 SID decoding passed");
	return 0;
}
