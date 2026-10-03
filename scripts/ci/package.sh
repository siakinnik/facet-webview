#!/usr/bin/env bash
# Packs a release archive facet-webview-<version>-linux-<arch>.tar.gz whose top
# directory is the plugin directory (manifest.json, executable, runtime/), as
# expected by facet-core's `get.sh --plugin`. Firefox itself is not included:
# the module downloads Firefox ESR from Mozilla on the device.
#   scripts/ci/package.sh <plugin-dir> <version> <arch> <out-dir>
set -euo pipefail
dir="$1" version="$2" arch="$3" out="$4"
root="$(cd "$(dirname "$0")/../.." && pwd)"
stage="$(mktemp -d)/webview"
mkdir -p "$stage"
install -m755 "$dir/webview" "$stage/webview"
install -m644 "$dir/manifest.json" "$stage/manifest.json"
cp -a "$dir/runtime" "$stage/runtime"
for f in LICENSE README.md; do
    [[ -f "$root/$f" ]] && install -m644 "$root/$f" "$stage/$f"
done
# The executable is static: the licenses of what is built into it, the
# packages they come from (licenses/STATIC) and the full license texts.
bash "$root/scripts/ci/licenses.sh" "$stage" "${CXX:-g++}" libc.a libstdc++.a libgcc_eh.a libssl.a libcrypto.a liblzma.a
mkdir -p "$out"
name="facet-webview-$version-linux-$arch.tar.gz"
tar -C "$(dirname "$stage")" -czf "$out/$name" webview
echo "$out/$name"
