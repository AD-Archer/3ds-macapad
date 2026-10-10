#!/bin/sh
# Packs macapad.elf + macapad.smdh (from `make`) into macapad.cia, for installing with FBI.
# Runs inside devkitPro's devkitarm image on x86_64 Linux (GitHub Actions, or `just cia`).
# VERSION=1.2.3 stamps the title's version; it defaults to 0.0.0.
set -e
cd "$(dirname "$0")/.."

# Older builds on purpose: newer Linux ones need a newer glibc than the devkitarm image (Debian 12) has.
MAKEROM_URL=https://github.com/3DSGuy/Project_CTR/releases/download/makerom-v0.18.4/makerom-v0.18.4-ubuntu_x86_64.zip
MAKEROM_SHA=dd596854718c195c6e3229286be485b122921715555af8ae5cf8e9a465d9f970
BANNERTOOL_URL=https://github.com/diasurgical/bannertool/releases/download/1.2.0/bannertool.zip
BANNERTOOL_SHA=69768596f836acb3e3aeaa66e47c6ba560dde813c6dfcd33c8afc25fe29b7524

TOOLS=.cia-tools
mkdir -p "$TOOLS"

fetch() {  # url sha256 file
	[ -f "$TOOLS/$3" ] || curl -sSfL "$1" -o "$TOOLS/$3"
	echo "$2  $TOOLS/$3" | sha256sum -c --quiet
}
if [ ! -f "$TOOLS/makerom" ]; then
	fetch "$MAKEROM_URL" "$MAKEROM_SHA" makerom.zip
	unzip -qo "$TOOLS/makerom.zip" -d "$TOOLS"
fi
if [ ! -f "$TOOLS/bannertool" ]; then
	fetch "$BANNERTOOL_URL" "$BANNERTOOL_SHA" bannertool.zip
	unzip -qjo "$TOOLS/bannertool.zip" linux-x86_64/bannertool -d "$TOOLS"
fi
# (not -x: as root, the shell says yes to that even with no execute bit)
chmod +x "$TOOLS/makerom" "$TOOLS/bannertool"

[ -f macapad.elf ] && [ -f macapad.smdh ] || { echo "Run make first" >&2; exit 1; }

VERSION="${VERSION:-0.0.0}"
VERSION="${VERSION#v}"
MAJOR=$(echo "$VERSION" | cut -d. -f1)
MINOR=$(echo "$VERSION" | cut -d. -f2)
MICRO=$(echo "$VERSION" | cut -d. -f3)

mkdir -p build
"$TOOLS/bannertool" makebanner -i cia/banner.png -a cia/Single-Click.wav -o build/banner.bnr >/dev/null
"$TOOLS/makerom" -f cia -o macapad.cia -target t -exefslogo \
	-elf macapad.elf -rsf cia/macapad.rsf -icon macapad.smdh -banner build/banner.bnr \
	-major "${MAJOR:-0}" -minor "${MINOR:-0}" -micro "${MICRO:-0}"
echo "built ... macapad.cia ($VERSION)"
