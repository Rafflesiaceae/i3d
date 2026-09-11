#include "i3d.h"

#include "ipc.h"

#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <lualib.h>
#include <math.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

static char script_registry_key;

struct find_matcher {
  char *key;
  enum { MATCH_BOOL, MATCH_INTEGER, MATCH_STRING } kind;
  union {
    bool boolean;
    int64_t integer;
    char *string;
  } value;
};

struct find_query {
  struct find_matcher *matchers;
  size_t matcher_count;
  char **fields;
  size_t field_count;
  size_t limit;
  size_t found;
};

static yyjson_doc *acquire_tree(lua_State *lua, bool *owned);

static struct i3d_script *current_script(lua_State *lua) {
  lua_pushlightuserdata(lua, &script_registry_key);
  lua_rawget(lua, LUA_REGISTRYINDEX);
  struct i3d_script *script = lua_touserdata(lua, -1);
  lua_pop(lua, 1);
  return script;
}

static int absolute_index(lua_State *lua, int index) {
  return index < 0 && index > LUA_REGISTRYINDEX ? lua_gettop(lua) + index + 1
                                                : index;
}

static void push_json(lua_State *lua, yyjson_val *value, unsigned depth) {
  if (value == NULL || yyjson_is_null(value)) {
    lua_pushnil(lua);
  } else if (yyjson_is_bool(value)) {
    lua_pushboolean(lua, yyjson_get_bool(value));
  } else if (yyjson_is_sint(value)) {
    lua_pushnumber(lua, (lua_Number)yyjson_get_sint(value));
  } else if (yyjson_is_uint(value)) {
    lua_pushnumber(lua, (lua_Number)yyjson_get_uint(value));
  } else if (yyjson_is_real(value)) {
    lua_pushnumber(lua, yyjson_get_real(value));
  } else if (yyjson_is_str(value)) {
    lua_pushlstring(lua, yyjson_get_str(value), yyjson_get_len(value));
  } else if (depth >= 256U) {
    // i3 trees are shallow; bounding recursion protects the C stack from
    // adversarial raw-query responses without changing normal results.
    lua_pushnil(lua);
  } else if (yyjson_is_arr(value)) {
    lua_createtable(lua, (int)yyjson_arr_size(value), 0);
    size_t index, maximum;
    yyjson_val *item;
    yyjson_arr_foreach(value, index, maximum, item) {
      push_json(lua, item, depth + 1U);
      lua_rawseti(lua, -2, (int)index + 1);
    }
  } else if (yyjson_is_obj(value)) {
    lua_createtable(lua, 0, (int)yyjson_obj_size(value));
    size_t index, maximum;
    yyjson_val *key;
    yyjson_val *item;
    yyjson_obj_foreach(value, index, maximum, key, item) {
      lua_pushlstring(lua, yyjson_get_str(key), yyjson_get_len(key));
      push_json(lua, item, depth + 1U);
      lua_rawset(lua, -3);
    }
  } else {
    lua_pushnil(lua);
  }
}

static uint64_t monotonic_milliseconds(void) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}

static uint64_t timespec_milliseconds(const struct timespec *value) {
  return (uint64_t)value->tv_sec * 1000U + (uint64_t)value->tv_nsec / 1000000U;
}

static void execution_hook(lua_State *lua, lua_Debug *debug_record) {
  (void)debug_record;
  struct i3d_script *script = current_script(lua);
  if (script == NULL) {
    luaL_error(lua, "i3d execution context is missing");
    return;
  }
  struct i3d_app *app = script->app;
  script->hook_steps += 1000U;
  if (app->handler_max_steps != 0 &&
      script->hook_steps >= app->handler_max_steps) {
    luaL_error(lua, "instruction limit exceeded in %s", script->base);
    return;
  }
  if (app->handler_timeout_ms != 0 &&
      monotonic_milliseconds() >=
          timespec_milliseconds(&script->hook_deadline)) {
    luaL_error(lua, "timeout after %llu ms in %s",
               (unsigned long long)app->handler_timeout_ms, script->base);
  }
}

static int guarded_pcall(struct i3d_script *script, int arguments,
                         int results) {
  struct i3d_app *app = script->app;
  script->hook_steps = 0;
  clock_gettime(CLOCK_MONOTONIC, &script->hook_deadline);
  uint64_t deadline_ns = (uint64_t)script->hook_deadline.tv_nsec +
                         app->handler_timeout_ms * 1000000U;
  script->hook_deadline.tv_sec += (time_t)(deadline_ns / 1000000000U);
  script->hook_deadline.tv_nsec = (long)(deadline_ns % 1000000000U);
  if (app->handler_max_steps != 0 || app->handler_timeout_ms != 0) {
    lua_sethook(script->lua, execution_hook, LUA_MASKCOUNT, 1000);
  }
  int result = lua_pcall(script->lua, arguments, results, 0);
  lua_sethook(script->lua, NULL, 0, 0);
  return result;
}

static int lua_log(lua_State *lua) {
  struct i3d_script *script = current_script(lua);
  const char *message = luaL_checkstring(lua, 1);
  i3d_log(script->app, I3D_LOG_INFO, "%s", message);
  return 0;
}

static int lua_print(lua_State *lua) {
  struct i3d_script *script = current_script(lua);
  struct i3d_buffer output = {0};
  int arguments = lua_gettop(lua);
  for (int index = 1; index <= arguments; ++index) {
    // Lua 5.1 has no luaL_tolstring, so call the protected environment's
    // standard tostring function explicitly for print compatibility.
    lua_getglobal(lua, "tostring");
    lua_pushvalue(lua, index);
    if (lua_pcall(lua, 1, 1, 0) != 0) {
      const char *error = lua_tostring(lua, -1);
      free(output.data);
      return luaL_error(lua, "tostring failed: %s",
                        error == NULL ? "unknown error" : error);
    }
    size_t length = 0;
    const char *text = lua_tolstring(lua, -1, &length);
    if (length > 64U * 1024U * 1024U - output.len) {
      free(output.data);
      return luaL_error(lua, "printed output is too large");
    }
    size_t extra = length + (index == 1 ? 0U : 1U);
    if (output.data == NULL || output.len + extra + 1U > output.cap) {
      size_t capacity = output.cap == 0 ? 128U : output.cap;
      while (capacity < output.len + extra + 1U) {
        capacity *= 2U;
      }
      char *grown = realloc(output.data, capacity);
      if (grown == NULL) {
        free(output.data);
        return luaL_error(lua, "out of memory");
      }
      output.data = grown;
      output.cap = capacity;
    }
    if (index != 1) {
      output.data[output.len++] = '\t';
    }
    memcpy(output.data + output.len, text, length);
    output.len += length;
    lua_pop(lua, 1);
  }
  if (output.data == NULL) {
    output.data = strdup("");
    if (output.data == NULL) {
      return luaL_error(lua, "out of memory");
    }
  } else {
    output.data[output.len] = '\0';
  }
  // Keep the journald priority prefix attached to every physical record.
  for (char *cursor = output.data; *cursor != '\0'; ++cursor) {
    if (*cursor == '\r' || *cursor == '\n') {
      *cursor = ' ';
    }
  }
  if (script->app->journald) {
    fputs("<6>", stdout);
  }
  fputs(output.data, stdout);
  fputc('\n', stdout);
  fflush(stdout);
  free(output.data);
  return 0;
}

static int request_raw(lua_State *lua, uint32_t type, const char *payload,
                       size_t payload_len, char **reply, size_t *reply_len) {
  struct i3d_script *script = current_script(lua);
  if (i3d_ipc_request(script->app, type, payload, payload_len, reply,
                      reply_len) < 0) {
    return luaL_error(lua, "i3 IPC request failed: %s", strerror(errno));
  }
  return 0;
}

