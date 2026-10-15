# SPDX-License-Identifier: GPL-2.0-or-later
import frrtest


class TestEvpnSrv6Sid(frrtest.TestMultiOut):
    program = "./test_evpn_srv6_sid"


TestEvpnSrv6Sid.onesimple("EVPN SRv6 SID decoding passed")
