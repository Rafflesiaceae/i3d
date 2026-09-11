#ifndef I3D_XCB_BACKEND_H
#define I3D_XCB_BACKEND_H

struct i3d_app;

// The XCB seam intentionally mirrors an event-loop backend. A future backend
// can expose xcb_get_file_descriptor() here without changing daemon ownership
// or Lua APIs.
struct i3d_xcb_backend {
  int fd;
  void *connection;
};

int i3d_xcb_backend_init(struct i3d_app *app, struct i3d_xcb_backend *backend);
void i3d_xcb_backend_dispatch(struct i3d_app *app,
                              struct i3d_xcb_backend *backend);
void i3d_xcb_backend_destroy(struct i3d_xcb_backend *backend);

#endif
