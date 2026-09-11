#include "i3d.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/cn_proc.h>
#include <linux/connector.h>
#include <linux/netlink.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <unistd.h>

static int compare_pid(const void *left, const void *right) {
  pid_t a = *(const pid_t *)left;
  pid_t b = *(const pid_t *)right;
  return (a > b) - (a < b);
}

static int snapshot_pids(pid_t **output, size_t *output_len) {
  DIR *proc = opendir("/proc");
  if (proc == NULL) {
    return -1;
  }
  pid_t *items = NULL;
  size_t count = 0;
  size_t capacity = 0;
  errno = 0;
  for (struct dirent *entry = readdir(proc); entry != NULL;
       entry = readdir(proc)) {
    char *end = NULL;
    errno = 0;
    long value = strtol(entry->d_name, &end, 10);
    if (errno != 0 || end == entry->d_name || *end != '\0' || value <= 0) {
      continue;
    }
    if (count == capacity) {
      size_t next = capacity == 0 ? 1024U : capacity * 2U;
      pid_t *grown = realloc(items, next * sizeof(*grown));
      if (grown == NULL) {
        free(items);
        closedir(proc);
        return -1;
      }
      items = grown;
      capacity = next;
    }
    items[count++] = (pid_t)value;
  }
  int saved_errno = errno;
  closedir(proc);
  if (saved_errno != 0) {
    free(items);
    errno = saved_errno;
    return -1;
  }
  if (count != 0) {
    qsort(items, count, sizeof(*items), compare_pid);
  }
  *output = items;
  *output_len = count;
  return 0;
}

static pid_t read_ppid(pid_t pid) {
  char path[64];
  snprintf(path, sizeof(path), "/proc/%d/stat", pid);
  FILE *file = fopen(path, "re");
  if (file == NULL) {
    return 0;
  }
  char *line = NULL;
  size_t capacity = 0;
  ssize_t length = getline(&line, &capacity, file);
  fclose(file);
  if (length <= 0) {
    free(line);
    return 0;
  }
  char *close_paren = strrchr(line, ')');
  char state;
  int parent = 0;
  if (close_paren == NULL ||
      sscanf(close_paren + 1, " %c %d", &state, &parent) != 2) {
    parent = 0;
  }
  free(line);
  return parent > 0 ? (pid_t)parent : 0;
}

bool i3d_pid_is_ancestor(pid_t ancestor, pid_t descendant) {
  if (ancestor <= 0 || descendant <= 0) {
    return false;
  }
  pid_t current = descendant;
  // PID ancestry cannot legitimately be deeper than the PID namespace's
  // numeric range; this bound also makes corrupt /proc data harmless.
  for (unsigned depth = 0; depth < 65536U; ++depth) {
    if (current == ancestor) {
      return true;
    }
    if (current <= 1) {
      return false;
    }
    pid_t parent = read_ppid(current);
    if (parent <= 0 || parent == current) {
      return false;
    }
    current = parent;
  }
  return false;
}

static int netlink_control(int fd, enum proc_cn_mcast_op operation) {
  // cn_msg ends in a flexible array, so spell out its fixed wire header when
  // embedding the multicast operation in a stack message.
  struct fixed_cn_message {
    struct cb_id id;
    uint32_t seq;
    uint32_t ack;
    uint16_t len;
    uint16_t flags;
  };
  struct {
    struct nlmsghdr nl;
    struct fixed_cn_message cn;
    enum proc_cn_mcast_op operation;
  } message = {0};
  message.nl.nlmsg_len = sizeof(message);
  message.nl.nlmsg_type = NLMSG_DONE;
  message.nl.nlmsg_pid = (uint32_t)getpid();
  message.cn.id.idx = CN_IDX_PROC;
  message.cn.id.val = CN_VAL_PROC;
  message.cn.len = sizeof(operation);
  message.operation = operation;

  struct sockaddr_nl kernel = {.nl_family = AF_NETLINK};
  return sendto(fd, &message, sizeof(message), 0, (struct sockaddr *)&kernel,
                sizeof(kernel)) < 0
             ? -1
             : 0;
}

