#!/usr/bin/env bash
# Back-compat wrapper. Use ./install.sh --user from the repo root.
exec "$(cd "$(dirname "$0")/.." && pwd)/install.sh" --user "$@"
