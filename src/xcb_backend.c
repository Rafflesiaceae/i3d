#include "i3d.h"

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/xcbext.h>

#define I3D_XCB_MAX_PROPERTY_BYTES (1024U * 1024U)
#define I3D_XCB_MAX_CACHED_ATOMS 128U

struct i3d_xcb_atom {
  struct i3d_xcb_atom *next;
  xcb_atom_t id;
  char name[];
};

static uint64_t monotonic_milliseconds(void) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}

// Poll replies against one deadline shared by atom lookup and property reads.
// Reset failed connections so pending replies cannot accumulate after timeouts.
static void *wait_reply(struct i3d_xcb_backend *backend, unsigned sequence,
                        uint64_t deadline_ms) {
  if (xcb_flush(backend->connection) <= 0) {
    errno = EIO;
    goto failed;
  }
  for (;;) {
    void *reply = NULL;
    xcb_generic_error_t *error = NULL;
    if (xcb_poll_for_reply(backend->connection, sequence, &reply, &error)) {
      if (error != NULL) {
        errno = error->error_code == XCB_WINDOW ? ENOENT : EPROTO;
        free(error);
        free(reply);
        return NULL;
      }
      if (reply != NULL) {
        return reply;
      }
      errno = EIO;
      goto failed;
    }
    if (xcb_connection_has_error(backend->connection)) {
      errno = EIO;
      goto failed;
    }
    uint64_t now = monotonic_milliseconds();
    if (now >= deadline_ms) {
      errno = ETIMEDOUT;
      goto failed;
    }
    uint64_t remaining = deadline_ms - now;
    int timeout = remaining > INT_MAX ? INT_MAX : (int)remaining;
    struct pollfd fd = {.fd = xcb_get_file_descriptor(backend->connection),
                        .events = POLLIN};
    int ready = poll(&fd, 1, timeout);
    if (ready < 0 && errno != EINTR) {
      goto failed;
    }
    if (ready > 0 && (fd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0 &&
        (fd.revents & POLLIN) == 0) {
      errno = EIO;
      goto failed;
    }
  }

failed:
  {
    int saved_errno = errno;
    i3d_xcb_backend_destroy(backend);
    errno = saved_errno;
    return NULL;
  }
}

// Cache only atoms known to exist: a missing atom can be created later by an
// application. Once full, keep serving queries without growing the cache.
static void cache_atom(struct i3d_xcb_backend *backend, xcb_atom_t id,
                       const char *name) {
  if (backend->atom_count >= I3D_XCB_MAX_CACHED_ATOMS) {
    return;
  }
  size_t length = strlen(name);
  struct i3d_xcb_atom *atom = malloc(sizeof(*atom) + length + 1U);
  if (atom == NULL) {
    return;
  }
  atom->id = id;
  memcpy(atom->name, name, length + 1U);
  atom->next = backend->atoms;
  backend->atoms = atom;
  ++backend->atom_count;
}

static int intern_atom(struct i3d_xcb_backend *backend, const char *name,
                       uint64_t deadline_ms, xcb_atom_t *id) {
  for (struct i3d_xcb_atom *atom = backend->atoms; atom != NULL;
       atom = atom->next) {
    if (strcmp(atom->name, name) == 0) {
      *id = atom->id;
      return 0;
    }
  }
  size_t length = strlen(name);
  if (length == 0 || length > UINT16_MAX) {
    errno = EINVAL;
    return -1;
  }
  xcb_intern_atom_cookie_t cookie =
      xcb_intern_atom(backend->connection, true, (uint16_t)length, name);
  xcb_intern_atom_reply_t *reply =
      wait_reply(backend, cookie.sequence, deadline_ms);
  if (reply == NULL) {
    return -1;
  }
  *id = reply->atom;
  free(reply);
  if (*id != XCB_ATOM_NONE) {
    cache_atom(backend, *id, name);
  }
  return 0;
}

static char *get_atom_name(struct i3d_xcb_backend *backend, xcb_atom_t id,
                           uint64_t deadline_ms) {
  for (struct i3d_xcb_atom *atom = backend->atoms; atom != NULL;
       atom = atom->next) {
    if (atom->id == id) {
      return strdup(atom->name);
    }
  }
  xcb_get_atom_name_cookie_t cookie =
      xcb_get_atom_name(backend->connection, id);
  xcb_get_atom_name_reply_t *reply =
      wait_reply(backend, cookie.sequence, deadline_ms);
  if (reply == NULL) {
    return NULL;
  }
  char *name = strndup(xcb_get_atom_name_name(reply),
                       (size_t)xcb_get_atom_name_name_length(reply));
  free(reply);
  if (name != NULL) {
    cache_atom(backend, id, name);
  }
  return name;
}

int i3d_xcb_get_property(struct i3d_xcb_backend *backend, xcb_window_t window,
                         const char *name, uint64_t deadline_ms,
                         struct i3d_xcb_property *property) {
  *property = (struct i3d_xcb_property){0};
  if (backend->connection != NULL &&
      xcb_connection_has_error(backend->connection)) {
    i3d_xcb_backend_destroy(backend);
  }
  if (backend->connection == NULL) {
    backend->connection = xcb_connect(NULL, NULL);
    if (xcb_connection_has_error(backend->connection)) {
      i3d_xcb_backend_destroy(backend);
      errno = ENOTCONN;
      return -1;
    }
  }

  xcb_atom_t atom;
  if (intern_atom(backend, name, deadline_ms, &atom) < 0) {
    return -1;
  }
  if (atom == XCB_ATOM_NONE) {
    return 0;
  }
  // The protocol expresses the size limit in four-byte units. Reject partial
  // replies rather than silently exposing a truncated property to scripts.
  xcb_get_property_cookie_t cookie = xcb_get_property(
      backend->connection, false, window, atom, XCB_GET_PROPERTY_TYPE_ANY, 0,
      I3D_XCB_MAX_PROPERTY_BYTES / 4U);
  xcb_get_property_reply_t *reply =
      wait_reply(backend, cookie.sequence, deadline_ms);
  if (reply == NULL) {
    return errno == ENOENT ? 0 : -1;
  }
  if (reply->type == XCB_ATOM_NONE) {
    free(reply);
    return 0;
  }
  if (reply->bytes_after != 0) {
    free(reply);
    errno = EOVERFLOW;
    return -1;
  }
  if ((reply->format != 8 && reply->format != 16 && reply->format != 32) ||
      reply->value_len > I3D_XCB_MAX_PROPERTY_BYTES / (reply->format / 8U)) {
    free(reply);
    errno = EPROTO;
    return -1;
  }
  char *type_name = get_atom_name(backend, reply->type, deadline_ms);
  if (type_name == NULL) {
    free(reply);
    return -1;
  }
  property->reply = reply;
  property->type_name = type_name;
  return 0;
}

void i3d_xcb_backend_destroy(struct i3d_xcb_backend *backend) {
  if (backend->connection != NULL) {
    xcb_disconnect(backend->connection);
  }
  backend->connection = NULL;
  while (backend->atoms != NULL) {
    struct i3d_xcb_atom *next = backend->atoms->next;
    free(backend->atoms);
    backend->atoms = next;
  }
  backend->atom_count = 0;
}
