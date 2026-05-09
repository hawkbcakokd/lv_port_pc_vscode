#ifndef SHELL_APP_RUNTIME_PROTOCOL_H
#define SHELL_APP_RUNTIME_PROTOCOL_H

/*
 * IPC between shell (compositor) and foreground app. The shared middle viewport
 * uses screen coordinates: y in [STATUS_H, SCREEN_H) with height reduced by
 * NAV_H when the shell shows the navigation bar.
 */

#include <stdint.h>
#include <lv_conf.h>

#define RUNTIME_SOCK_PATH "/tmp/shell_app_runtime.sock"

#define SCREEN_W 320
#define SCREEN_H 480
#define STATUS_H 28
#define NAV_H 44
#define BPP (LV_COLOR_DEPTH/8) /* ARGB8888 */

typedef enum {
    MSG_APP_REGISTER = 1,
    MSG_SHELL_SURFACE,
    MSG_SHELL_VIEWPORT_UPDATE,
    MSG_APP_PRESENT,
    MSG_APP_NAV_REQ
} msg_type_t;

#pragma pack(push, 1)
typedef struct {
    uint32_t type;
    uint32_t seq;

    uint16_t dirty_x;
    uint16_t dirty_y;
    uint16_t dirty_w;
    uint16_t dirty_h;

    uint16_t vp_x;
    uint16_t vp_y;
    uint16_t vp_w;
    uint16_t vp_h;
    uint32_t stride_bytes;

    uint8_t nav_visible; /* 0 hide, 1 show */
    uint8_t reserved[3];
} runtime_msg_t;
#pragma pack(pop)

#endif