static bool message_type(const char *name, uint32_t *type) {
  if (strcasecmp(name, "get_tree") == 0 || strcasecmp(name, "tree") == 0) {
    *type = I3D_IPC_GET_TREE;
  } else if (strcasecmp(name, "get_workspaces") == 0 ||
             strcasecmp(name, "workspaces") == 0) {
    *type = I3D_IPC_GET_WORKSPACES;
  } else if (strcasecmp(name, "get_outputs") == 0 ||
             strcasecmp(name, "outputs") == 0) {
    *type = I3D_IPC_GET_OUTPUTS;
  } else if (strcasecmp(name, "get_marks") == 0 ||
             strcasecmp(name, "marks") == 0) {
    *type = I3D_IPC_GET_MARKS;
  } else if (strcasecmp(name, "get_bar_config") == 0 ||
             strcasecmp(name, "bar_config") == 0 ||
             strcasecmp(name, "get_bar_ids") == 0 ||
             strcasecmp(name, "bar_ids") == 0) {
    *type = I3D_IPC_GET_BAR_CONFIG;
  } else if (strcasecmp(name, "get_version") == 0 ||
             strcasecmp(name, "version") == 0) {
    *type = I3D_IPC_GET_VERSION;
  } else {
    return false;
  }
  return true;
}

static int lua_i3_raw(lua_State *lua) {
  const char *name = luaL_checkstring(lua, 1);
  size_t payload_len = 0;
  const char *payload = luaL_optlstring(lua, 2, "", &payload_len);
  uint32_t type = 0;
  if (!message_type(name, &type)) {
    return luaL_error(lua, "unknown i3 message type: %s", name);
  }
  struct i3d_app *app = current_script(lua)->app;
  if (type == I3D_IPC_GET_TREE && app->dispatching) {
    bool owned = false;
    acquire_tree(lua, &owned);
    lua_pushlstring(lua, app->event_tree_raw, app->event_tree_raw_len);
    return 1;
  }
  char *reply = NULL;
  size_t reply_len = 0;
  if (request_raw(lua, type, payload, payload_len, &reply, &reply_len) != 0) {
    return lua_error(lua);
  }
  lua_pushlstring(lua, reply, reply_len);
  free(reply);
  return 1;
}

static int lua_i3_query(lua_State *lua) {
  const char *name = luaL_checkstring(lua, 1);
  size_t payload_len = 0;
  const char *payload = luaL_optlstring(lua, 2, "", &payload_len);
  uint32_t type = 0;
  if (!message_type(name, &type)) {
    return luaL_error(lua, "unknown i3 message type: %s", name);
  }
  if (type == I3D_IPC_GET_TREE) {
    bool owned = false;
    yyjson_doc *tree = acquire_tree(lua, &owned);
    push_json(lua, yyjson_doc_get_root(tree), 0);
    if (owned) {
      yyjson_doc_free(tree);
    }
    return 1;
  }
  char *reply = NULL;
  size_t reply_len = 0;
  if (request_raw(lua, type, payload, payload_len, &reply, &reply_len) != 0) {
    return lua_error(lua);
  }
  yyjson_read_err error;
  yyjson_doc *doc = yyjson_read_opts(reply, reply_len, 0, NULL, &error);
  free(reply);
  if (doc == NULL) {
    return luaL_error(lua, "invalid JSON reply at byte %zu: %s", error.pos,
                      error.msg);
  }
  push_json(lua, yyjson_doc_get_root(doc), 0);
  yyjson_doc_free(doc);
  return 1;
}

static int lua_i3_command(lua_State *lua) {
  size_t command_len = 0;
  const char *command = luaL_checklstring(lua, 1, &command_len);
  char *reply = NULL;
  size_t reply_len = 0;
  if (request_raw(lua, I3D_IPC_RUN_COMMAND, command, command_len, &reply,
                  &reply_len) != 0) {
    return lua_error(lua);
  }
  yyjson_doc *doc = yyjson_read(reply, reply_len, 0);
  free(reply);
  yyjson_val *root = doc == NULL ? NULL : yyjson_doc_get_root(doc);
  bool success = yyjson_is_arr(root) && yyjson_arr_size(root) != 0;
  size_t index, maximum;
  yyjson_val *item;
  yyjson_arr_foreach(root, index, maximum, item) {
    if (!yyjson_is_true(yyjson_obj_get(item, "success"))) {
      success = false;
      break;
    }
  }
  yyjson_doc_free(doc);
  lua_pushboolean(lua, success);
  return 1;
}

static int query_and_push(lua_State *lua, uint32_t type, const char *payload) {
  char *reply = NULL;
  size_t reply_len = 0;
  if (request_raw(lua, type, payload, strlen(payload), &reply, &reply_len) !=
      0) {
    return lua_error(lua);
  }
  yyjson_read_err error;
  yyjson_doc *doc = yyjson_read_opts(reply, reply_len, 0, NULL, &error);
  free(reply);
  if (doc == NULL) {
    return luaL_error(lua, "invalid JSON reply at byte %zu: %s", error.pos,
                      error.msg);
  }
  push_json(lua, yyjson_doc_get_root(doc), 0);
  yyjson_doc_free(doc);
  return 1;
}

static int lua_i3_get_workspaces(lua_State *lua) {
  luaL_checktype(lua, 1, LUA_TNONE);
  return query_and_push(lua, I3D_IPC_GET_WORKSPACES, "");
}

static int lua_i3_get_outputs(lua_State *lua) {
  luaL_checktype(lua, 1, LUA_TNONE);
  return query_and_push(lua, I3D_IPC_GET_OUTPUTS, "");
}

static int lua_i3_get_marks(lua_State *lua) {
  luaL_checktype(lua, 1, LUA_TNONE);
  return query_and_push(lua, I3D_IPC_GET_MARKS, "");
}

static int lua_i3_get_version(lua_State *lua) {
  luaL_checktype(lua, 1, LUA_TNONE);
  return query_and_push(lua, I3D_IPC_GET_VERSION, "");
}

static int lua_i3_get_bar_ids(lua_State *lua) {
  luaL_checktype(lua, 1, LUA_TNONE);
  return query_and_push(lua, I3D_IPC_GET_BAR_CONFIG, "");
}

static int lua_i3_get_bar_config(lua_State *lua) {
  const char *bar_id = luaL_checkstring(lua, 1);
  return query_and_push(lua, I3D_IPC_GET_BAR_CONFIG, bar_id);
}

static yyjson_doc *acquire_tree(lua_State *lua, bool *owned) {
  struct i3d_app *app = current_script(lua)->app;
  if (app->dispatching && app->event_tree != NULL) {
    *owned = false;
    return app->event_tree;
  }
  char *reply = NULL;
  size_t reply_len = 0;
  if (request_raw(lua, I3D_IPC_GET_TREE, "", 0, &reply, &reply_len) != 0) {
    return NULL;
  }
  yyjson_read_err error;
  yyjson_doc *doc = yyjson_read_opts(reply, reply_len, 0, NULL, &error);
  if (doc == NULL) {
    free(reply);
    luaL_error(lua, "invalid GET_TREE JSON at byte %zu: %s", error.pos,
               error.msg);
    return NULL;
  }
  if (app->dispatching) {
    app->event_tree = doc;
    app->event_tree_raw = reply;
    app->event_tree_raw_len = reply_len;
    *owned = false;
  } else {
    free(reply);
    *owned = true;
  }
  return doc;
}

static int lua_i3_get_tree(lua_State *lua) {
  if (lua_gettop(lua) != 0) {
    return luaL_error(lua, "get_tree takes no arguments");
  }
  bool owned = false;
  yyjson_doc *doc = acquire_tree(lua, &owned);
  push_json(lua, yyjson_doc_get_root(doc), 0);
  if (owned) {
    yyjson_doc_free(doc);
  }
  return 1;
}

static yyjson_val *child_array(yyjson_val *node, const char *key) {
  yyjson_val *children = yyjson_obj_get(node, key);
  return yyjson_is_arr(children) ? children : NULL;
}

