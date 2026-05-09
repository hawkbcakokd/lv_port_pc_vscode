/*
 * Foreground/background sub-application process.
 */
#define _GNU_SOURCE
#include "protocol.h"
#include "ipc.h"
#include "lvgl/lvgl.h"
#include "lvgl/src/draw/lv_draw_buf.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define APP_VP_MAX_H ((uint32_t)(SCREEN_H - STATUS_H))
#ifndef APP_VARIANT
#define APP_VARIANT 0
#endif

typedef struct {
    int sock;
    int memfd;
    uint8_t *surface;
    uint32_t stride;
    uint16_t vp_w;
    uint16_t vp_h;
    bool nav_visible;
    bool is_foreground;
    uint32_t seq;
    uint32_t tick_ms;
    uint32_t beat;
    lv_display_t *disp;
    void *draw_buf_owner;
    lv_indev_t *ptr_indev;
    lv_obj_t *root;
    lv_obj_t *title_label;
    lv_obj_t *info_label;
    lv_obj_t *btn_a;
    lv_indev_state_t ptr_state;
    lv_point_t ptr_point;
} app_ctx_t;

static const char *app_name(void)
{
    if (APP_VARIANT == 1) return "App B";
    if (APP_VARIANT == 2) return "App C";
    return "App A";
}

static uint16_t app_id(void) { return (uint16_t)(APP_VARIANT + 1); }

static lv_color_t app_bg(bool fg)
{
    if (APP_VARIANT == 1) return fg ? lv_color_hex(0x263238) : lv_color_hex(0x37474F);
    if (APP_VARIANT == 2) return fg ? lv_color_hex(0x3E2723) : lv_color_hex(0x4E342E);
    return fg ? lv_color_hex(0x1B5E20) : lv_color_hex(0x2E7D32);
}

static int app_connect(void)
{
    int fd;
    struct sockaddr_un addr;
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, RUNTIME_SOCK_PATH, sizeof(addr.sun_path) - 1);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) return -1;
    return fd;
}

static int app_send_nav_req(app_ctx_t *ctx, bool visible)
{
    runtime_msg_t m;
    memset(&m, 0, sizeof(m));
    m.type = MSG_APP_NAV_REQ;
    m.seq = ++ctx->seq;
    m.app_id = app_id();
    m.nav_visible = visible ? 1 : 0;
    return ipc_send_msg(ctx->sock, &m);
}

static int app_send_present(app_ctx_t *ctx, uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    runtime_msg_t m;
    memset(&m, 0, sizeof(m));
    m.type = MSG_APP_PRESENT;
    m.seq = ++ctx->seq;
    m.app_id = app_id();
    m.dirty_x = x;
    m.dirty_y = y;
    m.dirty_w = w;
    m.dirty_h = h;
    return ipc_send_msg(ctx->sock, &m);
}

static void app_update_info(app_ctx_t *ctx)
{
    char title[64];
    char info[200];
    snprintf(title, sizeof(title), "%s (%s)", app_name(), ctx->is_foreground ? "Foreground" : "Background");
    lv_label_set_text(ctx->title_label, title);
    snprintf(info, sizeof(info), "beat=%lu\nviewport=%ux%u\nnav=%s",
             (unsigned long)ctx->beat, ctx->vp_w, ctx->vp_h, ctx->nav_visible ? "visible" : "hidden");
    lv_label_set_text(ctx->info_label, info);
}

static void app_btn_a_clicked(lv_event_t *e)
{
    app_ctx_t *ctx = (app_ctx_t *)lv_event_get_user_data(e);
    if (!ctx) return;
    app_send_nav_req(ctx, true);
    lv_obj_add_flag(ctx->btn_a, LV_OBJ_FLAG_HIDDEN);
}

