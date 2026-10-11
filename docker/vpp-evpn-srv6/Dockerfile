# syntax=docker/dockerfile:1
# VPP — jbemmel/vpp feature/linux-cp-srv6-route-sync, plus bird3 and frr.
# Runtime matches templates/docker_image/vpp.xpt (Debian bookworm).

ARG VPP_REF=feature/linux-cp-srv6-route-sync
ARG VPP_REPO=https://github.com/jbemmel/vpp.git
ARG VPP_COMMIT=

FROM debian:bookworm AS vpp-build
ENV DEBIAN_FRONTEND=noninteractive
# Shallow clone has no release tags. scripts/version and build/external/Makefile
# both shell out to git describe; an empty version fails CMake configure.
ENV PKG_VERSION=25.10 \
	PKG_SUFFIX=0 \
	SOURCE_DATE_EPOCH=0
ARG VPP_REF
ARG VPP_REPO
ARG VPP_COMMIT
RUN apt-get update && apt-get install -y --no-install-recommends \
		bash \
		ca-certificates \
		git \
		make \
		sudo \
	&& rm -rf /var/lib/apt/lists/*
WORKDIR /src
RUN git init . \
	&& git remote add origin "$VPP_REPO" \
	&& git fetch --depth 1 origin "${VPP_COMMIT:-$VPP_REF}" \
	&& git checkout --detach FETCH_HEAD \
	&& git -c user.email=build@localhost -c user.name=vpp-build \
		tag -a v25.10 -m 25.10
# debian-12 install-dep asks for clang-format-19, which bookworm may not ship.
# Checkstyle is not used for this image.
# Bookworm nasm is 2.16.01; intel-ipsec-mb needs >= 2.16.02 for SM3/SM4/SHA512-NI.
RUN apt-get update \
	&& if ! apt-cache show clang-format-19 >/dev/null 2>&1; then \
		sed -i 's/clang-format-19/clang-format-14/' Makefile; \
	fi \
	&& make UNATTENDED=y install-dep \
	&& curl -fsSL -o /tmp/nasm.tar.gz https://www.nasm.us/pub/nasm/releasebuilds/2.16.03/nasm-2.16.03.tar.gz \
	&& tar -C /tmp -xzf /tmp/nasm.tar.gz \
	&& cd /tmp/nasm-2.16.03 \
	&& ./configure --prefix=/usr/local \
	&& make -j"$(nproc)" \
	&& make install \
	&& cd /src \
	&& nasm -v \
	&& rm -rf /tmp/nasm-2.16.03 /tmp/nasm.tar.gz \
	&& rm -f build/external/deb/debian/changelog \
	&& make install-ext-deps \
	&& make build-release \
	&& make pkg-deb \
	&& mkdir -p /out \
	&& find . -maxdepth 2 -name '*.deb' ! -name 'vpp-dbg*' ! -name 'vom*' -exec cp {} /out/ \; \
	&& test -n "$(ls /out/vpp_*.deb)" \
	&& git rev-parse HEAD > /out/vpp.commit \
	&& rm -rf /var/lib/apt/lists/*

FROM debian:bookworm-slim AS vpp-base
ENV DEBIAN_FRONTEND=noninteractive
LABEL description="FD.io VPP (jbemmel/vpp feature/linux-cp-srv6-route-sync)"

RUN apt-get update && apt-get install -y --no-install-recommends \
		apt-transport-https \
		ca-certificates \
		curl \
		gnupg \
		iproute2 \
		procps \
	&& rm -rf /var/lib/apt/lists/*

COPY --from=vpp-build /out/ /tmp/vpp-debs/
RUN set -eux; \
	sysctl_bin="$(command -v sysctl)"; \
	mv "$sysctl_bin" "${sysctl_bin}.real"; \
	printf '#!/bin/sh\nexit 0\n' > "$sysctl_bin"; \
	chmod +x "$sysctl_bin"; \
	apt-get update; \
	apt-get install -y -V /tmp/vpp-debs/*.deb; \
	mv "${sysctl_bin}.real" "$sysctl_bin"; \
	mkdir -p /vpp; \
	dpkg-query -f '${Version}\n' -W vpp > /vpp/version; \
	mkdir -p /usr/share/vpp/evpn; \
	cp /tmp/vpp-debs/vpp.commit /usr/share/vpp/evpn/vpp.commit; \
	rm -rf /tmp/vpp-debs; \
	rm -rf /var/lib/apt/lists/*;

RUN set -eux; \
	apt-get update; \
	apt-get install -y --no-install-recommends \
		curl \
		gnupg \
		wget; \
	wget -O /usr/share/keyrings/cznic-labs-pkg.gpg https://pkg.labs.nic.cz/gpg; \
	echo "deb [signed-by=/usr/share/keyrings/cznic-labs-pkg.gpg] https://pkg.labs.nic.cz/bird3 bookworm main" \
		> /etc/apt/sources.list.d/cznic-labs-bird3.list; \
	curl -fsSL https://deb.frrouting.org/frr/keys.asc | gpg --dearmor -o /etc/apt/trusted.gpg.d/frr.gpg; \
	echo "deb https://deb.frrouting.org/frr bookworm frr-stable" > /etc/apt/sources.list.d/frr.list; \
	apt-get update; \
	apt-get install -y --no-install-recommends bird3 frr frr-pythontools; \
	mkdir -p /etc/bird /run/bird /var/log/frr /var/run/frr /var/log/vpp; \
	rm -rf /var/lib/apt/lists/*;

CMD ["/usr/bin/vpp", "-c", "/etc/vpp/startup.conf"]

FROM vpp-base AS evpn-plugin
ARG EVPN_REPO=https://github.com/exergy-connect/vpp-evpn-plugin.git
ARG EVPN_REF=feature/ipv6-dual-stack
ARG EVPN_COMMIT=
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake git ca-certificates python3 python3-ply \
    && rm -rf /var/lib/apt/lists/*
# vpp-base installed /out/*.deb from vpp-build, including its development packages.
# Never install headers or libraries from a separate VPP package repository.
RUN mkdir -p /src/evpn && cd /src/evpn \
    && git init . && git remote add origin "$EVPN_REPO" \
    && git fetch --depth 1 origin "${EVPN_COMMIT:-$EVPN_REF}" \
    && git checkout --detach FETCH_HEAD
RUN cmake -S /src/evpn -B /src/evpn/build \
    -DCMAKE_BUILD_TYPE=Release -DVPP_EXTERNAL_PROJECT=ON \
    -DVPP_INSTALL_PATH=/usr -DCMAKE_INSTALL_PREFIX=/usr \
    && cmake --build /src/evpn/build -j4 \
    && DESTDIR=/runtime cmake --install /src/evpn/build \
    && test -s /runtime/usr/lib/x86_64-linux-gnu/vpp_plugins/evpn_plugin.so \
    && mkdir -p /runtime/usr/share/vpp/evpn \
    && git -C /src/evpn rev-parse HEAD > /runtime/usr/share/vpp/evpn/plugin.commit

FROM vpp-base AS build
ARG FRR_COMMIT=working-tree
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential git ca-certificates autoconf automake \
    libtool pkg-config bison flex libjson-c-dev libc-ares-dev libpcre2-dev \
    libreadline-dev libyang-dev python3-dev libcap-dev libelf-dev \
    protobuf-c-compiler libprotobuf-c-dev && rm -rf /var/lib/apt/lists/*
# Build the workflow checkout, including exactly the triggering FRR commit.
COPY . /src/frr
WORKDIR /src/frr
RUN ./bootstrap.sh && ./configure --prefix=/usr --sysconfdir=/etc \
    --localstatedir=/var --sbindir=/usr/lib/frr --libdir=/usr/lib/x86_64-linux-gnu \
    --enable-user=frr --enable-group=frr --enable-vty-group=frrvty \
    --enable-vtysh --enable-multipath=64 --disable-doc \
    && make -j4 && make DESTDIR=/runtime install \
    && mkdir -p /runtime/usr/share/vpp/evpn \
       /runtime/usr/lib/x86_64-linux-gnu/vpp_plugins \
    && printf '%s\n' "$FRR_COMMIT" > /runtime/usr/share/vpp/evpn/frr.commit
COPY --from=evpn-plugin /runtime/ /runtime/
COPY --from=evpn-plugin /src/evpn/evpn_srv6_wire.h /tmp/evpn_srv6_wire.h
RUN cmp /src/frr/zebra/zebra_evpn_srv6_wire.h /tmp/evpn_srv6_wire.h

FROM vpp-base
ARG FRR_COMMIT=working-tree
ARG EVPN_REF=feature/ipv6-dual-stack
ENV DEBIAN_FRONTEND=noninteractive
LABEL org.opencontainers.image.revision="$FRR_COMMIT" \
      description="VPP with a source-matched EVPN plugin and jbemmel/frr SRv6 EVPN branch" \
      org.opencontainers.image.source="https://github.com/jbemmel/frr" \
      io.xform.frr.commit="$FRR_COMMIT" \
      io.xform.evpn.source="https://github.com/exergy-connect/vpp-evpn-plugin" \
      io.xform.evpn.ref="$EVPN_REF"
RUN apt-get update && apt-get install -y --no-install-recommends \
    libyang3 libprotobuf-c1 libcap2 libelf1 libreadline8 libc-ares2 libjson-c5 \
    && rm -rf /var/lib/apt/lists/*
COPY --from=build /runtime/ /
RUN mkdir -p /etc/frr /run/frr /var/log/frr /run/vpp \
    && chown -R frr:frr /etc/frr /run/frr /var/log/frr \
    && ldconfig \
    && /usr/lib/frr/bgpd --version \
    && ldd /usr/lib/x86_64-linux-gnu/vpp_plugins/evpn_plugin.so > /tmp/evpn-ldd \
    && cat /tmp/evpn-ldd \
    && if grep -q 'not found' /tmp/evpn-ldd; then exit 1; fi
# Loading the plugin also catches unresolved VPP symbols that plain ldd misses.
RUN set -eu; \
    rm -rf /tmp/evpn-smoke; \
    mkdir -p /tmp/evpn-smoke/vat; \
    printf '%s\n' \
      'unix { nodaemon nosyslog cli-listen /tmp/evpn-smoke/cli.sock }' \
      'memory { main-heap-page-size 4k }' \
      'buffers { buffers-per-numa 1024 page-size 4k }' \
      'plugins { vat-path /tmp/evpn-smoke/vat plugin default { disable } plugin evpn_plugin.so { enable } plugin lacp_plugin.so { enable } }' \
      > /tmp/evpn-smoke/startup.conf; \
    vpp -c /tmp/evpn-smoke/startup.conf > /tmp/evpn-smoke/vpp.log 2>&1 & \
    vpp_pid=$!; \
    trap 'kill "$vpp_pid" 2>/dev/null || :; wait "$vpp_pid" 2>/dev/null || :; cat /tmp/evpn-smoke/vpp.log' EXIT; \
    timeout 30 sh -c 'until vppctl -s /tmp/evpn-smoke/cli.sock show version >/dev/null 2>&1; do sleep 0.1; done'; \
    vppctl -s /tmp/evpn-smoke/cli.sock show plugins > /tmp/evpn-smoke/plugins; \
    cat /tmp/evpn-smoke/plugins; \
    grep -q 'evpn_plugin.so' /tmp/evpn-smoke/plugins; \
    vppctl -s /tmp/evpn-smoke/cli.sock help set interface lacp port-id > /tmp/evpn-smoke/lacp-port-id; \
    grep -q 'set interface lacp port-id' /tmp/evpn-smoke/lacp-port-id; \
    kill "$vpp_pid"; wait "$vpp_pid" || :; trap - EXIT; \
    rm -rf /tmp/evpn-smoke
CMD ["/usr/bin/vpp", "-c", "/etc/vpp/startup.conf"]