static yyjson_val *find_node_by_id(yyjson_val *node, int64_t id,
                                   int64_t current_workspace,
                                   int64_t *workspace, unsigned depth) {
  if (!yyjson_is_obj(node) || depth >= 256U) {
    return NULL;
  }
  yyjson_val *type = yyjson_obj_get(node, "type");
  if (yyjson_is_str(type) && strcmp(yyjson_get_str(type), "workspace") == 0) {
    yyjson_val *number = yyjson_obj_get(node, "num");
    if (yyjson_is_int(number)) {
      current_workspace = yyjson_get_sint(number);
    }
  }
  yyjson_val *node_id = yyjson_obj_get(node, "id");
  if (yyjson_is_int(node_id) && yyjson_get_sint(node_id) == id) {
    *workspace = current_workspace;
    return node;
  }
  const char *const arrays[] = {"nodes", "floating_nodes"};
  for (size_t array_index = 0; array_index < 2U; ++array_index) {
    yyjson_val *children = child_array(node, arrays[array_index]);
    size_t index, maximum;
    yyjson_val *child;
    yyjson_arr_foreach(children, index, maximum, child) {
      yyjson_val *found =
          find_node_by_id(child, id, current_workspace, workspace, depth + 1U);
      if (found != NULL) {
        return found;
      }
    }
  }
  return NULL;
}

static int64_t check_integer(lua_State *lua, int index, const char *name) {
  lua_Number number = luaL_checknumber(lua, index);
  if (!isfinite(number) || floor(number) != number ||
      number < (lua_Number)INT64_MIN || number > (lua_Number)INT64_MAX) {
    luaL_error(lua, "%s must be an integer", name);
  }
  return (int64_t)number;
}

static bool exact_integer(lua_State *lua, int index, int64_t *output) {
  if (lua_type(lua, index) != LUA_TNUMBER) {
    return false;
  }
  lua_Number number = lua_tonumber(lua, index);
  if (!isfinite(number) || floor(number) != number ||
      number < (lua_Number)INT64_MIN || number > (lua_Number)INT64_MAX) {
    return false;
  }
  *output = (int64_t)number;
  return true;
}

static int lua_i3_get_window_pid(lua_State *lua) {
  int64_t id = check_integer(lua, 1, "con_id");
  bool owned = false;
  yyjson_doc *doc = acquire_tree(lua, &owned);
  int64_t workspace = 0;
  yyjson_val *node =
      find_node_by_id(yyjson_doc_get_root(doc), id, 0, &workspace, 0);
  yyjson_val *pid = node == NULL ? NULL : yyjson_obj_get(node, "pid");
  if (yyjson_is_int(pid)) {
    lua_pushnumber(lua, (lua_Number)yyjson_get_sint(pid));
  } else {
    lua_pushnil(lua);
  }
  if (owned) {
    yyjson_doc_free(doc);
  }
  return 1;
}

static int lua_i3_get_class_names(lua_State *lua) {
  int64_t id = check_integer(lua, 1, "con_id");
  bool owned = false;
  yyjson_doc *doc = acquire_tree(lua, &owned);
  int64_t workspace = 0;
  yyjson_val *node =
      find_node_by_id(yyjson_doc_get_root(doc), id, 0, &workspace, 0);
  if (node == NULL) {
    lua_pushnil(lua);
  } else {
    lua_createtable(lua, 2, 0);
    int output_index = 1;
    yyjson_val *properties = yyjson_obj_get(node, "window_properties");
    yyjson_val *instance = yyjson_obj_get(properties, "instance");
    yyjson_val *class_name = yyjson_obj_get(properties, "class");
    const char *instance_text =
        yyjson_is_str(instance) ? yyjson_get_str(instance) : NULL;
    if (instance_text != NULL && instance_text[0] != '\0') {
      lua_pushstring(lua, instance_text);
      lua_rawseti(lua, -2, output_index++);
    }
    if (yyjson_is_str(class_name) && yyjson_get_len(class_name) != 0 &&
        (instance_text == NULL ||
         strcmp(instance_text, yyjson_get_str(class_name)) != 0)) {
      lua_pushlstring(lua, yyjson_get_str(class_name),
                      yyjson_get_len(class_name));
      lua_rawseti(lua, -2, output_index);
    }
  }
  if (owned) {
    yyjson_doc_free(doc);
  }
  return 1;
}

static bool json_integer(yyjson_val *value, int64_t *output) {
  if (yyjson_is_sint(value)) {
    *output = yyjson_get_sint(value);
    return true;
  }
  if (yyjson_is_uint(value) && yyjson_get_uint(value) <= INT64_MAX) {
    *output = (int64_t)yyjson_get_uint(value);
    return true;
  }
  return false;
}

static bool matcher_matches(const struct find_matcher *matcher,
                            yyjson_val *node, int64_t workspace) {
  yyjson_val *value = NULL;
  if (strcasecmp(matcher->key, "workspace_num") == 0) {
    return matcher->kind == MATCH_INTEGER &&
           matcher->value.integer == workspace;
  }
  if (strcasecmp(matcher->key, "con_id") == 0) {
    value = yyjson_obj_get(node, "id");
  } else if (strcasecmp(matcher->key, "fullscreen") == 0) {
    int64_t fullscreen = 0;
    json_integer(yyjson_obj_get(node, "fullscreen_mode"), &fullscreen);
    return matcher->kind == MATCH_BOOL &&
           matcher->value.boolean == (fullscreen != 0);
  } else {
    value = yyjson_obj_get(node, matcher->key);
  }
  switch (matcher->kind) {
  case MATCH_BOOL:
    return yyjson_is_bool(value) &&
           yyjson_get_bool(value) == matcher->value.boolean;
  case MATCH_INTEGER: {
    int64_t integer;
    return json_integer(value, &integer) && integer == matcher->value.integer;
  }
  case MATCH_STRING:
    return yyjson_is_str(value) &&
           strcmp(yyjson_get_str(value), matcher->value.string) == 0;
  }
  return false;
}

static bool query_matches(const struct find_query *query, yyjson_val *node,
                          int64_t workspace) {
  for (size_t index = 0; index < query->matcher_count; ++index) {
    if (!matcher_matches(&query->matchers[index], node, workspace)) {
      return false;
    }
  }
  return true;
}

static void project_node(lua_State *lua, const struct find_query *query,
                         yyjson_val *node, int64_t workspace) {
  lua_createtable(lua, 0, (int)query->field_count);
  for (size_t index = 0; index < query->field_count; ++index) {
    const char *field = query->fields[index];
    lua_pushstring(lua, field);
    if (strcasecmp(field, "workspace_num") == 0) {
      lua_pushnumber(lua, (lua_Number)workspace);
    } else {
      const char *json_field = strcasecmp(field, "con_id") == 0 ? "id" : field;
      push_json(lua, yyjson_obj_get(node, json_field), 0);
    }
    lua_rawset(lua, -3);
  }
}

static void find_walk(lua_State *lua, struct find_query *query,
                      yyjson_val *node, int64_t workspace, unsigned depth) {
  if (!yyjson_is_obj(node) || depth >= 256U ||
      (query->limit != 0 && query->found >= query->limit)) {
    return;
  }
  yyjson_val *type = yyjson_obj_get(node, "type");
  if (yyjson_is_str(type) && strcmp(yyjson_get_str(type), "workspace") == 0) {
    int64_t number;
    if (json_integer(yyjson_obj_get(node, "num"), &number)) {
      workspace = number;
    }
  }
  if (query_matches(query, node, workspace)) {
    project_node(lua, query, node, workspace);
    lua_rawseti(lua, -2, (int)++query->found);
    if (query->limit != 0 && query->found >= query->limit) {
      return;
    }
  }
  const char *const arrays[] = {"nodes", "floating_nodes"};
  for (size_t array_index = 0; array_index < 2U; ++array_index) {
    yyjson_val *children = child_array(node, arrays[array_index]);
    size_t index, maximum;
    yyjson_val *child;
    yyjson_arr_foreach(children, index, maximum, child) {
      find_walk(lua, query, child, workspace, depth + 1U);
      if (query->limit != 0 && query->found >= query->limit) {
        return;
      }
    }
  }
}

static void free_find_query(struct find_query *query) {
  for (size_t index = 0; index < query->matcher_count; ++index) {
    free(query->matchers[index].key);
    if (query->matchers[index].kind == MATCH_STRING) {
      free(query->matchers[index].value.string);
    }
  }
  free(query->matchers);
  for (size_t index = 0; index < query->field_count; ++index) {
    free(query->fields[index]);
  }
  free(query->fields);
}

