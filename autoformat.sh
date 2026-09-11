#!/bin/sh
set -eu

# clang-format understands C23 and keeps all hand-written C sources stable.
files=$(find src -type f \( -name '*.c' -o -name '*.h' \) -print)
if test -n "$files"; then
    # Intentional word splitting: source paths in this repository contain no
    # whitespace and clang-format accepts all files in one invocation.
    # shellcheck disable=SC2086
    clang-format -i $files
fi

# With no arguments, format every shipped Lua config. Explicit paths are
# useful while editing and StyLua accepts either a Lua file or a directory.
if test "$#" -eq 0; then
    stylua examples
else
    for lua_path in "$@"; do
        stylua "$lua_path"
    done
fi
