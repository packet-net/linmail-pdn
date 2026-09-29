#!/bin/sh
# Build the pdn-linmail .deb from an already built pdn/linmail-pdn.
#
#   packaging/build-deb.sh ARCH VERSION      (run from pdn/, after make static)
#
# ARCH is the Debian architecture (amd64, arm64, armhf). The .deb lands in
# pdn/dist. Code goes to /usr/share/packetnet/apps/linmail, where pdn discovers
# app packages; state is left to /var/lib/packetnet/apps/linmail at runtime.

set -e

ARCH=${1:?usage: build-deb.sh ARCH VERSION}
VERSION=${2:?usage: build-deb.sh ARCH VERSION}

HERE=$(cd "$(dirname "$0")/.." && pwd)
TOP=$(cd "$HERE/.." && pwd)
STAGE="$HERE/dist/stage-$ARCH"
APP="$STAGE/usr/share/packetnet/apps/linmail"
DOC="$STAGE/usr/share/doc/pdn-linmail"

test -x "$HERE/linmail-pdn" || { echo "build linmail-pdn first (make -C pdn static)"; exit 1; }

rm -rf "$STAGE"
mkdir -p "$APP/HTML" "$DOC" "$STAGE/DEBIAN"

install -m 0755 "$HERE/linmail-pdn" "$APP/linmail-pdn"
strip "$APP/linmail-pdn"
sed "s/@VERSION@/$VERSION/" "$HERE/packaging/pdn-app.yaml" > "$APP/pdn-app.yaml"
install -m 0644 "$HERE/linmail-pdn.conf.example" "$APP/linmail-pdn.conf.example"

# The HTML templates the mail pages use (LinBPQ keeps these in its HTML/ dir)
for f in MainConfig.txt FwdPage.txt FwdDetail.txt UserPage.txt UserDetail.txt MsgPage.txt \
		WP.txt Housekeeping.txt WebMailPage.txt WebMailMsg.txt webscript.js background.jpg favicon.ico; do
	install -m 0644 "$TOP/HTML/$f" "$APP/HTML/$f"
done

install -m 0644 "$HERE/MIGRATING.md" "$DOC/MIGRATING.md"
cat > "$DOC/copyright" <<COPYRIGHT
pdn-linmail: the LinBPQ mail server as a Packet.NET app.

LinBPQ/BPQ32 is Copyright 2001-2026 John Wiseman G8BPQ, and the pdn shim is
Copyright 2026 the linbpq fork contributors. Source: https://github.com/M0LTE/linbpq

This program is free software: you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation, either version 3 of the License, or (at your option) any later
version. On Debian systems the full text is in /usr/share/common-licenses/GPL-3.
COPYRIGHT

sed -e "s/@VERSION@/$VERSION/" -e "s/@ARCH@/$ARCH/" "$HERE/packaging/control.in" > "$STAGE/DEBIAN/control"
echo "Installed-Size: $(du -sk --apparent-size "$STAGE/usr" | cut -f1)" >> "$STAGE/DEBIAN/control"
install -m 0755 "$HERE/packaging/postinst" "$STAGE/DEBIAN/postinst"

dpkg-deb --root-owner-group --build "$STAGE" "$HERE/dist/pdn-linmail_${VERSION}_${ARCH}.deb"
rm -rf "$STAGE"
ls -l "$HERE/dist/pdn-linmail_${VERSION}_${ARCH}.deb"