static const char *add_matcher(lua_State *lua, struct find_query *query,
                               const char *key, int value_index) {
  int value_type = lua_type(lua, value_index);
  int64_t integer = 0;
  if (value_type != LUA_TBOOLEAN && value_type != LUA_TSTRING &&
      (value_type != LUA_TNUMBER ||
       !exact_integer(lua, value_index, &integer))) {
    return "matcher values must be booleans, integers, or strings";
  }
  struct find_matcher *grown = realloc(
      query->matchers, (query->matcher_count + 1U) * sizeof(*query->matchers));
  if (grown == NULL) {
    return "out of memory";
  }
  query->matchers = grown;
  struct find_matcher *matcher = &grown[query->matcher_count];
  memset(matcher, 0, sizeof(*matcher));
  matcher->key = strdup(key);
  if (matcher->key == NULL) {
    return "out of memory";
  }
  if (value_type == LUA_TBOOLEAN) {
    matcher->kind = MATCH_BOOL;
    matcher->value.boolean = lua_toboolean(lua, value_index);
  } else if (value_type == LUA_TNUMBER) {
    matcher->kind = MATCH_INTEGER;
    matcher->value.integer = integer;
  } else {
    matcher->kind = MATCH_STRING;
    matcher->value.string = strdup(lua_tostring(lua, value_index));
    if (matcher->value.string == NULL) {
      free(matcher->key);
      matcher->key = NULL;
      return "out of memory";
    }
  }
  ++query->matcher_count;
  return NULL;
}

static const char *parse_matcher_table(lua_State *lua, int table_index,
                                       struct find_query *query) {
  table_index = absolute_index(lua, table_index);
  lua_pushnil(lua);
  while (lua_next(lua, table_index) != 0) {
    if (lua_type(lua, -2) != LUA_TSTRING) {
      lua_pop(lua, 2);
      return "criteria keys must be strings";
    }
    const char *error = add_matcher(lua, query, lua_tostring(lua, -2), -1);
    if (error != NULL) {
      lua_pop(lua, 2);
      return error;
    }
    lua_pop(lua, 1);
  }
  return NULL;
}

static const char *parse_fields(lua_State *lua, int table_index,
                                struct find_query *query) {
  size_t count = lua_objlen(lua, table_index);
  if (count == 0) {
    return NULL;
  }
  for (size_t index = 0; index < count; ++index) {
    lua_rawgeti(lua, table_index, (int)index + 1);
    bool string = lua_type(lua, -1) == LUA_TSTRING;
    lua_pop(lua, 1);
    if (!string) {
      return "fields must contain only strings";
    }
  }
  query->fields = calloc(count, sizeof(*query->fields));
  if (query->fields == NULL) {
    return "out of memory";
  }
  for (size_t index = 0; index < count; ++index) {
    lua_rawgeti(lua, table_index, (int)index + 1);
    query->fields[index] = strdup(lua_tostring(lua, -1));
    lua_pop(lua, 1);
    if (query->fields[index] == NULL) {
      return "out of memory";
    }
    ++query->field_count;
  }
  return NULL;
}

static int lua_i3_find(lua_State *lua) {
  luaL_checktype(lua, 1, LUA_TTABLE);
  struct find_query query = {0};
  lua_getfield(lua, 1, "limit");
  if (!lua_isnil(lua, -1)) {
    int64_t limit = check_integer(lua, -1, "limit");
    if (limit < 0) {
      return luaL_error(lua, "limit must be non-negative");
    }
    query.limit = (size_t)limit;
  }
  lua_pop(lua, 1);

  lua_getfield(lua, 1, "fields");
  if (!lua_isnil(lua, -1)) {
    if (!lua_istable(lua, -1)) {
      return luaL_error(lua, "i3.find fields must be a table");
    }
    const char *error = parse_fields(lua, -1, &query);
    if (error != NULL) {
      lua_pop(lua, 1);
      free_find_query(&query);
      return luaL_error(lua, "i3.find: %s", error);
    }
  }
  lua_pop(lua, 1);
  if (query.field_count == 0) {
    query.fields = malloc(sizeof(*query.fields));
    if (query.fields == NULL || (query.fields[0] = strdup("id")) == NULL) {
      free_find_query(&query);
      return luaL_error(lua, "out of memory");
    }
    query.field_count = 1;
  }

  lua_getfield(lua, 1, "where");
  if (!lua_isnil(lua, -1)) {
    if (!lua_istable(lua, -1)) {
      free_find_query(&query);
      return luaL_error(lua, "i3.find where must be a table");
    }
    const char *error = parse_matcher_table(lua, -1, &query);
    if (error != NULL) {
      lua_pop(lua, 1);
      free_find_query(&query);
      return luaL_error(lua, "i3.find where: %s", error);
    }
  }
  lua_pop(lua, 1);
  lua_pushnil(lua);
  while (lua_next(lua, 1) != 0) {
    const char *key =
        lua_type(lua, -2) == LUA_TSTRING ? lua_tostring(lua, -2) : NULL;
    if (key == NULL) {
      free_find_query(&query);
      return luaL_error(lua, "i3.find criteria keys must be strings");
    }
    if (strcmp(key, "where") != 0 && strcmp(key, "fields") != 0 &&
        strcmp(key, "limit") != 0) {
      const char *error = add_matcher(lua, &query, key, -1);
      if (error != NULL) {
        free_find_query(&query);
        return luaL_error(lua, "i3.find %s: %s", key, error);
      }
    }
    lua_pop(lua, 1);
  }

  bool owned = false;
  yyjson_doc *doc = acquire_tree(lua, &owned);
  lua_newtable(lua);
  find_walk(lua, &query, yyjson_doc_get_root(doc), 0, 0);
  if (owned) {
    yyjson_doc_free(doc);
  }
  free_find_query(&query);
  return 1;
}

static int command_text(lua_State *lua, const char *command, size_t length,
                        bool *success) {
  char *reply = NULL;
  size_t reply_len = 0;
  if (request_raw(lua, I3D_IPC_RUN_COMMAND, command, length, &reply,
                  &reply_len) != 0) {
    return -1;
  }
  yyjson_doc *doc = yyjson_read(reply, reply_len, 0);
  free(reply);
  yyjson_val *root = doc == NULL ? NULL : yyjson_doc_get_root(doc);
  *success = yyjson_is_arr(root) && yyjson_arr_size(root) != 0;
  size_t index, maximum;
  yyjson_val *item;
  yyjson_arr_foreach(root, index, maximum, item) {
    if (!yyjson_is_true(yyjson_obj_get(item, "success"))) {
      *success = false;
      break;
    }
  }
  yyjson_doc_free(doc);
  return 0;
}

static char *quote_i3(const char *text) {
  size_t length = 3U;
  for (const char *cursor = text; *cursor != '\0'; ++cursor) {
    length += (*cursor == '\\' || *cursor == '"') ? 2U : 1U;
  }
  char *quoted = malloc(length);
  if (quoted == NULL) {
    return NULL;
  }
  char *output = quoted;
  *output++ = '"';
  for (const char *cursor = text; *cursor != '\0'; ++cursor) {
    if (*cursor == '\\' || *cursor == '"') {
      *output++ = '\\';
    }
    *output++ = (*cursor == '\r' || *cursor == '\n') ? ' ' : *cursor;
  }
  *output++ = '"';
  *output = '\0';
  return quoted;
}

static int lua_i3_set_urgency(lua_State *lua) {
  int64_t id = check_integer(lua, 1, "con_id");
  if (id <= 0) {
    return luaL_error(lua, "con_id must be positive");
  }
  bool urgent = lua_gettop(lua) < 2 || lua_toboolean(lua, 2);
  char *command = NULL;
  if (asprintf(&command, "[con_id=\"%lld\"] urgent %s", (long long)id,
               urgent ? "enable" : "disable") < 0) {
    return luaL_error(lua, "out of memory");
  }
  bool success = false;
  int result = command_text(lua, command, strlen(command), &success);
  free(command);
  if (result < 0) {
    return lua_error(lua);
  }
  lua_pushboolean(lua, success);
  return 1;
}

