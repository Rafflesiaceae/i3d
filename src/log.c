#include "i3d.h"

#include "ipc.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

void i3d_log(struct i3d_app *app, enum i3d_log_level level, const char *format,
             ...) {
  if (level == I3D_LOG_DEBUG && !app->debug) {
    return;
  }

  va_list arguments;
  va_start(arguments, format);
  char *message = NULL;
  int formatted = vasprintf(&message, format, arguments);
  va_end(arguments);

  if (formatted < 0) {
    // Logging must remain usable while reporting an allocation failure.
    message = NULL;
  } else {
    // journald's native stream protocol applies the priority prefix per line.
    // Flatten user-controlled newlines so one call cannot forge extra records.
    for (char *cursor = message; *cursor != '\0'; ++cursor) {
      if (*cursor == '\r' || *cursor == '\n') {
        *cursor = ' ';
      }
    }
  }

  FILE *stream = stderr;
  if (app->journald) {
    fprintf(stream, "<%d>", level);
  }
  fputs(level == I3D_LOG_DEBUG ? "i3d[debug] " : "i3d ", stream);
  fputs(message == NULL ? "could not format log message" : message, stream);
  fputc('\n', stream);
  fflush(stream);
  free(message);
}

const char *i3d_event_name(enum i3d_event_type type) {
  static const char *const names[I3D_EVENT_COUNT] = {
      [I3D_EVENT_WORKSPACE] = "workspace",
      [I3D_EVENT_OUTPUT] = "output",
      [I3D_EVENT_MODE] = "mode",
      [I3D_EVENT_WINDOW] = "window",
      [I3D_EVENT_BARCONFIG_UPDATE] = "barconfig_update",
      [I3D_EVENT_BINDING] = "binding",
  };
  return type < I3D_EVENT_COUNT ? names[type] : "unknown";
}

int i3d_event_from_ipc(uint32_t ipc_type, enum i3d_event_type *type) {
  if ((ipc_type & I3D_IPC_EVENT_BIT) == 0) {
    return -1;
  }
  uint32_t value = ipc_type & ~I3D_IPC_EVENT_BIT;
  if (value >= I3D_EVENT_COUNT) {
    return -1;
  }
  *type = (enum i3d_event_type)value;
  return 0;
}