static void app_build_ui(app_ctx_t *ctx)
{
    ctx->root = lv_obj_create(lv_scr_act());
    lv_obj_set_size(ctx->root, ctx->vp_w, ctx->vp_h);
    lv_obj_set_style_bg_color(ctx->root, app_bg(true), LV_PART_MAIN);
    lv_obj_set_style_border_width(ctx->root, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(ctx->root, 8, LV_PART_MAIN);
    ctx->title_label = lv_label_create(ctx->root);
    lv_obj_align(ctx->title_label, LV_ALIGN_TOP_MID, 0, 2);
    lv_obj_set_style_text_color(ctx->title_label, lv_color_hex(0xECEFF1), LV_PART_MAIN);
    ctx->info_label = lv_label_create(ctx->root);
    lv_label_set_long_mode(ctx->info_label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(ctx->info_label, ctx->vp_w - 24);
    lv_obj_align(ctx->info_label, LV_ALIGN_CENTER, 0, 4);
    ctx->btn_a = lv_button_create(ctx->root);
    lv_obj_set_size(ctx->btn_a, 126, 34);
    lv_obj_align(ctx->btn_a, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_add_event_cb(ctx->btn_a, app_btn_a_clicked, LV_EVENT_CLICKED, ctx);
    lv_obj_t *lb = lv_label_create(ctx->btn_a);
    lv_label_set_text(lb, "A: show nav");
    lv_obj_center(lb);
    lv_obj_update_layout(ctx->root);
}

static void app_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    app_ctx_t *ctx = (app_ctx_t *)lv_display_get_user_data(disp);
    int32_t y;
    int32_t w = lv_area_get_width(area);
    uint32_t line_bytes = (uint32_t)w * BPP;
    for (y = area->y1; y <= area->y2; y++) {
        uint8_t *dst = ctx->surface + (uint32_t)y * ctx->stride + (uint32_t)area->x1 * BPP;
        uint8_t *src = px_map + (uint32_t)(y - area->y1) * line_bytes;
        memcpy(dst, src, line_bytes);
    }
    app_send_present(ctx, (uint16_t)area->x1, (uint16_t)area->y1, (uint16_t)w, (uint16_t)lv_area_get_height(area));
    lv_display_flush_ready(disp);
}

static void app_pointer_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    app_ctx_t *ctx = (app_ctx_t *)lv_indev_get_user_data(indev);
    data->point = ctx->ptr_point;
    data->state = ctx->ptr_state;
}

static void app_apply_viewport(app_ctx_t *ctx, const runtime_msg_t *m)
{
    ctx->vp_w = m->vp_w;
    ctx->vp_h = m->vp_h;
    ctx->nav_visible = m->nav_visible != 0;
    lv_display_set_resolution(ctx->disp, ctx->vp_w, ctx->vp_h);
    lv_obj_set_size(ctx->root, ctx->vp_w, ctx->vp_h);
    lv_obj_set_width(ctx->info_label, ctx->vp_w - 24);
    if (!ctx->nav_visible) {
        lv_obj_clear_flag(ctx->btn_a, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(ctx->btn_a);
    } else
        lv_obj_add_flag(ctx->btn_a, LV_OBJ_FLAG_HIDDEN);
    app_update_info(ctx);
    lv_obj_update_layout(ctx->root);
    lv_obj_invalidate(ctx->root);
}

int app_main(void)
{
    app_ctx_t ctx;
    runtime_msg_t in, out;
    int rc;
    int recv_fd = -1;
    memset(&ctx, 0, sizeof(ctx));
    ctx.ptr_state = LV_INDEV_STATE_RELEASED;
    ctx.sock = app_connect();
    if (ctx.sock < 0) return 1;

    memset(&out, 0, sizeof(out));
    out.type = MSG_APP_REGISTER;
    out.app_id = app_id();
    if (ipc_send_msg(ctx.sock, &out) != 0) return 1;

    rc = ipc_recv_msg_with_fd(ctx.sock, &in, &recv_fd);
    if (rc != 0 || in.type != MSG_SHELL_SURFACE || recv_fd < 0) return 1;
    ctx.memfd = recv_fd;
    ctx.vp_w = in.vp_w;
    ctx.vp_h = in.vp_h;
    ctx.stride = in.stride_bytes;
    ctx.nav_visible = in.nav_visible != 0;

    {
        size_t map_size = (size_t)ctx.stride * (SCREEN_H - STATUS_H);
        ctx.surface = (uint8_t *)mmap(NULL, map_size, PROT_READ | PROT_WRITE, MAP_SHARED, ctx.memfd, 0);
        if (ctx.surface == MAP_FAILED) return 1;
    }

    ctx.disp = lv_display_create(ctx.vp_w, ctx.vp_h);
    lv_display_set_user_data(ctx.disp, &ctx);
    lv_display_set_flush_cb(ctx.disp, app_flush_cb);
    {
        lv_color_format_t cf = lv_display_get_color_format(ctx.disp);
        uint32_t stride = lv_draw_buf_width_to_stride(ctx.vp_w, cf);
        uint32_t buf_sz = stride * APP_VP_MAX_H;
        ctx.draw_buf_owner = lv_malloc(buf_sz + LV_DRAW_BUF_ALIGN * 2);
        if (ctx.draw_buf_owner == NULL) return 1;
        lv_display_set_buffers(ctx.disp, lv_draw_buf_align(ctx.draw_buf_owner, cf), NULL, buf_sz,
                               LV_DISPLAY_RENDER_MODE_FULL);
    }
    lv_display_set_default(ctx.disp);

    ctx.ptr_indev = lv_indev_create();
    lv_indev_set_type(ctx.ptr_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(ctx.ptr_indev, app_pointer_read);
    lv_indev_set_user_data(ctx.ptr_indev, &ctx);
    lv_indev_set_display(ctx.ptr_indev, ctx.disp);

    app_build_ui(&ctx);
    app_update_info(&ctx);
    if (!ctx.nav_visible) lv_obj_clear_flag(ctx.btn_a, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(ctx.btn_a, LV_OBJ_FLAG_HIDDEN);

    while (1) {
        struct timespec ts = {0, 33 * 1000 * 1000};
        nanosleep(&ts, NULL);
        ctx.tick_ms += 33;
        if (ctx.tick_ms % 1000 < 33) {
            ctx.beat++;
            lv_obj_set_style_bg_color(ctx.root, app_bg(ctx.is_foreground), LV_PART_MAIN);
            app_update_info(&ctx);
        }

        {
            struct pollfd pfd;
            pfd.fd = ctx.sock;
            pfd.events = POLLIN;
            if (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN)) {
                runtime_msg_t m;
                rc = ipc_recv_msg(ctx.sock, &m);
                if (rc != 0) return rc == 1 ? 0 : 1;
                if (m.type == MSG_SHELL_VIEWPORT_UPDATE) app_apply_viewport(&ctx, &m);
                else if (m.type == MSG_SHELL_SET_FOREGROUND) ctx.is_foreground = m.input_state != 0;
                else if (m.type == MSG_SHELL_INPUT_POINTER) {
                    ctx.ptr_point.x = m.input_x;
                    ctx.ptr_point.y = m.input_y;
                    ctx.ptr_state = m.input_state ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
                } else if (m.type == MSG_SHELL_GO_HOME) {
                    ctx.is_foreground = false;
                    app_update_info(&ctx);
                }
            }
        }

        lv_tick_inc(33);
        (void)lv_timer_handler();
    }
}