static int lua_i3_rename_workspace(lua_State *lua) {
  char old_number[64];
  const char *old_name;
  if (lua_type(lua, 1) == LUA_TNUMBER) {
    snprintf(old_number, sizeof(old_number), "%lld",
             (long long)check_integer(lua, 1, "old"));
    old_name = old_number;
  } else {
    old_name = luaL_checkstring(lua, 1);
  }
  const char *new_name = luaL_checkstring(lua, 2);
  if (new_name[0] == '\0') {
    return luaL_error(lua, "new workspace name must be non-empty");
  }
  char *old_quoted = quote_i3(old_name);
  char *new_quoted = quote_i3(new_name);
  char *command = NULL;
  if (old_quoted == NULL || new_quoted == NULL ||
      asprintf(&command, "rename workspace %s to %s", old_quoted, new_quoted) <
          0) {
    free(old_quoted);
    free(new_quoted);
    return luaL_error(lua, "out of memory");
  }
  free(old_quoted);
  free(new_quoted);
  bool success = false;
  int result = command_text(lua, command, strlen(command), &success);
  free(command);
  if (result < 0) {
    return lua_error(lua);
  }
  lua_pushboolean(lua, success);
  return 1;
}

static int lua_i3_get_workspace_names(lua_State *lua) {
  if (lua_gettop(lua) != 0) {
    return luaL_error(lua, "get_workspace_names takes no arguments");
  }
  char *reply = NULL;
  size_t reply_len = 0;
  if (request_raw(lua, I3D_IPC_GET_WORKSPACES, "", 0, &reply, &reply_len) !=
      0) {
    return lua_error(lua);
  }
  yyjson_doc *doc = yyjson_read(reply, reply_len, 0);
  free(reply);
  yyjson_val *root = doc == NULL ? NULL : yyjson_doc_get_root(doc);
  if (!yyjson_is_arr(root)) {
    yyjson_doc_free(doc);
    return luaL_error(lua, "GET_WORKSPACES returned invalid JSON");
  }
  lua_createtable(lua, (int)yyjson_arr_size(root), 0);
  size_t index, maximum;
  yyjson_val *workspace;
  yyjson_arr_foreach(root, index, maximum, workspace) {
    yyjson_val *name = yyjson_obj_get(workspace, "name");
    push_json(lua, name, 0);
    lua_rawseti(lua, -2, (int)index + 1);
  }
  yyjson_doc_free(doc);
  return 1;
}

static int lua_i3_rename_current_workspace(lua_State *lua) {
  const char *new_name = luaL_checkstring(lua, 1);
  if (new_name[0] == '\0') {
    return luaL_error(lua, "new workspace name must be non-empty");
  }
  char *reply = NULL;
  size_t reply_len = 0;
  if (request_raw(lua, I3D_IPC_GET_WORKSPACES, "", 0, &reply, &reply_len) !=
      0) {
    return lua_error(lua);
  }
  yyjson_doc *doc = yyjson_read(reply, reply_len, 0);
  free(reply);
  yyjson_val *root = doc == NULL ? NULL : yyjson_doc_get_root(doc);
  const char *old_name = NULL;
  size_t index, maximum;
  yyjson_val *workspace;
  yyjson_arr_foreach(root, index, maximum, workspace) {
    if (yyjson_is_true(yyjson_obj_get(workspace, "focused"))) {
      yyjson_val *name = yyjson_obj_get(workspace, "name");
      if (yyjson_is_str(name)) {
        old_name = yyjson_get_str(name);
      }
      break;
    }
  }
  if (old_name == NULL) {
    yyjson_doc_free(doc);
    return luaL_error(lua, "focused workspace not found");
  }
  char *old_copy = strdup(old_name);
  char *new_copy = strdup(new_name);
  yyjson_doc_free(doc);
  if (old_copy == NULL || new_copy == NULL) {
    free(old_copy);
    free(new_copy);
    return luaL_error(lua, "out of memory");
  }
  lua_settop(lua, 0);
  lua_pushstring(lua, old_copy);
  lua_pushstring(lua, new_copy);
  free(old_copy);
  free(new_copy);
  return lua_i3_rename_workspace(lua);
}

static int lua_time_now_sec(lua_State *lua) {
  if (lua_gettop(lua) != 0) {
    return luaL_error(lua, "time.now_sec takes no arguments");
  }
  lua_pushnumber(lua, (lua_Number)time(NULL));
  return 1;
}

static int lua_pid_is_ancestor(lua_State *lua) {
  pid_t ancestor = (pid_t)check_integer(lua, 1, "ancestor");
  pid_t descendant = (pid_t)check_integer(lua, 2, "descendant");
  lua_pushboolean(lua, i3d_pid_is_ancestor(ancestor, descendant));
  return 1;
}

static int lua_pid_watch_stop(lua_State *lua) {
  struct i3d_pid_watch *watch = lua_touserdata(lua, lua_upvalueindex(1));
  struct i3d_script *script = current_script(lua);
  if (lua_gettop(lua) != 0) {
    return luaL_error(lua, "pid watch stop takes no arguments");
  }
  i3d_pid_watch_stop(script->app, watch);
  return 0;
}

static int lua_pid_watch_new(lua_State *lua) {
  luaL_checktype(lua, 1, LUA_TFUNCTION);
  int64_t interval =
      lua_gettop(lua) >= 2 ? check_integer(lua, 2, "poll_interval_ms") : 100;
  if (interval <= 0 || interval > 3600000) {
    return luaL_error(lua, "poll_interval_ms must be between 1 and 3600000");
  }
  bool force_poll = lua_gettop(lua) >= 3 && lua_toboolean(lua, 3);
  lua_pushvalue(lua, 1);
  int callback_ref = luaL_ref(lua, LUA_REGISTRYINDEX);
  struct i3d_script *script = current_script(lua);
  struct i3d_pid_watch *watch = NULL;
  if (i3d_pid_watch_add(script->app, script, callback_ref, (unsigned)interval,
                        force_poll, &watch) < 0) {
    luaL_unref(lua, LUA_REGISTRYINDEX, callback_ref);
    return luaL_error(lua, "start PID watcher: %s", strerror(errno));
  }
  lua_pushlightuserdata(lua, watch);
  lua_pushcclosure(lua, lua_pid_watch_stop, 1);
  return 1;
}

static int append_output(struct i3d_buffer *buffer, const char *data,
                         size_t length) {
  // Cap captured streams independently so a faulty config cannot consume all
  // daemon memory. The child is terminated when this guard is hit.
  if (buffer->len + length > 64U * 1024U * 1024U) {
    errno = EFBIG;
    return -1;
  }
  if (buffer->len + length + 1U > buffer->cap) {
    size_t capacity = buffer->cap == 0 ? 4096U : buffer->cap;
    while (capacity < buffer->len + length + 1U) {
      capacity *= 2U;
    }
    char *grown = realloc(buffer->data, capacity);
    if (grown == NULL) {
      return -1;
    }
    buffer->data = grown;
    buffer->cap = capacity;
  }
  memcpy(buffer->data + buffer->len, data, length);
  buffer->len += length;
  buffer->data[buffer->len] = '\0';
  return 0;
}

