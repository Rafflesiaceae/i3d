#include "i3d.h"

#include "ipc.h"

#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#define I3D_IPC_HEADER_SIZE 14U
#define I3D_IPC_TIMEOUT_MS 2000

static const unsigned char i3_magic[6] = {'i', '3', '-', 'i', 'p', 'c'};

extern char **environ;

static uint32_t load_le32(const unsigned char *input) {
  return (uint32_t)input[0] | (uint32_t)input[1] << 8U |
         (uint32_t)input[2] << 16U | (uint32_t)input[3] << 24U;
}

static void store_le32(unsigned char *output, uint32_t value) {
  output[0] = (unsigned char)value;
  output[1] = (unsigned char)(value >> 8U);
  output[2] = (unsigned char)(value >> 16U);
  output[3] = (unsigned char)(value >> 24U);
}

static int wait_ready(int epoll_fd, int fd, uint32_t events) {
  struct epoll_event interest = {
      .events = events | EPOLLERR | EPOLLHUP | EPOLLRDHUP, .data.fd = fd};
  if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &interest) < 0) {
    return -1;
  }
  struct epoll_event event;
  for (;;) {
    int count = epoll_wait(epoll_fd, &event, 1, I3D_IPC_TIMEOUT_MS);
    if (count > 0) {
      if ((event.events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) != 0 &&
          (event.events & events) == 0) {
        errno = ECONNRESET;
        return -1;
      }
      if ((event.events & events) != 0) {
        return 0;
      }
      continue;
    }
    if (count == 0) {
      errno = ETIMEDOUT;
      return -1;
    }
    if (errno != EINTR) {
      return -1;
    }
  }
}

static int connect_path(const char *path, int *wait_epoll) {
  if (strlen(path) >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
    errno = ENAMETOOLONG;
    return -1;
  }

  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    return -1;
  }
  int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
  if (epoll_fd < 0) {
    close(fd);
    return -1;
  }

  struct epoll_event event = {.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP,
                              .data.fd = fd};
  if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &event) < 0) {
    close(epoll_fd);
    close(fd);
    return -1;
  }

  struct sockaddr_un address = {.sun_family = AF_UNIX};
  memcpy(address.sun_path, path, strlen(path) + 1U);
  int connect_result =
      connect(fd, (struct sockaddr *)&address, sizeof(address));
  if (connect_result < 0) {
    if (errno != EINPROGRESS || wait_ready(epoll_fd, fd, EPOLLOUT) < 0) {
      close(epoll_fd);
      close(fd);
      return -1;
    }
  }

  int socket_error = 0;
  socklen_t error_len = sizeof(socket_error);
  if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &error_len) < 0 ||
      socket_error != 0) {
    if (socket_error != 0) {
      errno = socket_error;
    }
    close(epoll_fd);
    close(fd);
    return -1;
  }
  *wait_epoll = epoll_fd;
  return fd;
}

static int write_all(int fd, int epoll_fd, const void *data, size_t length) {
  const unsigned char *cursor = data;
  while (length != 0) {
    ssize_t written = send(fd, cursor, length, MSG_NOSIGNAL);
    if (written > 0) {
      cursor += (size_t)written;
      length -= (size_t)written;
      continue;
    }
    if (written < 0 && errno == EINTR) {
      continue;
    }
    if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      if (wait_ready(epoll_fd, fd, EPOLLOUT) == 0) {
        continue;
      }
    }
    return -1;
  }
  return 0;
}

static int read_all(int fd, int epoll_fd, void *data, size_t length) {
  unsigned char *cursor = data;
  while (length != 0) {
    ssize_t received = recv(fd, cursor, length, 0);
    if (received > 0) {
      cursor += (size_t)received;
      length -= (size_t)received;
      continue;
    }
    if (received == 0) {
      errno = ECONNRESET;
      return -1;
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      if (wait_ready(epoll_fd, fd, EPOLLIN) == 0) {
        continue;
      }
    }
    return -1;
  }
  return 0;
}

