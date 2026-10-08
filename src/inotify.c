#include "i3d.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/inotify.h>
#include <unistd.h>

static char *expand_home(const char *path) {
  // Support the common current-user shorthand without enabling shell parsing.
  if (path[0] != '~' || (path[1] != '\0' && path[1] != '/')) {
    return strdup(path);
  }
  const char *home = getenv("HOME");
  if (home == NULL || home[0] == '\0') {
    errno = ENOENT;
    return NULL;
  }
  char *expanded = NULL;
  if (asprintf(&expanded, "%s%s", home, path + 1) < 0) {
    return NULL;
  }
  return expanded;
}

int i3d_inotify_watch_add(struct i3d_app *app, struct i3d_script *script,
                          int callback_ref, const char *directory,
                          struct i3d_inotify_watch **out) {
  struct i3d_inotify_watch *watch = calloc(1, sizeof(*watch));
  if (watch == NULL) {
    return -1;
  }
  watch->fd = -1;
  watch->directory = expand_home(directory);
  if (watch->directory == NULL) {
    free(watch);
    return -1;
  }

  // A private fd gives each Lua watch its own stable epoll source and queue.
  watch->fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
  if (watch->fd < 0) {
    goto failed;
  }
  int watch_descriptor = inotify_add_watch(
      watch->fd, watch->directory,
      IN_CREATE | IN_MOVED_TO | IN_DELETE_SELF | IN_MOVE_SELF | IN_ONLYDIR);
  if (watch_descriptor < 0) {
    goto failed;
  }
  watch->script = script;
  watch->callback_ref = callback_ref;
  watch->active = true;
  watch->source = (struct i3d_source){
      .kind = I3D_SOURCE_INOTIFY_WATCH, .fd = watch->fd, .owner = watch};
  struct epoll_event event = {.events = EPOLLIN | EPOLLERR | EPOLLHUP,
                              .data.ptr = &watch->source};
  if (epoll_ctl(app->epoll_fd, EPOLL_CTL_ADD, watch->fd, &event) < 0) {
    goto failed;
  }
  watch->next = app->inotify_watches;
  app->inotify_watches = watch;
  *out = watch;
  return 0;

failed:
  {
    int saved_errno = errno;
    if (watch->fd >= 0) {
      close(watch->fd);
    }
    free(watch->directory);
    free(watch);
    errno = saved_errno;
    return -1;
  }
}

void i3d_inotify_watch_stop(struct i3d_app *app,
                            struct i3d_inotify_watch *watch) {
  if (watch == NULL || !watch->active) {
    return;
  }
  watch->active = false;
  if (watch->fd >= 0) {
    epoll_ctl(app->epoll_fd, EPOLL_CTL_DEL, watch->fd, NULL);
    close(watch->fd);
    watch->fd = -1;
  }
  // Retain the watch object until registry teardown in case epoll already
  // returned its source pointer in the current event batch.
}

void i3d_inotify_watch_remove_script(struct i3d_app *app,
                                     struct i3d_script *script) {
  struct i3d_inotify_watch **link = &app->inotify_watches;
  while (*link != NULL) {
    struct i3d_inotify_watch *watch = *link;
    if (watch->script != script) {
      link = &watch->next;
      continue;
    }
    i3d_inotify_watch_stop(app, watch);
    luaL_unref(script->lua, LUA_REGISTRYINDEX, watch->callback_ref);
    *link = watch->next;
    free(watch->directory);
    free(watch);
  }
}

static char *event_path(const char *directory, const char *name) {
  // Inotify reports a name relative to its watched directory; callbacks get
  // one path they can pass directly to other Lua APIs or subprocesses.
  size_t directory_len = strlen(directory);
  size_t name_len = strlen(name);
  bool separator = directory_len != 0 && directory[directory_len - 1U] != '/';
  if (directory_len > SIZE_MAX - name_len - (separator ? 2U : 1U)) {
    errno = ENAMETOOLONG;
    return NULL;
  }
  size_t length = directory_len + name_len + (separator ? 1U : 0U);
  char *path = malloc(length + 1U);
  if (path == NULL) {
    return NULL;
  }
  memcpy(path, directory, directory_len);
  size_t offset = directory_len;
  if (separator) {
    path[offset++] = '/';
  }
  memcpy(path + offset, name, name_len);
  path[length] = '\0';
  return path;
}

void i3d_inotify_dispatch(struct i3d_app *app,
                          struct i3d_inotify_watch *watch) {
  if (watch == NULL || !watch->active) {
    return;
  }
  char buffer[16384]
      __attribute__((aligned(__alignof__(struct inotify_event))));
  for (;;) {
    ssize_t length = read(watch->fd, buffer, sizeof(buffer));
    if (length < 0 && errno == EINTR) {
      continue;
    }
    if (length < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      return;
    }
    if (length <= 0) {
      if (length < 0) {
        i3d_log(app, I3D_LOG_WARNING, "read inotify watch for %s: %s",
                watch->directory, strerror(errno));
      }
      i3d_inotify_watch_stop(app, watch);
      return;
    }

    size_t remaining = (size_t)length;
    char *cursor = buffer;
    while (remaining >= sizeof(struct inotify_event) && watch->active) {
      const struct inotify_event *event = (const void *)cursor;
      size_t event_size = sizeof(*event) + event->len;
      if (event_size > remaining) {
        i3d_log(app, I3D_LOG_WARNING,
                "malformed inotify event for %s; stopping watch",
                watch->directory);
        i3d_inotify_watch_stop(app, watch);
        return;
      }
      // Creation reports direct writes; move-in reports atomic downloads.
      // Ignore subdirectories because this API watches files only.
      if ((event->mask & IN_Q_OVERFLOW) != 0) {
        i3d_log(app, I3D_LOG_WARNING,
                "inotify event queue overflow for %s; some file additions "
                "may have been missed",
                watch->directory);
      } else if ((event->mask & (IN_DELETE_SELF | IN_MOVE_SELF | IN_IGNORED)) !=
                 0) {
        i3d_log(app, I3D_LOG_WARNING, "watched directory is unavailable: %s",
                watch->directory);
        i3d_inotify_watch_stop(app, watch);
      } else if ((event->mask & (IN_CREATE | IN_MOVED_TO)) != 0 &&
                 (event->mask & IN_ISDIR) == 0 && event->len != 0) {
        char *path = event_path(watch->directory, event->name);
        if (path == NULL) {
          i3d_log(app, I3D_LOG_WARNING, "build inotify event path: %s",
                  strerror(errno));
        } else {
          i3d_lua_call_inotify(app, watch, path);
          free(path);
        }
      }
      cursor += event_size;
      remaining -= event_size;
    }
    if (!watch->active) {
      return;
    }
  }
}
