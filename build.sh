#!/bin/sh
# Builds macapad.3dsx with devkitPro's Docker image (no local devkitPro install needed).
# ./build.sh          clean build (the Docker VM's clock drifts, so incremental builds can go stale)
# ./build.sh <target> any other make target
set -e
cd "$(dirname "$0")"
run() { docker run --rm -v "$PWD":/src -w /src devkitpro/devkitarm make "$@"; }
if [ $# -eq 0 ]; then
	run clean >/dev/null
	run 2>&1 | grep -v "Clock skew"
else
	run "$@"
fi
