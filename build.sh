#!/bin/sh
set -eu

# Reconfigure on every invocation so dependency and option changes are picked
# up without requiring callers to understand Meson's setup lifecycle.
if test -d build; then
    meson setup --reconfigure build
else
    meson setup build
fi
meson compile -C build
