#!/usr/bin/env bash
# Builds the runtime the module ships next to its executable: the libraries
# Firefox needs beyond glibc and libstdc++ (from the Ubuntu 22.04 packages in
# RUNTIME), DejaVu fonts, the font configuration and the packages' licenses.
# Needs dpkg-deb; on hosts other than Ubuntu 22.04 the packages are fetched in
# an ubuntu:22.04 Docker container.
#   scripts/build-runtime.sh <out-dir>   (CMake runs it when needed)
set -euo pipefail
out="$(mkdir -p "$1" && cd "$1" && pwd)"
root="$(cd "$(dirname "$0")/.." && pwd)"
stamp="$(cat "$root/RUNTIME" "$0" | sha256sum | cut -c1-16)"
if [[ -f "$out/.runtime" && "$(cat "$out/.runtime")" == "$stamp" ]]; then
    echo "runtime $stamp already built in $out"
    exit 0
fi

packages="$(grep -v '^#' "$root/RUNTIME" | tr '\n' ' ')"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/debs" "$work/tree"

echo "== Ubuntu 22.04 packages"
if grep -q '^VERSION_CODENAME=jammy' /etc/os-release; then
    (cd "$work/debs" && apt-get download -q $packages)
else
    docker run --rm -v "$work/debs:/w" ubuntu:22.04 sh -c \
        "apt-get update -qq && cd /w && apt-get -o APT::Sandbox::User=root download -q $packages && chmod a+r *.deb"
fi
for deb in "$work"/debs/*.deb; do dpkg-deb -x "$deb" "$work/tree"; done

rm -rf "$out" && mkdir -p "$out"/{lib,fonts,etc,licenses}
for d in "$work/tree/lib/x86_64-linux-gnu" "$work/tree/usr/lib/x86_64-linux-gnu"; do
    [[ -d "$d" ]] && find "$d" -maxdepth 1 -name '*.so*' -exec cp -a {} "$out/lib/" \;
done
cp "$work"/tree/usr/share/fonts/truetype/dejavu/*.ttf "$out/fonts/"
# The copyright files refer to the full license texts by path; they come along.
cp -r /usr/share/common-licenses "$out/licenses/common-licenses" 2>/dev/null ||
    docker run --rm -v "$out/licenses:/w" ubuntu:22.04 cp -r /usr/share/common-licenses /w/
for doc in "$work"/tree/usr/share/doc/*/copyright; do
    cp "$doc" "$out/licenses/$(basename "$(dirname "$doc")").copyright"
done
# Binary package, version, source package and its version (scripts/ci/sources.sh).
for deb in "$work"/debs/*.deb; do
    pkg="$(dpkg-deb -f "$deb" Package)" ver="$(dpkg-deb -f "$deb" Version)" src="$(dpkg-deb -f "$deb" Source)"
    srcname="${src%% *}" srcver="$ver"
    [[ -z "$srcname" ]] && srcname="$pkg"
    [[ "$src" == *"("*")"* ]] && srcver="${src#*(}" && srcver="${srcver%)*}"
    echo "$pkg $ver $srcname $srcver"
done | sort > "$out/licenses/SOURCES"

cat > "$out/etc/fonts.conf" <<'EOF'
<?xml version="1.0"?>
<!DOCTYPE fontconfig SYSTEM "urn:fontconfig:fonts.dtd">
<fontconfig>
  <dir prefix="relative">../fonts</dir>
  <dir>/usr/share/fonts</dir>
  <cachedir prefix="xdg">fontconfig</cachedir>
  <alias><family>sans-serif</family><prefer><family>DejaVu Sans</family></prefer></alias>
  <alias><family>serif</family><prefer><family>DejaVu Serif</family></prefer></alias>
  <alias><family>monospace</family><prefer><family>DejaVu Sans Mono</family></prefer></alias>
</fontconfig>
EOF
echo "$stamp" > "$out/.runtime"
echo "runtime built in $out ($(du -sh "$out" | cut -f1))"
