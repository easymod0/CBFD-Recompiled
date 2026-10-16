#!/bin/sh
# Builds a Linux netplay playtest package that runs on most distributions, in an Ubuntu 24.04 container
# (Docker), from a commit pushed to GitHub:
#
#   tools/netplay/package-linux.sh <commit> <rom.z64> [out folder (.)]
#
# It writes ConkerRecompiled-netplay-playtest-<short commit>-Linux.tar.gz, packaged as release.yml packages
# the Linux build, with protobuf linked into the program (-DProtobuf_USE_STATIC_LIBS=ON: distributions
# ship other protobuf versions under other names, Arch libprotobuf.so.36 where Ubuntu has .so.32), plus
# LICENSE-protobuf.txt and LICENSE-GameNetworkingSockets.txt (both BSD 3-clause).
# Built on Ubuntu 24.04 (glibc 2.39), it runs on that and anything newer. A build on a newer distribution
# (Arch, say) wouldn't: it would need that distribution's glibc. The rest it needs (SDL2, OpenSSL 3,
# freetype, dbus) comes from the system, under the same names everywhere.
#
# REPO picks where the commit is cloned from (default: https://github.com/easymod0/CBFD-Recompiled).
# Check the package with tools/netplay/check.sh --exe <unpacked>/ConkerRecompiled/ConkerRecomp.
set -eu

[ $# -ge 2 ] || { sed -n '2,17p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }
COMMIT=$1
ROM=$(realpath "$2")
OUT=$(realpath "${3:-.}")
REPO=${REPO:-https://github.com/easymod0/CBFD-Recompiled}
[ -f "$ROM" ] || { echo "no ROM at $2" >&2; exit 2; }
mkdir -p "$OUT"

docker run --rm -i \
    -e COMMIT="$COMMIT" -e REPO="$REPO" -e OWNER="$(id -u):$(id -g)" \
    -v "$ROM":/rom/baserom.us.z64:ro -v "$OUT":/out \
    ubuntu:24.04 sh -s <<'EOF'
set -eu
export DEBIAN_FRONTEND=noninteractive
apt-get update -q >/dev/null
apt-get install -y -q git python3 python3-venv cmake ninja-build clang lld pkg-config libsdl2-dev \
    libdbus-1-dev libfreetype-dev libprotobuf-dev protobuf-compiler libssl-dev binutils >/dev/null
git clone -q "$REPO" /src
cd /src
git checkout -q "$COMMIT"
CONKER_CMAKE_ARGS="-DCONKER_NETPLAY=ON -DProtobuf_USE_STATIC_LIBS=ON" ./build.sh /rom/baserom.us.z64 < /dev/null

out=/pkg/ConkerRecompiled
mkdir -p "$out"
cp host/build/ConkerRecomp "$out/"
strip "$out/ConkerRecomp"
cp -r host/build/assets "$out/assets"
cp .github/release/README.txt LICENSE .github/release/LICENSES.txt "$out/"
cp tools/N64ModernRuntime/COPYING "$out/LICENSE-GPL-3.0.txt"
cp host/build/_deps/gamenetworkingsockets-src/LICENSE "$out/LICENSE-GameNetworkingSockets.txt"
cp /usr/share/doc/libprotobuf32t64/copyright "$out/LICENSE-protobuf.txt"
if ldd "$out/ConkerRecomp" | grep -q libprotobuf; then echo "protobuf isn't linked statically" >&2; exit 1; fi

name=ConkerRecompiled-netplay-playtest-$(git rev-parse --short=7 HEAD)-Linux.tar.gz
tar -czf "/out/$name" -C /pkg ConkerRecompiled
chown "$OWNER" "/out/$name"
echo "build id: $(grep -o '"[^"]*"' host/build/netplay_build_id.cpp)"
echo "wrote $name"
EOF
