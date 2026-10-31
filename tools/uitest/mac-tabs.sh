#!/bin/bash
# The tabs of the previous session on macOS: two repositories saved as tabs reopen in one window; the close button
# closes the tab on screen, forgotten while another stays open; the last one closed stays saved and the Dock icon
# reopens it; quitting keeps every tab (MANGROVE_TABTEST makes the app take these steps and log them). Fails when
# one of them doesn't happen, or when the app crashes or hangs.
#
#   ./tools/uitest/mac-tabs.sh
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
app="$root/bin/Mangrove.app/Contents/MacOS/Mangrove"
[ -x "$app" ] || { echo "Build the app first: ./build-mac.sh" >&2; exit 1; }
work="$(mktemp -d "${TMPDIR:-/tmp}/mangrove-tabs.XXXXXX")"
trap 'rm -rf "$work"' EXIT

for name in one two; do
    git init -q -b main "$work/$name"
    git -C "$work/$name" -c user.name="Ada Lovelace" -c user.email=ada@example.com commit -q --allow-empty -m "Start $name"
done
settings="$work/settings.ini"
printf 'tab=%s\ntab=%s\nactive=1\n' "$work/one" "$work/two" >"$settings"

MANGROVE_SETTINGS="$settings" MANGROVE_TABTEST=1 "$app" >"$work/app.log" 2>&1 &
pid=$!
status=0
for _ in $(seq 1 60); do
    kill -0 "$pid" 2>/dev/null || break
    sleep 0.5
done
if kill -0 "$pid" 2>/dev/null; then
    echo "The app did not finish in 30 seconds." >&2
    kill "$pid" || true
    status=1
else
    wait "$pid" || status=$?
fi
cat "$work/app.log"
expect() { grep -q "tabtest: $1" "$work/app.log" || { echo "Expected \"$1\"" >&2; status=1; }; }
expect "restored 2 repositories in 2 tabs"
expect "after closing a tab 1 repositories are open, 1 saved"
expect "after closing the last one 0 repositories are open, 1 saved"
expect "the Dock icon reopened 1 repositories"
expect "quitting with 2 repositories open"
saved="$(tr -d '\r' <"$settings" | grep -c '^tab=' || true)"  # the app writes CRLF line ends
[ "$saved" = 2 ] || { echo "The settings keep $saved tabs, not 2." >&2; status=1; }
exit "$status"