static int ensure_netlink(struct i3d_app *app) {
  if (app->pid_netlink_fd >= 0) {
    return 0;
  }
  int fd = socket(PF_NETLINK, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                  NETLINK_CONNECTOR);
  if (fd < 0) {
    return -1;
  }
  int receive_buffer = 4 * 1024 * 1024;
  setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &receive_buffer,
             sizeof(receive_buffer));
  struct sockaddr_nl address = {
      .nl_family = AF_NETLINK,
      .nl_pid = (uint32_t)getpid(),
      .nl_groups = CN_IDX_PROC,
  };
  if (bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0 ||
      netlink_control(fd, PROC_CN_MCAST_LISTEN) < 0) {
    close(fd);
    return -1;
  }
  app->pid_netlink_fd = fd;
  app->pid_netlink_source = (struct i3d_source){
      .kind = I3D_SOURCE_PID_NETLINK, .fd = fd, .owner = app};
  struct epoll_event event = {.events = EPOLLIN | EPOLLERR,
                              .data.ptr = &app->pid_netlink_source};
  if (epoll_ctl(app->epoll_fd, EPOLL_CTL_ADD, fd, &event) < 0) {
    close(fd);
    app->pid_netlink_fd = -1;
    return -1;
  }
  return 0;
}

static int setup_poll(struct i3d_app *app, struct i3d_pid_watch *watch) {
  if (snapshot_pids(&watch->snapshot, &watch->snapshot_len) < 0) {
    return -1;
  }
  watch->timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
  if (watch->timer_fd < 0) {
    free(watch->snapshot);
    watch->snapshot = NULL;
    return -1;
  }
  struct itimerspec interval = {0};
  interval.it_value.tv_sec = watch->poll_interval_ms / 1000U;
  interval.it_value.tv_nsec =
      (long)(watch->poll_interval_ms % 1000U) * 1000000L;
  interval.it_interval = interval.it_value;
  if (timerfd_settime(watch->timer_fd, 0, &interval, NULL) < 0) {
    close(watch->timer_fd);
    watch->timer_fd = -1;
    free(watch->snapshot);
    watch->snapshot = NULL;
    return -1;
  }
  watch->source = (struct i3d_source){
      .kind = I3D_SOURCE_PID_POLL, .fd = watch->timer_fd, .owner = watch};
  struct epoll_event event = {.events = EPOLLIN, .data.ptr = &watch->source};
  if (epoll_ctl(app->epoll_fd, EPOLL_CTL_ADD, watch->timer_fd, &event) < 0) {
    close(watch->timer_fd);
    watch->timer_fd = -1;
    free(watch->snapshot);
    watch->snapshot = NULL;
    return -1;
  }
  watch->polling = true;
  return 0;
}

int i3d_pid_watch_add(struct i3d_app *app, struct i3d_script *script,
                      int callback_ref, unsigned poll_interval_ms,
                      bool force_poll, struct i3d_pid_watch **out) {
  struct i3d_pid_watch *watch = calloc(1, sizeof(*watch));
  if (watch == NULL) {
    return -1;
  }
  watch->script = script;
  watch->callback_ref = callback_ref;
  watch->active = true;
  watch->poll_interval_ms = poll_interval_ms == 0 ? 100U : poll_interval_ms;
  watch->timer_fd = -1;

  if (force_poll || ensure_netlink(app) < 0) {
    if (!force_poll) {
      i3d_log(app, I3D_LOG_WARNING,
              "process connector unavailable (%s); using /proc polling",
              strerror(errno));
    }
    if (setup_poll(app, watch) < 0) {
      free(watch);
      return -1;
    }
  }
  watch->next = app->pid_watches;
  app->pid_watches = watch;
  *out = watch;
  return 0;
}

void i3d_pid_watch_stop(struct i3d_app *app, struct i3d_pid_watch *watch) {
  if (watch == NULL || !watch->active) {
    return;
  }
  watch->active = false;
  if (watch->timer_fd >= 0) {
    epoll_ctl(app->epoll_fd, EPOLL_CTL_DEL, watch->timer_fd, NULL);
    close(watch->timer_fd);
    watch->timer_fd = -1;
  }
  free(watch->snapshot);
  watch->snapshot = NULL;
  watch->snapshot_len = 0;
}