static int roundtrip_fd(int fd, int epoll_fd, uint32_t type,
                        const char *payload, size_t payload_len, char **reply,
                        size_t *reply_len) {
  if (payload_len > UINT32_MAX) {
    errno = EOVERFLOW;
    return -1;
  }

  unsigned char header[I3D_IPC_HEADER_SIZE];
  memcpy(header, i3_magic, sizeof(i3_magic));
  store_le32(header + 6, (uint32_t)payload_len);
  store_le32(header + 10, type);
  if (write_all(fd, epoll_fd, header, sizeof(header)) < 0 ||
      (payload_len != 0 && write_all(fd, epoll_fd, payload, payload_len) < 0)) {
    return -1;
  }

  if (read_all(fd, epoll_fd, header, sizeof(header)) < 0) {
    return -1;
  }
  if (memcmp(header, i3_magic, sizeof(i3_magic)) != 0) {
    errno = EPROTO;
    return -1;
  }
  uint32_t length = load_le32(header + 6);
  uint32_t response_type = load_le32(header + 10);
  if (length > I3D_IPC_MAX_PAYLOAD || response_type != type) {
    errno = EPROTO;
    return -1;
  }

  char *body = malloc((size_t)length + 1U);
  if (body == NULL) {
    return -1;
  }
  if (length != 0 && read_all(fd, epoll_fd, body, length) < 0) {
    free(body);
    return -1;
  }
  body[length] = '\0';
  *reply = body;
  *reply_len = length;
  return 0;
}

static void close_request(struct i3d_ipc *ipc) {
  if (ipc->request_fd >= 0) {
    close(ipc->request_fd);
    ipc->request_fd = -1;
  }
  if (ipc->request_epoll_fd >= 0) {
    close(ipc->request_epoll_fd);
    ipc->request_epoll_fd = -1;
  }
}

static int ensure_request(struct i3d_app *app) {
  if (app->ipc.request_fd >= 0) {
    return 0;
  }
  if (app->ipc.socket_path == NULL) {
    errno = ENOTCONN;
    return -1;
  }
  app->ipc.request_fd =
      connect_path(app->ipc.socket_path, &app->ipc.request_epoll_fd);
  if (app->ipc.request_fd < 0) {
    i3d_log(app, I3D_LOG_ERROR, "connect i3 IPC request socket: %s",
            strerror(errno));
    return -1;
  }
  return 0;
}

int i3d_ipc_request(struct i3d_app *app, uint32_t type, const char *payload,
                    size_t payload_len, char **reply, size_t *reply_len) {
  for (unsigned attempt = 0; attempt < 2; ++attempt) {
    if (ensure_request(app) == 0 &&
        roundtrip_fd(app->ipc.request_fd, app->ipc.request_epoll_fd, type,
                     payload, payload_len, reply, reply_len) == 0) {
      i3d_log(app, I3D_LOG_DEBUG,
              "i3 IPC roundtrip type=%u payload=%zu reply=%zu", type,
              payload_len, *reply_len);
      return 0;
    }
    int saved_errno = errno;
    close_request(&app->ipc);
    errno = saved_errno;
    i3d_log(app, I3D_LOG_DEBUG, "i3 IPC request failed; reconnecting: %s",
            strerror(errno));
  }
  i3d_log(app, I3D_LOG_ERROR, "i3 IPC request failed: %s", strerror(errno));
  return -1;
}

static bool candidate_add(char ***items, size_t *count, size_t *capacity,
                          const char *path) {
  if (path == NULL || path[0] == '\0') {
    return true;
  }
  for (size_t index = 0; index < *count; ++index) {
    if (strcmp((*items)[index], path) == 0) {
      return true;
    }
  }
  if (*count == *capacity) {
    size_t next = *capacity == 0 ? 8U : *capacity * 2U;
    char **grown = realloc(*items, next * sizeof(*grown));
    if (grown == NULL) {
      return false;
    }
    *items = grown;
    *capacity = next;
  }
  (*items)[*count] = strdup(path);
  if ((*items)[*count] == NULL) {
    return false;
  }
  ++*count;
  return true;
}

