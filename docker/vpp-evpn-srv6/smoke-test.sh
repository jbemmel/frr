#!/bin/sh
# Run VPP under GDB so startup crashes include a backtrace in the build log.
set -eu
rm -rf /tmp/evpn-smoke
mkdir -p /tmp/evpn-smoke/vat
printf '%s\n' \
    'unix { nodaemon nosyslog runtime-dir /tmp/evpn-smoke cli-listen /tmp/evpn-smoke/cli.sock pidfile /tmp/evpn-smoke/vpp.pid }' \
    'memory { main-heap-page-size 4k }' \
    'buffers { buffers-per-numa 1024 page-size 4k }' \
    'plugins { vat-path /tmp/evpn-smoke/vat plugin default { disable } plugin evpn_plugin.so { enable } plugin lacp_plugin.so { enable } }' \
    > /tmp/evpn-smoke/startup.conf

gdb --batch --return-child-result \
    -ex 'set pagination off' \
    -ex 'handle SIGTERM nostop noprint pass' \
    -ex run -ex 'thread apply all bt full' \
    --args vpp -c /tmp/evpn-smoke/startup.conf \
    > /tmp/evpn-smoke/vpp.log 2>&1 &
debugger_pid=$!
cleanup() {
    if [ -s /tmp/evpn-smoke/vpp.pid ]; then
        kill "$(cat /tmp/evpn-smoke/vpp.pid)" 2>/dev/null || :
    fi
    wait "$debugger_pid" 2>/dev/null || :
    cat /tmp/evpn-smoke/vpp.log
}
trap cleanup EXIT
# Stop immediately if the inferior crashes, instead of hiding it behind a
# readiness timeout. GDB has printed the backtrace before it exits.
timeout 30 sh -c '
    until vppctl -s /tmp/evpn-smoke/cli.sock show version >/dev/null 2>&1; do
        kill -0 "$1" 2>/dev/null || exit 1
        sleep 0.1
    done
' sh "$debugger_pid"
vppctl -s /tmp/evpn-smoke/cli.sock show plugins > /tmp/evpn-smoke/plugins
cat /tmp/evpn-smoke/plugins
grep -q 'evpn_plugin.so' /tmp/evpn-smoke/plugins
vppctl -s /tmp/evpn-smoke/cli.sock help set interface lacp port-id > /tmp/evpn-smoke/lacp-port-id
grep -q 'set interface lacp port-id' /tmp/evpn-smoke/lacp-port-id
cleanup
trap - EXIT
rm -rf /tmp/evpn-smoke
