#include "xcb_backend.h"

#include "i3d.h"

int i3d_xcb_backend_init(struct i3d_app *app, struct i3d_xcb_backend *backend) {
  (void)app;
  backend->fd = -1;
  backend->connection = NULL;
  return 0;
}

void i3d_xcb_backend_dispatch(struct i3d_app *app,
                              struct i3d_xcb_backend *backend) {
  (void)app;
  (void)backend;
}

void i3d_xcb_backend_destroy(struct i3d_xcb_backend *backend) {
  backend->fd = -1;
  backend->connection = NULL;
}
