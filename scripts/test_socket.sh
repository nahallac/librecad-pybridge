#!/usr/bin/env bash
# End-to-end transport test without LibreCAD: serve the stub document over a
# scratch socket, run the Python smoke test against it, shut the server down.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
server="$repo_root/build-tmp/dispatchtest/dispatchtest"
socket_dir="$(mktemp -d)"
export LC_PYBRIDGE_SOCKET="$socket_dir/bridge.sock"

cleanup() {
    [ -n "${server_pid:-}" ] && kill "$server_pid" 2>/dev/null || true
    rm -rf "$socket_dir"
}
trap cleanup EXIT

"$server" --serve "$LC_PYBRIDGE_SOCKET" &
server_pid=$!

# The server prints its socket path once it listens; wait for the file.
for _ in $(seq 1 50); do
    [ -S "$LC_PYBRIDGE_SOCKET" ] && break
    sleep 0.1
done
[ -S "$LC_PYBRIDGE_SOCKET" ] || { echo "server never opened the socket" >&2; exit 1; }

python3 "$repo_root/python/smoke_test.py" --shutdown

wait "$server_pid"