static void candidates_glob(char ***items, size_t *count, size_t *capacity,
                            const char *pattern) {
  glob_t matches = {0};
  if (glob(pattern, 0, NULL, &matches) == 0) {
    for (size_t index = 0; index < matches.gl_pathc; ++index) {
      if (!candidate_add(items, count, capacity, matches.gl_pathv[index])) {
        break;
      }
    }
  }
  globfree(&matches);
}

static int connect_candidates(struct i3d_app *app, char **candidates,
                              size_t begin, size_t count) {
  for (size_t index = begin; index < count; ++index) {
    int wait_epoll = -1;
    int fd = connect_path(candidates[index], &wait_epoll);
    if (fd >= 0) {
      app->ipc.socket_path = strdup(candidates[index]);
      close(wait_epoll);
      close(fd);
      if (app->ipc.socket_path != NULL) {
        i3d_log(app, I3D_LOG_DEBUG, "using i3 socket %s", app->ipc.socket_path);
        return 0;
      }
      return -1;
    }
    i3d_log(app, I3D_LOG_DEBUG, "i3 socket candidate %s failed: %s",
            candidates[index], strerror(errno));
  }
  return -1;
}

static bool query_i3_socket_path(char *output, size_t capacity) {
  int pipe_fds[2];
  if (capacity < 2U || pipe2(pipe_fds, O_CLOEXEC) < 0) {
    return false;
  }
  posix_spawn_file_actions_t actions;
  int action_error = posix_spawn_file_actions_init(&actions);
  bool actions_initialized = action_error == 0;
  if (action_error == 0) {
    action_error =
        posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], STDOUT_FILENO);
  }
  if (action_error == 0) {
    action_error = posix_spawn_file_actions_addclose(&actions, pipe_fds[0]);
  }
  if (action_error == 0) {
    action_error = posix_spawn_file_actions_addopen(&actions, STDERR_FILENO,
                                                    "/dev/null", O_WRONLY, 0);
  }
  char *const arguments[] = {"i3", "--get-socketpath", NULL};
  pid_t child = -1;
  int spawn_error = action_error;
  if (action_error == 0) {
    spawn_error =
        posix_spawnp(&child, arguments[0], &actions, NULL, arguments, environ);
  }
  if (actions_initialized) {
    posix_spawn_file_actions_destroy(&actions);
  }
  close(pipe_fds[1]);
  if (spawn_error != 0) {
    close(pipe_fds[0]);
    return false;
  }

  int flags = fcntl(pipe_fds[0], F_GETFL);
  if (flags >= 0) {
    fcntl(pipe_fds[0], F_SETFL, flags | O_NONBLOCK);
  }
  int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
  struct epoll_event interest = {.events = EPOLLIN | EPOLLHUP | EPOLLERR,
                                 .data.fd = pipe_fds[0]};
  size_t length = 0;
  if (epoll_fd >= 0 &&
      epoll_ctl(epoll_fd, EPOLL_CTL_ADD, pipe_fds[0], &interest) == 0) {
    struct epoll_event event;
    int ready;
    do {
      ready = epoll_wait(epoll_fd, &event, 1, 400);
    } while (ready < 0 && errno == EINTR);
    if (ready > 0) {
      for (;;) {
        ssize_t count =
            read(pipe_fds[0], output + length, capacity - length - 1U);
        if (count > 0) {
          length += (size_t)count;
          if (length + 1U == capacity) {
            break;
          }
          continue;
        }
        if (count < 0 && errno == EINTR) {
          continue;
        }
        break;
      }
    }
  }
  if (epoll_fd >= 0) {
    close(epoll_fd);
  }
  close(pipe_fds[0]);

  int status;
  pid_t waited = waitpid(child, &status, WNOHANG);
  if (waited == 0) {
    // The discovery fallback has already exceeded its deadline. SIGKILL
    // provides a hard bound even if an unexpected i3 wrapper ignores TERM.
    kill(child, SIGKILL);
    do {
      waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
  }
  output[length] = '\0';
  output[strcspn(output, "\r\n")] = '\0';
  return output[0] != '\0';
}

