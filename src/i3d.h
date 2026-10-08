#ifndef I3D_H
#define I3D_H

#define _GNU_SOURCE

#include <lauxlib.h>
#include <lua.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>
#include <yyjson.h>

enum i3d_log_level {
  I3D_LOG_ERROR = 3,
  I3D_LOG_WARNING = 4,
  I3D_LOG_INFO = 6,
  I3D_LOG_DEBUG = 7,
};

enum i3d_event_type {
  I3D_EVENT_WORKSPACE = 0,
  I3D_EVENT_OUTPUT,
  I3D_EVENT_MODE,
  I3D_EVENT_WINDOW,
  I3D_EVENT_BARCONFIG_UPDATE,
  I3D_EVENT_BINDING,
  I3D_EVENT_COUNT,
};

enum i3d_source_kind {
  I3D_SOURCE_SIGNAL = 1,
  I3D_SOURCE_INOTIFY,
  I3D_SOURCE_RELOAD_TIMER,
  I3D_SOURCE_RECONNECT_TIMER,
  I3D_SOURCE_I3_EVENT,
  I3D_SOURCE_PID_NETLINK,
  I3D_SOURCE_PID_POLL,
  I3D_SOURCE_INOTIFY_WATCH,
};

struct i3d_app;
struct i3d_script;

struct i3d_source {
  enum i3d_source_kind kind;
  int fd;
  void *owner;
};

struct i3d_buffer {
  char *data;
  size_t len;
  size_t cap;
};

struct i3d_ipc {
  char *socket_path;
  int request_fd;
  int request_epoll_fd;
  int event_fd;
  struct i3d_buffer event_input;
};

struct i3d_pid_watch {
  struct i3d_pid_watch *next;
  struct i3d_script *script;
  int callback_ref;
  bool active;
  bool polling;
  unsigned poll_interval_ms;
  int timer_fd;
  struct i3d_source source;
  pid_t *snapshot;
  size_t snapshot_len;
};

struct i3d_inotify_watch {
  struct i3d_inotify_watch *next;
  struct i3d_script *script;
  int callback_ref;
  bool active;
  int fd;
  char *directory;
  struct i3d_source source;
};

struct i3d_script {
  struct i3d_script *next;
  struct i3d_app *app;
  char *path;
  char *base;
  lua_State *lua;
  int handlers[I3D_EVENT_COUNT];
  uint64_t hook_steps;
  struct timespec hook_deadline;
};

struct i3d_registry {
  struct i3d_script *scripts;
  struct i3d_script **handlers[I3D_EVENT_COUNT];
  size_t handler_count[I3D_EVENT_COUNT];
  size_t script_count;
  size_t total_handler_count;
};

struct i3d_app {
  const char *program_name;
  char *config_dir;
  bool debug;
  bool journald;
  bool full_lua;
  bool running;
  uint64_t handler_max_steps;
  uint64_t handler_timeout_ms;

  int epoll_fd;
  int signal_fd;
  int inotify_fd;
  int inotify_wd;
  int reload_timer_fd;
  int reconnect_timer_fd;
  int pid_netlink_fd;
  struct i3d_source signal_source;
  struct i3d_source inotify_source;
  struct i3d_source reload_source;
  struct i3d_source reconnect_source;
  struct i3d_source event_source;
  struct i3d_source pid_netlink_source;

  struct i3d_ipc ipc;
  struct i3d_registry *registry;
  struct i3d_registry *retired_registry;
  struct i3d_pid_watch *pid_watches;
  struct i3d_inotify_watch *inotify_watches;

  yyjson_doc *event_tree;
  char *event_tree_raw;
  size_t event_tree_raw_len;
  bool dispatching;
};

void i3d_log(struct i3d_app *app, enum i3d_log_level level, const char *format,
             ...) __attribute__((format(printf, 3, 4)));

const char *i3d_event_name(enum i3d_event_type type);
int i3d_event_from_ipc(uint32_t ipc_type, enum i3d_event_type *type);

int i3d_ipc_discover(struct i3d_app *app);
int i3d_ipc_verify(struct i3d_app *app);
int i3d_ipc_subscribe(struct i3d_app *app);
int i3d_ipc_reconnect_events(struct i3d_app *app);
void i3d_ipc_close(struct i3d_app *app);
int i3d_ipc_request(struct i3d_app *app, uint32_t type, const char *payload,
                    size_t payload_len, char **reply, size_t *reply_len);
int i3d_ipc_drain_events(struct i3d_app *app);

struct i3d_registry *i3d_registry_load(struct i3d_app *app);
void i3d_registry_free(struct i3d_app *app, struct i3d_registry *registry);
void i3d_dispatch_json_event(struct i3d_app *app, uint32_t ipc_type,
                             const char *json, size_t json_len);
void i3d_event_cache_clear(struct i3d_app *app);
void i3d_lua_call_pid(struct i3d_app *app, struct i3d_pid_watch *watch,
                      pid_t child_pid, pid_t parent_pid);
void i3d_lua_call_inotify(struct i3d_app *app, struct i3d_inotify_watch *watch,
                          const char *path);
bool i3d_pid_is_ancestor(pid_t ancestor, pid_t descendant);

int i3d_pid_watch_add(struct i3d_app *app, struct i3d_script *script,
                      int callback_ref, unsigned poll_interval_ms,
                      bool force_poll, struct i3d_pid_watch **out);
void i3d_pid_watch_stop(struct i3d_app *app, struct i3d_pid_watch *watch);
void i3d_pid_watch_remove_script(struct i3d_app *app,
                                 struct i3d_script *script);
void i3d_pid_dispatch_netlink(struct i3d_app *app);
void i3d_pid_dispatch_poll(struct i3d_app *app, struct i3d_pid_watch *watch);

int i3d_inotify_watch_add(struct i3d_app *app, struct i3d_script *script,
                          int callback_ref, const char *directory,
                          struct i3d_inotify_watch **out);
void i3d_inotify_watch_stop(struct i3d_app *app,
                            struct i3d_inotify_watch *watch);
void i3d_inotify_watch_remove_script(struct i3d_app *app,
                                     struct i3d_script *script);
void i3d_inotify_dispatch(struct i3d_app *app, struct i3d_inotify_watch *watch);

int i3d_daemon_run(struct i3d_app *app);
void i3d_daemon_cleanup(struct i3d_app *app);

#endif
