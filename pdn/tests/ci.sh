#!/bin/sh
# The whole linmail-pdn test run, as CI does it. Used by both
# .github/workflows/linmail-pdn.yml (one stage per step) and the upstream sync
# workflow (every stage in one go, so its pull request can carry the result).
#
#   pdn/tests/ci.sh [STAGE ...]        (from the top of the repo; no STAGE = all)
#
# Stages, in order:
#   deps      install the build and test dependencies (apt, needs sudo)
#   build     build linmail-pdn, and linmail-pdn-asan with AddressSanitizer
#   linbpq    build LinBPQ from this tree (for the real pdn tests)
#   pdn       fetch the latest pdn node release (needs gh and GH_TOKEN)
#   fake      the fake RHP server and web suites
#   asan      the same, under AddressSanitizer
#   real      against a real pdn node and LinBPQ
#   debbuild  build the amd64 .deb in a Debian bookworm container (docker)
#   deb       install the .deb and run it against a real pdn node (sudo)
#
# Everything is on loopback. JUnit reports go to pdn/reports/. The pdn node is
# kept in $CI_WORK (default $RUNNER_TEMP/linmail-pdn-ci, else
# /tmp/linmail-pdn-ci) so later stages find it.

set -e

TOP=$(cd "$(dirname "$0")/../.." && pwd)
cd "$TOP"

CI_WORK=${CI_WORK:-${RUNNER_TEMP:-/tmp}/linmail-pdn-ci}
PDN_BIN=${PDN_BIN:-$CI_WORK/pdn/packetnet}
LINBPQ_BIN=${LINBPQ_BIN:-$TOP/linbpq}
export PDN_BIN LINBPQ_BIN

mkdir -p "$CI_WORK" pdn/reports

say() {
	echo
	echo "=== $* ==="
}

need() {
	for f in "$@"; do
		test -x "$f" || { echo "missing $f: run the earlier stages first"; exit 1; }
	done
}

stage_deps() {
	say "Installing build dependencies"
	sudo apt-get update
	sudo apt-get install -y --no-install-recommends \
		build-essential libjansson-dev libconfig-dev zlib1g-dev \
		libminiupnpc-dev libpcap-dev python3-pytest
}

stage_build() {
	say "Building linmail-pdn"
	make -C pdn -j"$(nproc)"
	say "Building linmail-pdn with AddressSanitizer"
	make -C pdn -j"$(nproc)" asan
	pdn/linmail-pdn --version
}

stage_linbpq() {
	say "Building LinBPQ (for the real pdn tests)"
	# G8BPQ's makefile, without MQTT; libbacktrace comes with gcc
	make -j"$(nproc)" nomqtt \
		LIBS="-lminiupnpc -lm -lz -lpthread -lconfig -lpcap -ljansson -lbacktrace"
	test -x linbpq
}

stage_pdn() {
	say "Fetching the latest pdn node release"
	tag=$(gh release list -R packet-net/packet.net --limit 30 --json tagName \
		-q '[.[] | select(.tagName | startswith("node-v"))][0].tagName')
	echo "pdn release $tag"
	mkdir -p "$CI_WORK/pdn"
	gh release download "$tag" -R packet-net/packet.net \
		-p packetnet_amd64.tar.gz -D "$CI_WORK/pdn" --clobber
	tar xzf "$CI_WORK/pdn/packetnet_amd64.tar.gz" -C "$CI_WORK/pdn" ./packetnet
	test -x "$PDN_BIN"
}

stage_fake() {
	need pdn/linmail-pdn
	say "Tests against a fake RHP server, and the web pages"
	python3 -m pytest -p no:cacheprovider pdn/tests -v --ignore=pdn/tests/test_real_pdn.py --ignore=pdn/tests/test_deb.py \
		--junitxml=pdn/reports/linmail-pdn-fake.xml
}

stage_asan() {
	need pdn/linmail-pdn-asan
	say "The same, under AddressSanitizer"
	LINMAIL_PDN_BIN="$TOP/pdn/linmail-pdn-asan" ASAN_OPTIONS=detect_leaks=0 \
		python3 -m pytest -p no:cacheprovider pdn/tests -v --ignore=pdn/tests/test_real_pdn.py --ignore=pdn/tests/test_deb.py \
		--junitxml=pdn/reports/linmail-pdn-asan.xml
}

stage_real() {
	need pdn/linmail-pdn "$PDN_BIN" "$LINBPQ_BIN"
	say "Tests against a real pdn node and LinBPQ"
	python3 -m pytest -p no:cacheprovider pdn/tests/test_real_pdn.py -v -s --junitxml=pdn/reports/linmail-pdn-real.xml
}

stage_debbuild() {
	version="$(make -C pdn -s linbpq-version)-pdn0~ci${GITHUB_RUN_NUMBER:-0}"
	say "Building the amd64 .deb $version (Debian bookworm container)"
	rm -f pdn/dist/pdn-linmail_*_amd64.deb
	pdn/packaging/build-in-docker.sh amd64 "$version"
}

stage_deb() {
	need "$PDN_BIN" "$LINBPQ_BIN"
	LINMAIL_PDN_DEB=$(ls "$TOP"/pdn/dist/pdn-linmail_*_amd64.deb)
	export LINMAIL_PDN_DEB
	say "Installing $(basename "$LINMAIL_PDN_DEB") and running it against a real pdn node"
	python3 -m pytest -p no:cacheprovider pdn/tests/test_deb.py -v -s --junitxml=pdn/reports/linmail-pdn-deb.xml
}

STAGES=${*:-deps build linbpq pdn fake asan real debbuild deb}

for s in $STAGES; do
	case "$s" in
	deps|build|linbpq|pdn|fake|asan|real|debbuild|deb) "stage_$s" ;;
	*) echo "unknown stage $s"; exit 2 ;;
	esac
done

say "All stages passed: $STAGES"
