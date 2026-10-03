#!/usr/bin/env bash
# Protocol smoke test: plays the core's side of a short session (hello, ping,
# shutdown) and checks the plugin's replies.
#   scripts/ci/smoke.sh build/webview.plugin/webview
set -euo pipefail
bin="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

input="$(printf '%s\n' \
    "{\"t\":\"hello\",\"api\":3,\"data_dir\":\"$work\",\"locale\":\"en\",\"timezone\":\"UTC\",\"permissions\":[\"background\"]}" \
    '{"t":"ping","seq":7}')"
out="$( (printf '%s\n' "$input"; sleep 1; echo '{"t":"shutdown"}') | FACET_PLUGIN_DATA="$work" timeout 30 "$bin")"
echo "$out"

check() {
    if ! grep -q "$1" <<<"$out"; then
        echo "smoke test failed: missing $2" >&2
        exit 1
    fi
}
check '"t":"hello"' "hello reply"
check '"id":"webview"' "plugin id"
check '"api":3' "API version"
check '"sdk":' "SDK version"
check '"t":"pong"' "pong"
check '"seq":7' "ping sequence"
echo "ok: protocol session passed"