int i3d_ipc_discover(struct i3d_app *app) {
  char **candidates = NULL;
  size_t count = 0;
  size_t capacity = 0;
  candidate_add(&candidates, &count, &capacity, getenv("I3SOCK"));

  char pattern[4096];
  const char *runtime = getenv("XDG_RUNTIME_DIR");
  if (runtime != NULL && runtime[0] != '\0' &&
      snprintf(pattern, sizeof(pattern), "%s/i3/ipc-socket.*", runtime) <
          (int)sizeof(pattern)) {
    candidates_glob(&candidates, &count, &capacity, pattern);
  }
  if (snprintf(pattern, sizeof(pattern), "/run/user/%u/i3/ipc-socket.*",
               (unsigned)getuid()) < (int)sizeof(pattern)) {
    candidates_glob(&candidates, &count, &capacity, pattern);
  }

  int result = connect_candidates(app, candidates, 0, count);
  if (result < 0) {
    // i3 itself is the authoritative final fallback. Spawn it only when the
    // environment and conventional paths did not yield a live socket.
    size_t previous_count = count;
    char line[4096];
    if (query_i3_socket_path(line, sizeof(line))) {
      candidate_add(&candidates, &count, &capacity, line);
    }
    result = connect_candidates(app, candidates, previous_count, count);
  }
  for (size_t index = 0; index < count; ++index) {
    free(candidates[index]);
  }
  free(candidates);
  if (result < 0) {
    i3d_log(app, I3D_LOG_ERROR,
            "could not connect to an i3 IPC socket candidate");
  }
  return result;
}

int i3d_ipc_verify(struct i3d_app *app) {
  char *reply = NULL;
  size_t reply_len = 0;
  int result =
      i3d_ipc_request(app, I3D_IPC_GET_VERSION, "", 0, &reply, &reply_len);
  free(reply);
  return result;
}

static int subscribe_socket(struct i3d_app *app, int *socket_out) {
  static const char subscription[] =
      "[\"workspace\",\"output\",\"mode\",\"window\","
      "\"barconfig_update\",\"binding\"]";
  int wait_epoll = -1;
  int fd = connect_path(app->ipc.socket_path, &wait_epoll);
  if (fd < 0) {
    return -1;
  }
  char *reply = NULL;
  size_t reply_len = 0;
  int result = roundtrip_fd(fd, wait_epoll, I3D_IPC_SUBSCRIBE, subscription,
                            sizeof(subscription) - 1U, &reply, &reply_len);
  if (result == 0) {
    yyjson_doc *doc = yyjson_read(reply, reply_len, 0);
    yyjson_val *root = doc == NULL ? NULL : yyjson_doc_get_root(doc);
    yyjson_val *success = root == NULL ? NULL : yyjson_obj_get(root, "success");
    if (!yyjson_is_true(success)) {
      errno = EPROTO;
      result = -1;
    }
    yyjson_doc_free(doc);
  }
  free(reply);
  close(wait_epoll);
  if (result < 0) {
    close(fd);
    return -1;
  }
  *socket_out = fd;
  return 0;
}

int i3d_ipc_subscribe(struct i3d_app *app) {
  if (app->ipc.socket_path == NULL) {
    errno = ENOTCONN;
    return -1;
  }
  if (subscribe_socket(app, &app->ipc.event_fd) < 0) {
    i3d_log(app, I3D_LOG_ERROR, "subscribe to i3 events: %s", strerror(errno));
    return -1;
  }
  app->event_source = (struct i3d_source){
      .kind = I3D_SOURCE_I3_EVENT,
      .fd = app->ipc.event_fd,
      .owner = app,
  };
  struct epoll_event event = {
      .events = EPOLLIN | EPOLLRDHUP | EPOLLERR,
      .data.ptr = &app->event_source,
  };
  if (epoll_ctl(app->epoll_fd, EPOLL_CTL_ADD, app->ipc.event_fd, &event) < 0) {
    close(app->ipc.event_fd);
    app->ipc.event_fd = -1;
    return -1;
  }
  return 0;
}