static int capture_child(pid_t child, int stdout_fd, int stderr_fd,
                         struct i3d_buffer *stdout_buffer,
                         struct i3d_buffer *stderr_buffer) {
  int result = -1;
  int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
  if (epoll_fd < 0) {
    if (stdout_fd >= 0) {
      close(stdout_fd);
    }
    if (stderr_fd >= 0) {
      close(stderr_fd);
    }
    kill(child, SIGKILL);
    return result;
  }
  int fds[2] = {stdout_fd, stderr_fd};
  struct i3d_buffer *buffers[2] = {stdout_buffer, stderr_buffer};
  int open_count = 0;
  for (int index = 0; index < 2; ++index) {
    if (fds[index] < 0) {
      continue;
    }
    int flags = fcntl(fds[index], F_GETFL);
    fcntl(fds[index], F_SETFL, flags | O_NONBLOCK);
    struct epoll_event event = {
        .events = EPOLLIN | EPOLLHUP | EPOLLERR,
        .data.u32 = (uint32_t)index,
    };
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fds[index], &event) < 0) {
      goto failed;
    }
    ++open_count;
  }
  while (open_count != 0) {
    struct epoll_event events[2];
    int count = epoll_wait(epoll_fd, events, 2, -1);
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count < 0) {
      goto failed;
    }
    for (int event_index = 0; event_index < count; ++event_index) {
      int index = (int)events[event_index].data.u32;
      if (fds[index] < 0) {
        continue;
      }
      bool closed = false;
      for (;;) {
        char chunk[8192];
        ssize_t length = read(fds[index], chunk, sizeof(chunk));
        if (length > 0) {
          if (append_output(buffers[index], chunk, (size_t)length) < 0) {
            goto failed;
          }
          continue;
        }
        if (length == 0) {
          closed = true;
        } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
          closed = true;
        }
        break;
      }
      if (closed) {
        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fds[index], NULL);
        close(fds[index]);
        fds[index] = -1;
        --open_count;
      }
    }
  }
  result = 0;
failed:
  if (result < 0) {
    int saved_errno = errno;
    kill(child, SIGKILL);
    for (int index = 0; index < 2; ++index) {
      if (fds[index] >= 0) {
        close(fds[index]);
      }
    }
    errno = saved_errno;
  }
  close(epoll_fd);
  return result;
}

static void free_arguments(char **arguments, size_t count) {
  if (arguments == NULL) {
    return;
  }
  for (size_t index = 0; index < count; ++index) {
    free(arguments[index]);
  }
  free(arguments);
}

static int lua_exec(lua_State *lua) {
  luaL_checktype(lua, 1, LUA_TTABLE);
  size_t argument_count = lua_objlen(lua, 1);
  if (argument_count == 0) {
    return luaL_error(lua, "exec args must be non-empty");
  }
  char **arguments = calloc(argument_count + 1U, sizeof(*arguments));
  if (arguments == NULL) {
    return luaL_error(lua, "out of memory");
  }
  // Validate before allocating strings so Lua type errors cannot longjmp
  // across partially built native state.
  for (size_t index = 0; index < argument_count; ++index) {
    lua_rawgeti(lua, 1, (int)index + 1);
    bool string = lua_type(lua, -1) == LUA_TSTRING;
    lua_pop(lua, 1);
    if (!string) {
      free_arguments(arguments, argument_count);
      return luaL_error(lua, "exec argument %zu must be a string", index + 1U);
    }
  }
  for (size_t index = 0; index < argument_count; ++index) {
    lua_rawgeti(lua, 1, (int)index + 1);
    arguments[index] = strdup(lua_tostring(lua, -1));
    lua_pop(lua, 1);
    if (arguments[index] == NULL) {
      free_arguments(arguments, argument_count);
      return luaL_error(lua, "out of memory");
    }
  }
  bool check = lua_gettop(lua) < 2 || lua_toboolean(lua, 2);
  bool capture_stdout = lua_gettop(lua) < 3 || lua_toboolean(lua, 3);
  bool capture_stderr = lua_gettop(lua) < 4 || lua_toboolean(lua, 4);
  int stdout_pipe[2] = {-1, -1};
  int stderr_pipe[2] = {-1, -1};
  if ((capture_stdout && pipe2(stdout_pipe, O_CLOEXEC) < 0) ||
      (capture_stderr && pipe2(stderr_pipe, O_CLOEXEC) < 0)) {
    int saved_errno = errno;
    if (stdout_pipe[0] >= 0) {
      close(stdout_pipe[0]);
      close(stdout_pipe[1]);
    }
    free_arguments(arguments, argument_count);
    errno = saved_errno;
    return luaL_error(lua, "create exec pipe: %s", strerror(errno));
  }

  posix_spawn_file_actions_t actions;
  int action_error = posix_spawn_file_actions_init(&actions);
  bool actions_initialized = action_error == 0;
  if (capture_stdout) {
    if (action_error == 0) {
      action_error = posix_spawn_file_actions_adddup2(&actions, stdout_pipe[1],
                                                      STDOUT_FILENO);
    }
    if (action_error == 0) {
      action_error =
          posix_spawn_file_actions_addclose(&actions, stdout_pipe[0]);
    }
  }
  if (capture_stderr) {
    if (action_error == 0) {
      action_error = posix_spawn_file_actions_adddup2(&actions, stderr_pipe[1],
                                                      STDERR_FILENO);
    }
    if (action_error == 0) {
      action_error =
          posix_spawn_file_actions_addclose(&actions, stderr_pipe[0]);
    }
  }
  pid_t child;
  int spawn_error = action_error;
  if (action_error == 0) {
    spawn_error =
        posix_spawnp(&child, arguments[0], &actions, NULL, arguments, environ);
  }
  if (actions_initialized) {
    posix_spawn_file_actions_destroy(&actions);
  }
  if (stdout_pipe[1] >= 0) {
    close(stdout_pipe[1]);
  }
  if (stderr_pipe[1] >= 0) {
    close(stderr_pipe[1]);
  }
  free_arguments(arguments, argument_count);
  if (spawn_error != 0) {
    if (stdout_pipe[0] >= 0) {
      close(stdout_pipe[0]);
    }
    if (stderr_pipe[0] >= 0) {
      close(stderr_pipe[0]);
    }
    return luaL_error(lua, "exec failed: %s", strerror(spawn_error));
  }

  struct i3d_buffer stdout_buffer = {0};
  struct i3d_buffer stderr_buffer = {0};
  if (capture_child(child, stdout_pipe[0], stderr_pipe[0], &stdout_buffer,
                    &stderr_buffer) < 0) {
    int saved_errno = errno;
    waitpid(child, NULL, 0);
    free(stdout_buffer.data);
    free(stderr_buffer.data);
    return luaL_error(lua, "capture exec output: %s", strerror(saved_errno));
  }
  int status = 0;
  pid_t waited;
  do {
    waited = waitpid(child, &status, 0);
  } while (waited < 0 && errno == EINTR);
  if (waited < 0) {
    free(stdout_buffer.data);
    free(stderr_buffer.data);
    return luaL_error(lua, "wait for child: %s", strerror(errno));
  }
  int return_code =
      WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
  if (check && return_code != 0) {
    free(stdout_buffer.data);
    free(stderr_buffer.data);
    return luaL_error(lua, "exec failed with status %d", return_code);
  }
  lua_createtable(lua, 0, 3);
  lua_pushnumber(lua, return_code);
  lua_setfield(lua, -2, "rc");
  if (capture_stdout) {
    lua_pushlstring(lua, stdout_buffer.data == NULL ? "" : stdout_buffer.data,
                    stdout_buffer.len);
  } else {
    lua_pushnil(lua);
  }
  lua_setfield(lua, -2, "stdout");
  if (capture_stderr) {
    lua_pushlstring(lua, stderr_buffer.data == NULL ? "" : stderr_buffer.data,
                    stderr_buffer.len);
  } else {
    lua_pushnil(lua);
  }
  lua_setfield(lua, -2, "stderr");
  free(stdout_buffer.data);
  free(stderr_buffer.data);
  return 1;
}

static void set_function(lua_State *lua, const char *name,
                         lua_CFunction function) {
  lua_pushcfunction(lua, function);
  lua_setfield(lua, -2, name);
}

