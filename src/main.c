#include "i3d.h"

#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void usage(FILE *stream) {
  fputs("i3d - small Linux/i3 daemon embedding LuaJIT configs\n\n"
        "Watches:\n"
        "  $HOME/.config/i3d/*.lua\n"
        "  (override with I3D_DIR=/path)\n\n"
        "Usage:\n"
        "  i3d [--journald] [--full-lua] [--debug]\n\n"
        "Options:\n"
        "  --journald  Prefix every log line with a syslog priority such "
        "as <3>\n"
        "  --full-lua  Load all LuaJIT libraries, including package and "
        "ffi\n"
        "              (configs are trusted native code in this mode)\n"
        "  --debug     Enable diagnostic logging (also enabled by "
        "DEBUG=1)\n"
        "  --help, -h  Show this help\n\n"
        "Environment:\n"
        "  I3D_DIR                    Override the config directory\n"
        "  DEBUG=1                    Enable diagnostic logging\n"
        "  I3D_HANDLER_MAX_STEPS      Instruction limit (default "
        "5000000)\n"
        "  I3D_HANDLER_TIMEOUT_MS     Wall-time limit in ms (default "
        "2000)\n",
        stream);
}

static int parse_u64_env(const char *name, uint64_t default_value,
                         uint64_t *value) {
  const char *text = getenv(name);
  if (text == NULL || text[0] == '\0') {
    *value = default_value;
    return 0;
  }
  if (text[0] == '-') {
    fprintf(stderr, "i3d: %s must be an unsigned integer, got %s\n", name,
            text);
    return -1;
  }
  errno = 0;
  char *end = NULL;
  unsigned long long parsed = strtoull(text, &end, 10);
  if (errno != 0 || end == text || *end != '\0') {
    fprintf(stderr, "i3d: %s must be an unsigned integer, got %s\n", name,
            text);
    return -1;
  }
  *value = (uint64_t)parsed;
  return 0;
}

static char *default_config_dir(void) {
  const char *override = getenv("I3D_DIR");
  if (override != NULL && override[0] != '\0') {
    return strdup(override);
  }
  const char *home = getenv("HOME");
  if (home == NULL || home[0] == '\0') {
    fprintf(stderr,
            "i3d: HOME is unset; set I3D_DIR to the config directory\n");
    return NULL;
  }
  size_t length = strlen(home) + sizeof("/.config/i3d");
  char *path = malloc(length);
  if (path != NULL) {
    snprintf(path, length, "%s/.config/i3d", home);
  }
  return path;
}

int main(int argc, char **argv) {
  struct i3d_app app = {
      .program_name = argv[0],
      .running = true,
      .epoll_fd = -1,
      .signal_fd = -1,
      .inotify_fd = -1,
      .inotify_wd = -1,
      .reload_timer_fd = -1,
      .reconnect_timer_fd = -1,
      .pid_netlink_fd = -1,
      .ipc = {.request_fd = -1, .request_epoll_fd = -1, .event_fd = -1},
  };

  static const struct option options[] = {
      {"journald", no_argument, NULL, 'j'},
      {"full-lua", no_argument, NULL, 'f'},
      {"debug", no_argument, NULL, 'd'},
      {"help", no_argument, NULL, 'h'},
      {NULL, 0, NULL, 0},
  };
  for (;;) {
    int option = getopt_long(argc, argv, "h", options, NULL);
    if (option < 0) {
      break;
    }
    switch (option) {
    case 'j':
      app.journald = true;
      break;
    case 'f':
      app.full_lua = true;
      break;
    case 'd':
      app.debug = true;
      break;
    case 'h':
      usage(stdout);
      return 0;
    default:
      usage(stderr);
      return 2;
    }
  }
  if (optind != argc) {
    fprintf(stderr, "i3d: unexpected argument: %s\n", argv[optind]);
    usage(stderr);
    return 2;
  }
  const char *debug_environment = getenv("DEBUG");
  if (debug_environment != NULL && strcmp(debug_environment, "1") == 0) {
    app.debug = true;
  }
  if (parse_u64_env("I3D_HANDLER_MAX_STEPS", 5000000, &app.handler_max_steps) <
          0 ||
      parse_u64_env("I3D_HANDLER_TIMEOUT_MS", 2000, &app.handler_timeout_ms) <
          0) {
    return 2;
  }
  app.config_dir = default_config_dir();
  if (app.config_dir == NULL) {
    return 1;
  }

  int result = i3d_daemon_run(&app);
  i3d_daemon_cleanup(&app);
  free(app.config_dir);
  return result == 0 ? 0 : 1;
}