void i3d_pid_watch_remove_script(struct i3d_app *app,
                                 struct i3d_script *script) {
  struct i3d_pid_watch **link = &app->pid_watches;
  while (*link != NULL) {
    struct i3d_pid_watch *watch = *link;
    if (watch->script != script) {
      link = &watch->next;
      continue;
    }
    i3d_pid_watch_stop(app, watch);
    luaL_unref(script->lua, LUA_REGISTRYINDEX, watch->callback_ref);
    *link = watch->next;
    free(watch);
  }
}

void i3d_pid_dispatch_poll(struct i3d_app *app, struct i3d_pid_watch *watch) {
  uint64_t expirations;
  if (!watch->active ||
      read(watch->timer_fd, &expirations, sizeof(expirations)) < 0) {
    return;
  }
  pid_t *current = NULL;
  size_t current_len = 0;
  if (snapshot_pids(&current, &current_len) < 0) {
    i3d_log(app, I3D_LOG_WARNING, "scan /proc: %s", strerror(errno));
    return;
  }
  size_t old_index = 0;
  for (size_t index = 0; index < current_len; ++index) {
    while (old_index < watch->snapshot_len &&
           watch->snapshot[old_index] < current[index]) {
      ++old_index;
    }
    if (old_index >= watch->snapshot_len ||
        watch->snapshot[old_index] != current[index]) {
      i3d_lua_call_pid(app, watch, current[index], read_ppid(current[index]));
      if (!watch->active) {
        break;
      }
    }
  }
  if (!watch->active) {
    free(current);
    return;
  }
  free(watch->snapshot);
  watch->snapshot = current;
  watch->snapshot_len = current_len;
}

static void dispatch_fork(struct i3d_app *app, pid_t child, pid_t parent) {
  for (struct i3d_pid_watch *watch = app->pid_watches; watch != NULL;
       watch = watch->next) {
    if (watch->active && !watch->polling) {
      i3d_lua_call_pid(app, watch, child, parent);
    }
  }
}

void i3d_pid_dispatch_netlink(struct i3d_app *app) {
  _Alignas(struct nlmsghdr) unsigned char buffer[16384];
  for (;;) {
    ssize_t length = recv(app->pid_netlink_fd, buffer, sizeof(buffer), 0);
    if (length < 0 && errno == EINTR) {
      continue;
    }
    if (length < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      return;
    }
    if (length < 0 && errno == ENOBUFS) {
      i3d_log(app, I3D_LOG_WARNING,
              "process connector overflow; fork events were dropped");
      return;
    }
    if (length <= 0) {
      return;
    }
    for (struct nlmsghdr *nl = (struct nlmsghdr *)buffer;
         NLMSG_OK(nl, (unsigned)length); nl = NLMSG_NEXT(nl, length)) {
      size_t payload_len = (size_t)NLMSG_PAYLOAD(nl, 0);
      if (nl->nlmsg_type == NLMSG_ERROR ||
          payload_len < sizeof(struct cn_msg)) {
        continue;
      }
      struct cn_msg *cn = NLMSG_DATA(nl);
      if (cn->id.idx != CN_IDX_PROC || cn->id.val != CN_VAL_PROC ||
          cn->len < sizeof(struct proc_event) ||
          cn->len > payload_len - sizeof(*cn)) {
        continue;
      }
      // Connector payload begins at a four-byte-aligned offset while
      // proc_event requires eight-byte alignment on 64-bit Linux. Copy into
      // aligned storage instead of dereferencing cn->data directly.
      struct proc_event event;
      memcpy(&event, cn->data, sizeof(event));
      if (event.what == PROC_EVENT_FORK) {
        dispatch_fork(app, (pid_t)event.event_data.fork.child_pid,
                      (pid_t)event.event_data.fork.parent_pid);
      }
    }
  }
}