static void install_environment(struct i3d_script *script) {
  lua_State *lua = script->lua;
  if (script->app->full_lua) {
    luaL_openlibs(lua);
  } else {
    // Open only deterministic, memory-safe libraries. In particular,
    // package, io, os, debug, jit, and ffi are unavailable by default.
    luaopen_base(lua);
    lua_settop(lua, 0);
    luaopen_table(lua);
    lua_settop(lua, 0);
    luaopen_string(lua);
    lua_settop(lua, 0);
    luaopen_math(lua);
    lua_settop(lua, 0);
    const char *const removed[] = {
        "collectgarbage", "dofile", "loadfile", "load",
        "loadstring",     "module", "require",
    };
    for (size_t index = 0; index < sizeof(removed) / sizeof(removed[0]);
         ++index) {
      lua_pushnil(lua);
      lua_setglobal(lua, removed[index]);
    }
  }

  lua_pushlightuserdata(lua, &script_registry_key);
  lua_pushlightuserdata(lua, script);
  lua_rawset(lua, LUA_REGISTRYINDEX);

  lua_pushcfunction(lua, lua_print);
  lua_setglobal(lua, "print");
  lua_pushcfunction(lua, lua_log);
  lua_setglobal(lua, "log");
  lua_pushcfunction(lua, lua_exec);
  lua_setglobal(lua, "exec");
  lua_pushboolean(lua, script->app->debug);
  lua_setglobal(lua, "i3d_debug");
  if (script->app->full_lua) {
    // Preserve the complete debug library in full mode and expose the
    // daemon switch as a field instead of replacing the library table.
    lua_getglobal(lua, "debug");
    lua_pushboolean(lua, script->app->debug);
    lua_setfield(lua, -2, "enabled");
    lua_pop(lua, 1);
  } else {
    lua_pushboolean(lua, script->app->debug);
    lua_setglobal(lua, "debug");
  }
  lua_pushstring(lua, script->path);
  lua_setglobal(lua, "__file__");

  lua_newtable(lua);
  set_function(lua, "command", lua_i3_command);
  set_function(lua, "raw", lua_i3_raw);
  set_function(lua, "query", lua_i3_query);
  set_function(lua, "find", lua_i3_find);
  set_function(lua, "set_urgency", lua_i3_set_urgency);
  set_function(lua, "rename_workspace", lua_i3_rename_workspace);
  set_function(lua, "rename_current_workspace",
               lua_i3_rename_current_workspace);
  set_function(lua, "get_workspace_names", lua_i3_get_workspace_names);
  set_function(lua, "get_tree", lua_i3_get_tree);
  set_function(lua, "get_workspaces", lua_i3_get_workspaces);
  set_function(lua, "get_outputs", lua_i3_get_outputs);
  set_function(lua, "get_marks", lua_i3_get_marks);
  set_function(lua, "get_version", lua_i3_get_version);
  set_function(lua, "get_bar_ids", lua_i3_get_bar_ids);
  set_function(lua, "get_bar_config", lua_i3_get_bar_config);
  set_function(lua, "get_window_pid", lua_i3_get_window_pid);
  set_function(lua, "get_class_names", lua_i3_get_class_names);
  lua_setglobal(lua, "i3");

  lua_newtable(lua);
  set_function(lua, "is_ancestor", lua_pid_is_ancestor);
  set_function(lua, "watch_new", lua_pid_watch_new);
  lua_setglobal(lua, "pid");

  lua_newtable(lua);
  set_function(lua, "now_sec", lua_time_now_sec);
  lua_setglobal(lua, "time");
}

static enum i3d_event_type event_by_name(const char *name, bool *valid) {
  for (enum i3d_event_type type = 0; type < I3D_EVENT_COUNT; ++type) {
    if (strcmp(name, i3d_event_name(type)) == 0 ||
        (type == I3D_EVENT_BARCONFIG_UPDATE &&
         strcmp(name, "bar_config_update") == 0)) {
      *valid = true;
      return type;
    }
  }
  *valid = false;
  return I3D_EVENT_WORKSPACE;
}

static int reference_handler(struct i3d_script *script,
                             enum i3d_event_type type, int value_index,
                             const char *name) {
  lua_State *lua = script->lua;
  if (!lua_isfunction(lua, value_index)) {
    i3d_log(script->app, I3D_LOG_ERROR, "script %s: %s must be a function",
            script->base, name);
    return -1;
  }
  if (script->handlers[type] != LUA_NOREF) {
    luaL_unref(lua, LUA_REGISTRYINDEX, script->handlers[type]);
  }
  lua_pushvalue(lua, value_index);
  script->handlers[type] = luaL_ref(lua, LUA_REGISTRYINDEX);
  return 0;
}

static int discover_handlers(struct i3d_script *script) {
  lua_State *lua = script->lua;
  for (enum i3d_event_type type = 0; type < I3D_EVENT_COUNT; ++type) {
    char name[64];
    snprintf(name, sizeof(name), "on_%s", i3d_event_name(type));
    lua_getglobal(lua, name);
    if (!lua_isnil(lua, -1) && reference_handler(script, type, -1, name) < 0) {
      lua_pop(lua, 1);
      return -1;
    }
    lua_pop(lua, 1);
  }

  lua_getglobal(lua, "handlers");
  if (lua_isnil(lua, -1)) {
    lua_pop(lua, 1);
    return 0;
  }
  if (!lua_istable(lua, -1)) {
    i3d_log(script->app, I3D_LOG_ERROR, "script %s: handlers must be a table",
            script->base);
    lua_pop(lua, 1);
    return -1;
  }
  lua_pushnil(lua);
  while (lua_next(lua, -2) != 0) {
    if (!lua_isstring(lua, -2)) {
      i3d_log(script->app, I3D_LOG_ERROR,
              "script %s: handlers keys must be strings", script->base);
      lua_pop(lua, 3);
      return -1;
    }
    bool valid;
    const char *name = lua_tostring(lua, -2);
    enum i3d_event_type type = event_by_name(name, &valid);
    if (!valid) {
      i3d_log(script->app, I3D_LOG_ERROR, "script %s: unknown handler event %s",
              script->base, name);
      lua_pop(lua, 3);
      return -1;
    }
    if (reference_handler(script, type, -1, name) < 0) {
      lua_pop(lua, 3);
      return -1;
    }
    lua_pop(lua, 1);
  }
  lua_pop(lua, 1);
  return 0;
}

static struct i3d_script *load_script(struct i3d_app *app, const char *path) {
  struct i3d_script *script = calloc(1, sizeof(*script));
  if (script == NULL) {
    return NULL;
  }
  script->app = app;
  script->path = strdup(path);
  const char *slash = strrchr(path, '/');
  script->base = strdup(slash == NULL ? path : slash + 1);
  script->lua = luaL_newstate();
  for (size_t index = 0; index < I3D_EVENT_COUNT; ++index) {
    script->handlers[index] = LUA_NOREF;
  }
  if (script->path == NULL || script->base == NULL || script->lua == NULL) {
    goto failed;
  }
  install_environment(script);
  if (luaL_loadfile(script->lua, path) != 0 ||
      guarded_pcall(script, 0, 0) != 0) {
    i3d_log(app, I3D_LOG_ERROR, "script %s: %s", script->base,
            lua_tostring(script->lua, -1));
    goto failed;
  }
  if (discover_handlers(script) < 0) {
    goto failed;
  }
  lua_getglobal(script->lua, "init");
  if (!lua_isnil(script->lua, -1)) {
    if (!lua_isfunction(script->lua, -1)) {
      i3d_log(app, I3D_LOG_ERROR, "script %s: init must be a function",
              script->base);
      lua_pop(script->lua, 1);
      goto failed;
    }
    if (guarded_pcall(script, 0, 0) != 0) {
      i3d_log(app, I3D_LOG_ERROR, "script %s init: %s", script->base,
              lua_tostring(script->lua, -1));
      goto failed;
    }
  } else {
    lua_pop(script->lua, 1);
  }
  return script;

failed:
  if (script->lua != NULL) {
    i3d_pid_watch_remove_script(app, script);
    lua_close(script->lua);
  }
  free(script->path);
  free(script->base);
  free(script);
  return NULL;
}

static int compare_handler(const void *left, const void *right) {
  const struct i3d_script *a = *(const struct i3d_script *const *)left;
  const struct i3d_script *b = *(const struct i3d_script *const *)right;
  return strcmp(a->base, b->base);
}

