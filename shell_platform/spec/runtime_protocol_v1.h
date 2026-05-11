/*
 * Reference specification: product-facing runtime protocol (v1 direction).
 *
 * This file is NOT wired into CMake by default. It documents evolution from
 * shell_app_runtime/protocol.h: add magic + version for ABI checks; keep
 * message layout packed and append-only for minor versions.
 *
 * Integration: copy fields into your shipping protocol.h or include this
 * after renaming conflicts.
 */
#ifndef SHELL_PLATFORM_RUNTIME_PROTOCOL_V1_H
#define SHELL_PLATFORM_RUNTIME_PROTOCOL_V1_H

#include <stdint.h>

/* Transport: AF_UNIX SOCK_STREAM, path configurable per product */
#ifndef RUNTIME_SOCK_PATH
#define RUNTIME_SOCK_PATH "/tmp/shell_app_runtime.sock"
#endif

#define RUNTIME_PROTO_MAGIC 0x52544D31u /* 'RTM1' little-endian on wire as 31 4D 54 52 */

#define RUNTIME_PROTO_VER_MAJOR 1u
#define RUNTIME_PROTO_VER_MINOR 0u

/*
 * Optional first message after connect (before MSG_APP_REGISTER):
 *   magic, major, minor, role (APP | LAUNCHER | DEBUG)
 * Shell rejects unknown major; may accept higher minor with feature bits.
 */
typedef enum {
    RUNTIME_ROLE_APP = 0,
    RUNTIME_ROLE_LAUNCHER = 1,
    RUNTIME_ROLE_RESERVED = 2
} runtime_role_t;

typedef enum {
    MSG_HELLO = 0, /* optional capability exchange */
    MSG_APP_REGISTER = 1,
    MSG_SHELL_SURFACE,
    MSG_SHELL_VIEWPORT_UPDATE,
    MSG_APP_PRESENT,
    MSG_APP_NAV_REQ,
    MSG_SHELL_INPUT_POINTER,
    MSG_SHELL_INPUT_KEY,
    MSG_SHELL_SET_FOREGROUND,
    MSG_SHELL_GO_HOME
} runtime_msg_type_v1_t;

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint16_t ver_major;
    uint16_t ver_minor;
    uint32_t role; /* runtime_role_t */
    uint16_t app_id; /* LAUNCHER may use 0 */
    uint16_t reserved0;
} runtime_hello_t;

/* Same logical layout as runtime_msg_t today; v1 adds hello before register. */
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
    uint16_t app_id;
    uint16_t input_x;
    uint16_t input_y;
    uint16_t reserved1;
    uint32_t key_code;
    uint8_t nav_visible;
    uint8_t input_state;
    uint8_t reserved2[2];
} runtime_msg_v1_t;
#pragma pack(pop)

#endif
