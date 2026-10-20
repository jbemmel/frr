# FRR + VPP + native SRv6 EVPN

The `Publish FRR VPP EVPN SRv6 image` workflow publishes
`ghcr.io/<repository-owner>/frr-vpp-evpn-srv6`. In this fork the image is
`ghcr.io/jbemmel/frr-vpp-evpn-srv6`.

Source/build changes on `master` and `feature/srv6-evpn-bridging` trigger a
build. The workflow also runs manually with selectable VPP and EVPN plugin
branches or tags. GHCR authentication uses `GITHUB_TOKEN` with
`packages: write`; no additional registry credential is needed.

Published tags:

- `master` or `feature-srv6-evpn-bridging`, depending on the triggering branch.
- `sha-<full FRR commit>` for each build.
- `latest` when building the repository's default branch.

The Dockerfile follows the combined xForm recipes
`vpp-linux-cp-srv6-route-sync.xpt` and `vpp-evpn-frr-master.xpt`: it builds
VPP's Debian packages, builds the EVPN plugin against those exact development
headers, and installs FRR into the shared runtime. FRR is built from the
workflow checkout, so the image contains the triggering commit. The VPP and
plugin refs are resolved to commit IDs before building to invalidate caches
when those dependencies change. Their default refs are:

- `jbemmel/vpp`: `feature/linux-cp-srv6-route-sync`.
- `exergy-connect/vpp-evpn-plugin`: `feature/ipv6-dual-stack`.

The build checks that FRR and VPP use identical native-control protocol
headers, starts VPP to check plugin loading and the LACP port-ID command,
and checks the FRR executable and plugin shared-library dependencies. Publish
matching protocol changes to the plugin branch before triggering an FRR build.
Recorded source identities are in `/usr/share/vpp/evpn/{frr,plugin,vpp}.commit`.

For a local build from this repository's root:

```sh
docker build -f docker/vpp-evpn-srv6/Dockerfile \
  --build-arg FRR_COMMIT="$(git rev-parse HEAD)" \
  -t frr-vpp-evpn-srv6 .
```

CI publishes `linux/amd64`. The runtime starts VPP using
`/etc/vpp/startup.conf`, as in the lab recipe; lab initialization configures
and starts FRR separately.

## Build caches

BuildKit exports all intermediate layers to branch-specific GHCR `buildcache-*`
tags. Builds import their branch cache and the default branch cache, so FRR-only
changes reuse the VPP packages and dependency-install layers. Commit build
arguments are declared only where needed; changing FRR's recorded commit does
not invalidate dependency installation or compilation.

Separate ccache mounts store VPP (2 GB), EVPN plugin (256 MB), and FRR (1 GB)
compiler results. The workflow restores these through `actions/cache` and
injects/extracts them with `buildkit-cache-dance`: layer cache exports alone do
not preserve cache-mount contents. Compiler contents, source, headers, and flags
still determine cache hits. Each compilation step prints ccache statistics.
Cache archives live outside the checkout and are saved under a new key per run.

A source edit still reruns FRR's bootstrap/configure, linking, installation, and
runtime checks, but unchanged compilations can hit ccache. The first run starts
cold. The `frr-build` target can be built independently to inspect compiler-cache
behavior without assembling the final image.
