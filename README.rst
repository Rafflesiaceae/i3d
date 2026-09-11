i3d
====

``i3d`` is a small Linux-only i3 daemon written in C23. It embeds LuaJIT,
implements the i3 IPC wire protocol directly, parses JSON with yyjson, and
drives its sockets, signals, timers, config changes, and PID notifications from
one epoll loop.

Configuration
-------------

The daemon watches ``$HOME/.config/i3d/*.lua``. Set ``I3D_DIR`` to override the
directory. When several files handle the same event, handlers run in ascending
alphabetical basename order. Additions, atomic replacements, edits, and
deletions are applied after a 200 ms debounce. A file that fails to load is
disabled while other files remain active.

Each file may define ``on_workspace``, ``on_output``, ``on_mode``,
``on_window``, ``on_barconfig_update``, or ``on_binding``. See ``API.rst`` and
``examples/``.

Lua environment
---------------

The default environment contains Lua's base, string, table, and math
facilities plus the documented i3d API. File loading, modules, I/O, OS access,
explicit garbage collection, the debug library, JIT controls, and FFI are
omitted. This prevents an accidental config error from directly corrupting
daemon state.

Restricted mode reduces the available Lua surface; it is not a security
sandbox. Configs can still issue i3 commands and launch programs through the
documented ``exec`` function, so config files must remain trusted.

``--full-lua`` calls ``luaL_openlibs`` and therefore enables all libraries in
the installed LuaJIT, including ``package``, ``jit``, and ``ffi``. Use it only
with trusted configs: FFI code has native process access and can crash or
corrupt i3d.

Lua patterns
------------

The examples use ``string.find`` in plain or Lua-pattern mode. This is enough
for their substring, anchored, wildcard, and case-folded title matching, and
avoids another regular-expression dependency. Lua patterns are intentionally
smaller than PCRE: they do not have alternation, lookaround, or inline flags.
The title-hiding example exposes an explicit ``ignore_case`` setting instead of
``(?i)``.

Build and install
-----------------

Dependencies are Meson, a C23 compiler, and LuaJIT. yyjson 0.12.0 is vendored
from its upstream release so builds do not depend on a system yyjson package.

::

   ./autoformat.sh
   ./build.sh
   meson install -C build

Run ``build/i3d`` or enable the installed user service with
``systemctl --user enable --now i3d.service``. The sample unit deliberately
uses ``--journald`` so every output line starts with a syslog priority such as
``<3>`` for errors, ``<6>`` for informational messages, or ``<7>`` for debug
messages.

Environment
-----------

``I3D_DIR`` overrides the config directory. ``DEBUG=1`` enables debug logs.
``I3D_HANDLER_MAX_STEPS`` sets the per-call Lua instruction limit (default
5,000,000; 0 disables it). ``I3D_HANDLER_TIMEOUT_MS`` sets the per-call
monotonic wall-clock limit (default 2,000 ms; 0 disables it).

Scope
-----

i3d supports Linux and i3 only. ``src/xcb_backend.*`` defines the lifecycle
and file-descriptor seam for a future XCB source without linking XCB or changing
the Lua API today.
