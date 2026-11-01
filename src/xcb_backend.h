#ifndef I3D_XCB_BACKEND_H
#define I3D_XCB_BACKEND_H

#include <stddef.h>
#include <stdint.h>
#include <xcb/xcb.h>

struct i3d_xcb_atom;

// Open the shared connection on demand; atom IDs belong to that connection's
// server and must be forgotten whenever it is replaced.
struct i3d_xcb_backend {
  xcb_connection_t *connection;
  struct i3d_xcb_atom *atoms;
  size_t atom_count;
};

struct i3d_xcb_property {
  xcb_get_property_reply_t *reply;
  char *type_name;
};

// Success with a NULL reply means the property or window is absent. The caller
// owns both result allocations; failures return -1 and set errno.
int i3d_xcb_get_property(struct i3d_xcb_backend *backend, xcb_window_t window,
                         const char *name, uint64_t deadline_ms,
                         struct i3d_xcb_property *property);
void i3d_xcb_backend_destroy(struct i3d_xcb_backend *backend);

#endif
