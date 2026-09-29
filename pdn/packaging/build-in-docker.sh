#!/bin/sh
# Build the pdn-linmail .deb for one architecture inside a Debian bookworm
# container, so the binary needs nothing newer than bookworm's libc and runs on
# bookworm and trixie alike. Non-native architectures run under QEMU (binfmt).
#
#   pdn/packaging/build-in-docker.sh ARCH VERSION     (ARCH: amd64, arm64, armhf;
#                                                      VERSION e.g. 6.0.25.41-pdn1)
#
# The .deb lands in pdn/dist.

set -e

ARCH=${1:?usage: build-in-docker.sh ARCH VERSION}
VERSION=${2:?usage: build-in-docker.sh ARCH VERSION}

case "$ARCH" in
amd64) PLATFORM=linux/amd64 ;;
arm64) PLATFORM=linux/arm64 ;;
armhf) PLATFORM=linux/arm/v7 ;;
*) echo "unknown ARCH $ARCH"; exit 1 ;;
esac

TOP=$(cd "$(dirname "$0")/../.." && pwd)
mkdir -p "$TOP/pdn/dist"

docker run --rm --platform "$PLATFORM" -v "$TOP:/src:ro" -v "$TOP/pdn/dist:/out" \
	-e ARCH="$ARCH" -e VERSION="$VERSION" -e HOST_UID="$(id -u)" -e HOST_GID="$(id -g)" \
	debian:bookworm sh -ec '
		apt-get update -qq
		apt-get install -y -qq --no-install-recommends build-essential libjansson-dev libconfig-dev zlib1g-dev >/dev/null
		mkdir /build
		cd /src && tar --exclude=./pdn/build --exclude=./pdn/dist --exclude=./pdn/linmail-pdn --exclude=./.git -cf - . | tar -xf - -C /build
		cd /build/pdn
		make -j"$(nproc)" static PDN_VERSION="$VERSION" >/dev/null
		packaging/build-deb.sh "$ARCH" "$VERSION"
		cp dist/*.deb /out/
		chown "$HOST_UID:$HOST_GID" /out/*.deb
	'