struct i3d_registry *i3d_registry_load(struct i3d_app *app) {
  struct i3d_registry *registry = calloc(1, sizeof(*registry));
  if (registry == NULL) {
    return NULL;
  }
  char *pattern = NULL;
  if (asprintf(&pattern, "%s/*.lua", app->config_dir) < 0) {
    free(registry);
    return NULL;
  }
  glob_t files = {0};
  int glob_result = glob(pattern, 0, NULL, &files);
  free(pattern);
  if (glob_result != 0 && glob_result != GLOB_NOMATCH) {
    i3d_log(app, I3D_LOG_ERROR, "scan config directory: glob error %d",
            glob_result);
    free(registry);
    return NULL;
  }
  struct i3d_script **tail = &registry->scripts;
  for (size_t index = 0; index < files.gl_pathc; ++index) {
    struct i3d_script *script = load_script(app, files.gl_pathv[index]);
    if (script == NULL) {
      continue;
    }
    *tail = script;
    tail = &script->next;
    ++registry->script_count;
    for (enum i3d_event_type type = 0; type < I3D_EVENT_COUNT; ++type) {
      if (script->handlers[type] != LUA_NOREF) {
        ++registry->handler_count[type];
        ++registry->total_handler_count;
      }
    }
    i3d_log(app, I3D_LOG_INFO, "script loaded: %s", script->base);
  }
  globfree(&files);

  for (enum i3d_event_type type = 0; type < I3D_EVENT_COUNT; ++type) {
    size_t count = registry->handler_count[type];
    if (count == 0) {
      continue;
    }
    registry->handlers[type] = calloc(count, sizeof(struct i3d_script *));
    if (registry->handlers[type] == NULL) {
      i3d_registry_free(app, registry);
      return NULL;
    }
    size_t index = 0;
    for (struct i3d_script *script = registry->scripts; script != NULL;
         script = script->next) {
      if (script->handlers[type] != LUA_NOREF) {
        registry->handlers[type][index++] = script;
      }
    }
    qsort(registry->handlers[type], count, sizeof(struct i3d_script *),
          compare_handler);
  }
  return registry;
}

void i3d_registry_free(struct i3d_app *app, struct i3d_registry *registry) {
  if (registry == NULL) {
    return;
  }
  struct i3d_script *script = registry->scripts;
  while (script != NULL) {
    struct i3d_script *next = script->next;
    i3d_pid_watch_remove_script(app, script);
    lua_close(script->lua);
    free(script->path);
    free(script->base);
    free(script);
    script = next;
  }
  for (size_t type = 0; type < I3D_EVENT_COUNT; ++type) {
    free(registry->handlers[type]);
  }
  free(registry);
}

static int64_t event_workspace(struct i3d_app *app, int64_t container_id,
                               bool *found) {
  if (app->event_tree == NULL) {
    char *reply = NULL;
    size_t reply_len = 0;
    if (i3d_ipc_request(app, I3D_IPC_GET_TREE, "", 0, &reply, &reply_len) < 0) {
      *found = false;
      return 0;
    }
    app->event_tree = yyjson_read(reply, reply_len, 0);
    app->event_tree_raw = reply;
    app->event_tree_raw_len = reply_len;
  }
  if (app->event_tree == NULL) {
    free(app->event_tree_raw);
    app->event_tree_raw = NULL;
    app->event_tree_raw_len = 0;
    *found = false;
    return 0;
  }
  int64_t workspace = 0;
  yyjson_val *node = find_node_by_id(yyjson_doc_get_root(app->event_tree),
                                     container_id, 0, &workspace, 0);
  *found = node != NULL;
  return workspace;
}

static void push_event(lua_State *lua, enum i3d_event_type type,
                       const char *change, bool has_container,
                       int64_t container_id, int64_t fullscreen_mode,
                       bool has_workspace, int64_t workspace) {
  lua_createtable(lua, 0, type == I3D_EVENT_WINDOW ? 5 : 2);
  lua_pushstring(lua, i3d_event_name(type));
  lua_setfield(lua, -2, "type");
  lua_pushstring(lua, change);
  lua_setfield(lua, -2, "change");
  if (type == I3D_EVENT_WINDOW) {
    if (has_container) {
      lua_pushnumber(lua, (lua_Number)container_id);
    } else {
      lua_pushnil(lua);
    }
    lua_setfield(lua, -2, "con_id");
    if (has_workspace) {
      lua_pushnumber(lua, (lua_Number)workspace);
    } else {
      lua_pushnil(lua);
    }
    lua_setfield(lua, -2, "workspace_num");
    lua_pushnumber(lua, (lua_Number)fullscreen_mode);
    lua_setfield(lua, -2, "fullscreen_mode");
  }
}

void i3d_dispatch_json_event(struct i3d_app *app, uint32_t ipc_type,
                             const char *json, size_t json_len) {
  enum i3d_event_type type;
  if (i3d_event_from_ipc(ipc_type, &type) < 0) {
    i3d_log(app, I3D_LOG_DEBUG, "ignored IPC message type=%u", ipc_type);
    return;
  }
  yyjson_read_err error = {0};
  yyjson_doc *event_doc =
      yyjson_read_opts((char *)json, json_len, 0, NULL, &error);
  yyjson_val *root = event_doc == NULL ? NULL : yyjson_doc_get_root(event_doc);
  if (!yyjson_is_obj(root)) {
    i3d_log(app, I3D_LOG_ERROR, "invalid %s event JSON at byte %zu: %s",
            i3d_event_name(type), error.pos,
            error.msg == NULL ? "root is not an object" : error.msg);
    yyjson_doc_free(event_doc);
    return;
  }
  yyjson_val *change_value = yyjson_obj_get(root, "change");
  const char *change =
      yyjson_is_str(change_value) ? yyjson_get_str(change_value) : "";
  size_t handler_count = app->registry->handler_count[type];
  if (handler_count == 0) {
    i3d_log(app, I3D_LOG_DEBUG, "event=%s change=%s (no handlers)",
            i3d_event_name(type), change);
    yyjson_doc_free(event_doc);
    return;
  }
  bool has_container = false;
  int64_t container_id = 0;
  int64_t fullscreen_mode = 0;
  if (type == I3D_EVENT_WINDOW) {
    yyjson_val *container = yyjson_obj_get(root, "container");
    has_container =
        json_integer(yyjson_obj_get(container, "id"), &container_id) &&
        container_id > 0;
    json_integer(yyjson_obj_get(container, "fullscreen_mode"),
                 &fullscreen_mode);
  }

  i3d_event_cache_clear(app);
  app->dispatching = true;
  bool has_workspace = false;
  int64_t workspace =
      has_container ? event_workspace(app, container_id, &has_workspace) : 0;
  i3d_log(app, I3D_LOG_DEBUG, "event=%s change=%s handlers=%zu",
          i3d_event_name(type), change, handler_count);
  for (size_t index = 0; index < handler_count; ++index) {
    struct i3d_script *script = app->registry->handlers[type][index];
    lua_rawgeti(script->lua, LUA_REGISTRYINDEX, script->handlers[type]);
    push_event(script->lua, type, change, has_container, container_id,
               fullscreen_mode, has_workspace, workspace);
    if (guarded_pcall(script, 1, 0) != 0) {
      i3d_log(app, I3D_LOG_ERROR, "handler %s (%s): %s", script->base,
              i3d_event_name(type), lua_tostring(script->lua, -1));
      lua_pop(script->lua, 1);
    }
  }
  app->dispatching = false;
  i3d_event_cache_clear(app);
  yyjson_doc_free(event_doc);
}

void i3d_lua_call_pid(struct i3d_app *app, struct i3d_pid_watch *watch,
                      pid_t child_pid, pid_t parent_pid) {
  if (!watch->active) {
    return;
  }
  struct i3d_script *script = watch->script;
  i3d_event_cache_clear(app);
  app->dispatching = true;
  lua_rawgeti(script->lua, LUA_REGISTRYINDEX, watch->callback_ref);
  lua_pushnumber(script->lua, (lua_Number)child_pid);
  lua_pushnumber(script->lua, (lua_Number)parent_pid);
  if (guarded_pcall(script, 2, 0) != 0) {
    i3d_log(app, I3D_LOG_ERROR, "pid.watch_new callback %s: %s", script->base,
            lua_tostring(script->lua, -1));
    lua_pop(script->lua, 1);
  }
  app->dispatching = false;
  i3d_event_cache_clear(app);
}
