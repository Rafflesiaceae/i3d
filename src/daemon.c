#include "i3d.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/inotify.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <unistd.h>

#define I3D_MAX_EPOLL_EVENTS 16

static int add_source(struct i3d_app *app, struct i3d_source *source,
                      uint32_t events) {
  struct epoll_event event = {.events = events, .data.ptr = source};
  return epoll_ctl(app->epoll_fd, EPOLL_CTL_ADD, source->fd, &event);
}

static int arm_timer(int fd, uint64_t milliseconds, bool periodic) {
  struct itimerspec value = {0};
  value.it_value.tv_sec = (time_t)(milliseconds / 1000U);
  value.it_value.tv_nsec = (long)(milliseconds % 1000U) * 1000000L;
  if (milliseconds != 0 && value.it_value.tv_sec == 0 &&
      value.it_value.tv_nsec == 0) {
    value.it_value.tv_nsec = 1;
  }
  if (periodic) {
    value.it_interval = value.it_value;
  }
  return timerfd_settime(fd, 0, &value, NULL);
}

static void consume_timer(int fd) {
  uint64_t expirations;
  while (read(fd, &expirations, sizeof(expirations)) < 0 && errno == EINTR) {
  }
}

static int mkdir_config(const char *path) {
  char *copy = strdup(path);
  if (copy == NULL) {
    return -1;
  }
  for (char *cursor = copy + 1; *cursor != '\0'; ++cursor) {
    if (*cursor != '/') {
      continue;
    }
    *cursor = '\0';
    if (mkdir(copy, 0755) < 0 && errno != EEXIST) {
      free(copy);
      return -1;
    }
    *cursor = '/';
  }
  int result = mkdir(copy, 0755);
  if (result < 0 && errno == EEXIST) {
    result = 0;
  }
  free(copy);
  return result;
}

static int setup_signals(struct i3d_app *app) {
  sigset_t signals;
  sigemptyset(&signals);
  sigaddset(&signals, SIGINT);
  sigaddset(&signals, SIGTERM);
  if (sigprocmask(SIG_BLOCK, &signals, NULL) < 0) {
    return -1;
  }
  app->signal_fd = signalfd(-1, &signals, SFD_NONBLOCK | SFD_CLOEXEC);
  if (app->signal_fd < 0) {
    return -1;
  }
  app->signal_source = (struct i3d_source){
      .kind = I3D_SOURCE_SIGNAL, .fd = app->signal_fd, .owner = app};
  return add_source(app, &app->signal_source, EPOLLIN);
}

static int setup_config_watch(struct i3d_app *app) {
  app->inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
  if (app->inotify_fd < 0) {
    return -1;
  }
  app->inotify_wd =
      inotify_add_watch(app->inotify_fd, app->config_dir,
                        IN_CLOSE_WRITE | IN_CREATE | IN_DELETE | IN_MOVED_FROM |
                            IN_MOVED_TO | IN_ATTRIB);
  if (app->inotify_wd < 0) {
    return -1;
  }
  app->inotify_source = (struct i3d_source){
      .kind = I3D_SOURCE_INOTIFY, .fd = app->inotify_fd, .owner = app};
  return add_source(app, &app->inotify_source, EPOLLIN);
}

static int setup_timer(struct i3d_app *app, int *fd, struct i3d_source *source,
                       enum i3d_source_kind kind) {
  *fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
  if (*fd < 0) {
    return -1;
  }
  *source = (struct i3d_source){.kind = kind, .fd = *fd, .owner = app};
  return add_source(app, source, EPOLLIN);
}

static bool lua_suffix(const char *name) {
  size_t length = strlen(name);
  return length > 4U && strcmp(name + length - 4U, ".lua") == 0;
}

static void handle_inotify(struct i3d_app *app) {
  char buffer[16384]
      __attribute__((aligned(__alignof__(struct inotify_event))));
  bool reload = false;
  for (;;) {
    ssize_t length = read(app->inotify_fd, buffer, sizeof(buffer));
    if (length < 0 && errno == EINTR) {
      continue;
    }
    if (length < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      break;
    }
    if (length <= 0) {
      break;
    }
    for (char *cursor = buffer; cursor < buffer + length;) {
      const struct inotify_event *event = (const void *)cursor;
      if ((event->mask & IN_Q_OVERFLOW) != 0 ||
          (event->len != 0 && lua_suffix(event->name))) {
        reload = true;
        i3d_log(app, I3D_LOG_DEBUG, "config change: %s",
                event->len == 0 ? "inotify overflow" : event->name);
      }
      cursor += sizeof(*event) + event->len;
    }
  }
  if (reload && arm_timer(app->reload_timer_fd, 200, false) < 0) {
    i3d_log(app, I3D_LOG_ERROR, "arm reload timer: %s", strerror(errno));
  }
}

static void reload_registry(struct i3d_app *app) {
  struct i3d_registry *replacement = i3d_registry_load(app);
  if (replacement == NULL) {
    i3d_log(app, I3D_LOG_ERROR,
            "reload failed before a registry could be built; keeping old "
            "configs");
    return;
  }
  struct i3d_registry *old = app->registry;
  app->registry = replacement;
  // epoll may have already returned a timer belonging to an old script in the
  // current batch. Keep that registry alive until every returned event has
  // been consumed, then release it in the main loop.
  app->retired_registry = old;
  i3d_log(app, I3D_LOG_INFO, "scripts active=%zu handlers=%zu",
          replacement->script_count, replacement->total_handler_count);
}

