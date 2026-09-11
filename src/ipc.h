#ifndef I3D_IPC_H
#define I3D_IPC_H

#include <stdint.h>

// Numeric request values are part of i3's stable wire protocol.
enum i3d_ipc_message_type {
  I3D_IPC_RUN_COMMAND = 0,
  I3D_IPC_GET_WORKSPACES = 1,
  I3D_IPC_SUBSCRIBE = 2,
  I3D_IPC_GET_OUTPUTS = 3,
  I3D_IPC_GET_TREE = 4,
  I3D_IPC_GET_MARKS = 5,
  I3D_IPC_GET_BAR_CONFIG = 6,
  I3D_IPC_GET_VERSION = 7,
};

#define I3D_IPC_EVENT_BIT UINT32_C(0x80000000)
#define I3D_IPC_MAX_PAYLOAD (128U * 1024U * 1024U)

#endif