int i3d_ipc_reconnect_events(struct i3d_app *app) {
  if (app->ipc.event_fd >= 0) {
    epoll_ctl(app->epoll_fd, EPOLL_CTL_DEL, app->ipc.event_fd, NULL);
    close(app->ipc.event_fd);
    app->ipc.event_fd = -1;
  }
  app->ipc.event_input.len = 0;
  if (app->ipc.socket_path != NULL && i3d_ipc_subscribe(app) == 0) {
    return 0;
  }

  // i3 uses its PID in the default socket name, so rediscover after a
  // compositor restart rather than retrying an obsolete path forever.
  close_request(&app->ipc);
  free(app->ipc.socket_path);
  app->ipc.socket_path = NULL;
  if (i3d_ipc_discover(app) < 0 || i3d_ipc_verify(app) < 0) {
    return -1;
  }
  return i3d_ipc_subscribe(app);
}

static int buffer_reserve(struct i3d_buffer *buffer, size_t additional) {
  if (additional > I3D_IPC_MAX_PAYLOAD ||
      buffer->len > I3D_IPC_MAX_PAYLOAD - additional) {
    errno = EOVERFLOW;
    return -1;
  }
  size_t needed = buffer->len + additional;
  if (needed <= buffer->cap) {
    return 0;
  }
  size_t capacity = buffer->cap == 0 ? 65536U : buffer->cap;
  while (capacity < needed) {
    capacity *= 2U;
  }
  char *grown = realloc(buffer->data, capacity);
  if (grown == NULL) {
    return -1;
  }
  buffer->data = grown;
  buffer->cap = capacity;
  return 0;
}

static int dispatch_buffered_events(struct i3d_app *app) {
  struct i3d_buffer *input = &app->ipc.event_input;
  size_t consumed = 0;
  while (input->len - consumed >= I3D_IPC_HEADER_SIZE) {
    unsigned char *header = (unsigned char *)input->data + consumed;
    if (memcmp(header, i3_magic, sizeof(i3_magic)) != 0) {
      errno = EPROTO;
      return -1;
    }
    uint32_t length = load_le32(header + 6);
    uint32_t type = load_le32(header + 10);
    if (length > I3D_IPC_MAX_PAYLOAD) {
      errno = EOVERFLOW;
      return -1;
    }
    size_t frame_len = I3D_IPC_HEADER_SIZE + (size_t)length;
    if (input->len - consumed < frame_len) {
      break;
    }
    i3d_dispatch_json_event(
        app, type, input->data + consumed + I3D_IPC_HEADER_SIZE, length);
    consumed += frame_len;
  }
  if (consumed != 0) {
    memmove(input->data, input->data + consumed, input->len - consumed);
    input->len -= consumed;
  }
  return 0;
}

int i3d_ipc_drain_events(struct i3d_app *app) {
  struct i3d_buffer *input = &app->ipc.event_input;
  for (;;) {
    if (buffer_reserve(input, 65536U) < 0) {
      return -1;
    }
    ssize_t count = recv(app->ipc.event_fd, input->data + input->len,
                         input->cap - input->len, 0);
    if (count > 0) {
      input->len += (size_t)count;
      if (dispatch_buffered_events(app) < 0) {
        return -1;
      }
      continue;
    }
    if (count == 0) {
      errno = ECONNRESET;
      return -1;
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return 0;
    }
    return -1;
  }
}

void i3d_ipc_close(struct i3d_app *app) {
  close_request(&app->ipc);
  if (app->ipc.event_fd >= 0) {
    close(app->ipc.event_fd);
    app->ipc.event_fd = -1;
  }
  free(app->ipc.event_input.data);
  app->ipc.event_input = (struct i3d_buffer){0};
  free(app->ipc.socket_path);
  app->ipc.socket_path = NULL;
}
