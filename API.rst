i3d Lua API
===========

Globals
-------

Every config receives ``i3``, ``pid``, ``inotify``, ``time``, ``exec``, ``log``,
``i3d_debug``, and ``__file__``. In restricted mode ``debug`` is the same
boolean. In full mode, the standard ``debug`` library remains intact and its
``debug.enabled`` member reflects the daemon setting. Lua calls use positional
arguments; optional arguments are shown in brackets.

Handlers
--------

A config may define these functions::

   function on_workspace(event) end
   function on_output(event) end
   function on_mode(event) end
   function on_window(event) end
   function on_barconfig_update(event) end
   function on_binding(event) end

Alternatively, ``handlers = { workspace = callback, ... }`` may register them.
When several files handle the same event, their handlers execute in ascending
alphabetical basename order. There is no separate priority setting.

Every event has ``type`` and ``change`` string fields. Window events also have
``con_id``, ``workspace_num``, and ``fullscreen_mode``. Values that i3 does not
provide or that cannot be recovered are ``nil``.

i3
--

``i3.command(command) -> boolean`` runs an i3 command and returns true only when
every command result succeeds.

``i3.raw(message [, payload]) -> string`` returns the raw JSON reply.
``i3.query(message [, payload]) -> value`` converts it to Lua tables and scalar
values. Message names include ``get_tree``, ``get_workspaces``, ``get_outputs``,
``get_marks``, ``get_bar_config``, ``get_bar_ids``, and ``get_version``.

``i3.find(criteria) -> array`` walks ``GET_TREE``. Matcher keys may be at the
top level or in ``where``. ``fields`` selects output fields and defaults to
``{"id"}``; ``limit`` of zero means unlimited. Derived match/output fields are
``con_id``, ``workspace_num``, and ``fullscreen``. Other keys address direct
node fields. Boolean, integer, and string equality is supported.

Convenience calls are::

   i3.set_urgency(con_id [, urgent=true])
   i3.rename_workspace(old, new)
   i3.rename_current_workspace(new)
   i3.get_workspace_names()
   i3.get_tree()
   i3.get_workspaces()
   i3.get_outputs()
   i3.get_marks()
   i3.get_version()
   i3.get_bar_ids()
   i3.get_bar_config(bar_id)
   i3.get_window_pid(con_id)
   i3.get_class_names(con_id)
   i3.get_window_property(con_id, atom_name)

One parsed ``GET_TREE`` document is cached for the duration of each event or PID
callback and released afterward.

``i3.get_window_property(con_id, atom_name)`` reads an X11 property directly
through XCB and returns a table with ``type`` (the property's type atom name),
``format`` (8, 16, or 32), and ``value``. Format 8 returns a binary Lua string,
preserving embedded NUL bytes. Formats 16 and 32 return arrays of unsigned
integers; ATOM properties contain numeric atom IDs. Empty properties return
an empty string or array. For example::

   local property = i3.get_window_property(event.con_id, "_GTK_APPLICATION_ID")
   if property and property.value == "com.carriez.flutter_hbb" then
       -- Handle windows carrying this application ID.
   end

An absent property, unknown atom, missing container, or closed window returns
``nil``. Invalid arguments, X connection/protocol failures, and properties over
1 MiB raise Lua errors. Atom names must contain 1..65535 bytes without NULs.
Property reads never delete values or create atoms. Values are read afresh on
each call; this API does not subscribe to property changes.

The X connection opens on the first property read using ``DISPLAY`` and the
normal X authentication environment. It is shared across configs, with a
bounded cache of existing atom names and IDs. X reply waits share a two-second
deadline, shortened to the remaining handler timeout when enabled. A failed
connection is discarded and opened again on a later call.

Processes and time
------------------

``pid.is_ancestor(ancestor, descendant)`` includes identity: a PID is its own
ancestor. ``pid.watch_new(callback [, poll_interval_ms=100 [, use_poll=false]])``
returns an idempotent stop function. The default uses Linux's process connector
and transparently falls back to a timerfd-driven ``/proc`` scan when the kernel
or permissions do not expose it.

``time.now_sec()`` returns Unix time in whole seconds.

Filesystem watches
------------------

``inotify.watch_new(directory, callback) -> stop`` watches one existing
directory, without recursing into subdirectories. A ``~/`` prefix expands
using ``HOME``. The callback receives the full path of each file created in
the directory or moved into it. Directory events are ignored. The returned
stop function is idempotent. Watching ``IN_CREATE`` reports files as soon as
they appear; applications that write temporary files and rename them on
completion also produce an ``IN_MOVED_TO`` callback for the final name.

Execution and logging
---------------------

``exec(args [, check=true [, capture_stdout=true [, capture_stderr=true]]])``
uses ``posix_spawnp`` without a shell. It returns ``rc``, ``stdout``, and
``stderr`` fields. An uncaptured stream is inherited and represented by nil.
Captured streams are each limited to 64 MiB for daemon stability.

``log(message)`` and ``print(...)`` emit informational lines. With
``--journald``, all lines have syslog priority prefixes. ``i3d_debug`` reflects
the daemon's debug setting and ``__file__`` is the absolute config path.
