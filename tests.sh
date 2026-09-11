#!/bin/sh
set -eu

# Keep the public test entry point consistent with build.sh and always test the
# current sources rather than a stale binary.
./build.sh
meson test -C build --print-errorlogs