static void schedule_reconnect(struct i3d_app *app) {
  if (app->ipc.event_fd >= 0) {
    epoll_ctl(app->epoll_fd, EPOLL_CTL_DEL, app->ipc.event_fd, NULL);
    close(app->ipc.event_fd);
    app->ipc.event_fd = -1;
  }
  if (arm_timer(app->reconnect_timer_fd, 1000, false) < 0) {
    i3d_log(app, I3D_LOG_ERROR, "arm reconnect timer: %s", strerror(errno));
    app->running = false;
  }
}

static void handle_source(struct i3d_app *app, struct i3d_source *source,
                          uint32_t events) {
  switch (source->kind) {
  case I3D_SOURCE_SIGNAL: {
    struct signalfd_siginfo signal_info;
    if (read(source->fd, &signal_info, sizeof(signal_info)) > 0) {
      i3d_log(app, I3D_LOG_DEBUG, "received signal %u", signal_info.ssi_signo);
      app->running = false;
    }
    break;
  }
  case I3D_SOURCE_INOTIFY:
    handle_inotify(app);
    break;
  case I3D_SOURCE_RELOAD_TIMER:
    consume_timer(source->fd);
    reload_registry(app);
    break;
  case I3D_SOURCE_RECONNECT_TIMER:
    consume_timer(source->fd);
    if (i3d_ipc_reconnect_events(app) < 0) {
      i3d_log(app, I3D_LOG_WARNING,
              "i3 event reconnect failed; retrying in 1 second");
      schedule_reconnect(app);
    } else {
      i3d_log(app, I3D_LOG_INFO, "i3 event connection restored");
    }
    break;
  case I3D_SOURCE_I3_EVENT:
    if ((events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) != 0 ||
        i3d_ipc_drain_events(app) < 0) {
      i3d_log(app, I3D_LOG_WARNING, "i3 event connection lost; reconnecting");
      schedule_reconnect(app);
    }
    break;
  case I3D_SOURCE_PID_NETLINK:
    i3d_pid_dispatch_netlink(app);
    break;
  case I3D_SOURCE_PID_POLL:
    i3d_pid_dispatch_poll(app, source->owner);
    break;
  case I3D_SOURCE_INOTIFY_WATCH:
    i3d_inotify_dispatch(app, source->owner);
    break;
  }
}

int i3d_daemon_run(struct i3d_app *app) {
  if (mkdir_config(app->config_dir) < 0) {
    i3d_log(app, I3D_LOG_ERROR, "create config directory %s: %s",
            app->config_dir, strerror(errno));
    return -1;
  }
  app->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
  if (app->epoll_fd < 0 || setup_signals(app) < 0 ||
      setup_config_watch(app) < 0 ||
      setup_timer(app, &app->reload_timer_fd, &app->reload_source,
                  I3D_SOURCE_RELOAD_TIMER) < 0 ||
      setup_timer(app, &app->reconnect_timer_fd, &app->reconnect_source,
                  I3D_SOURCE_RECONNECT_TIMER) < 0) {
    i3d_log(app, I3D_LOG_ERROR, "initialize epoll sources: %s",
            strerror(errno));
    return -1;
  }

  // Match the Go daemon's startup tolerance for user services that race i3.
  for (unsigned attempt = 1; attempt <= 15; ++attempt) {
    if (i3d_ipc_discover(app) == 0 && i3d_ipc_verify(app) == 0) {
      break;
    }
    free(app->ipc.socket_path);
    app->ipc.socket_path = NULL;
    if (attempt == 15) {
      return -1;
    }
    i3d_log(app, I3D_LOG_DEBUG, "i3 IPC unavailable (attempt %u/15)", attempt);
    struct timespec delay = {.tv_sec = 1};
    while (nanosleep(&delay, &delay) < 0 && errno == EINTR) {
    }
  }
  if (i3d_ipc_subscribe(app) < 0) {
    return -1;
  }

  app->registry = i3d_registry_load(app);
  if (app->registry == NULL) {
    return -1;
  }
  i3d_log(app, I3D_LOG_INFO, "scripts active=%zu handlers=%zu",
          app->registry->script_count, app->registry->total_handler_count);
  i3d_log(app, I3D_LOG_DEBUG, "running; config dir=%s", app->config_dir);

  struct epoll_event events[I3D_MAX_EPOLL_EVENTS];
  while (app->running) {
    int count = epoll_wait(app->epoll_fd, events, I3D_MAX_EPOLL_EVENTS, -1);
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count < 0) {
      i3d_log(app, I3D_LOG_ERROR, "epoll_wait: %s", strerror(errno));
      return -1;
    }
    for (int index = 0; index < count && app->running; ++index) {
      handle_source(app, events[index].data.ptr, events[index].events);
    }
    i3d_registry_free(app, app->retired_registry);
    app->retired_registry = NULL;
  }
  return 0;
}

void i3d_event_cache_clear(struct i3d_app *app) {
  yyjson_doc_free(app->event_tree);
  app->event_tree = NULL;
  free(app->event_tree_raw);
  app->event_tree_raw = NULL;
  app->event_tree_raw_len = 0;
}

void i3d_daemon_cleanup(struct i3d_app *app) {
  i3d_registry_free(app, app->registry);
  app->registry = NULL;
  i3d_registry_free(app, app->retired_registry);
  app->retired_registry = NULL;
  i3d_event_cache_clear(app);
  i3d_ipc_close(app);
  int *fds[] = {&app->signal_fd,       &app->inotify_fd,
                &app->reload_timer_fd, &app->reconnect_timer_fd,
                &app->pid_netlink_fd,  &app->epoll_fd};
  for (size_t index = 0; index < sizeof(fds) / sizeof(fds[0]); ++index) {
    if (*fds[index] >= 0) {
      close(*fds[index]);
      *fds[index] = -1;
    }
  }
}
