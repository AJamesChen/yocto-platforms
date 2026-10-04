#!/bin/sh
set -eu

if ! mountpoint -q /data; then
    echo "RAUC health check failed: /data is not mounted" >&2
    exit 1
fi

# Reaching boot-complete.target with persistent storage available is the initial
# health policy. Add application-specific checks here before production use.
exec rauc status mark-good
