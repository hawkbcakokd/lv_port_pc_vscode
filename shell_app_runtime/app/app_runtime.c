/*
 * Foreground sub-application process.
 *
 * LVGL renders into a local draw buffer; flush_cb copies each flushed rectangle
 * into the shell-provided shared surface (mmap of memfd) and sends MSG_APP_PRESENT
 * so the shell can composite the middle region onto the only physical display.
 */
#define _GNU_SOURCE
#include "protocol.h"
#include "ipc.h"
#include "lvgl/lvgl.h"
#include "lvgl/src/display/lv_display_private.h"
#include "lvgl/src/draw/lv_draw_buf.h"

/* Max viewport height when shell hides bottom navigation */
#define APP_VP_MAX_H ((uint32_t)(SCREEN_H - STATUS_H))

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

typedef struct {
    int sock;
    int memfd;
    uint8_t *surface;
    uint32_t stride;
    uint16_t vp_w;
    uint16_t vp_h;
    bool nav_visible;
    uint32_t seq;
    uint32_t tick_ms;
    bool back_enabled;

    lv_display_t *disp;
    void *draw_buf_owner;
    lv_obj_t *root;
    lv_obj_t *info_label;
} app_ctx_t;

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

static void app_build_ui(app_ctx_t *ctx)
{
    ctx->root = lv_obj_create(lv_scr_act());
    lv_obj_set_size(ctx->root, ctx->vp_w, ctx->vp_h);
    lv_obj_set_style_bg_color(ctx->root, lv_color_hex(0x37474F), LV_PART_MAIN);
    lv_obj_set_style_border_width(ctx->root, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(ctx->root, 8, LV_PART_MAIN);

    {
        lv_obj_t *t = lv_label_create(ctx->root);
        lv_label_set_text(t, "App - middle layer (only this process paints here)");
        lv_obj_set_style_text_color(t, lv_color_hex(0xECEFF1), LV_PART_MAIN);
        lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 4);
    }

    ctx->info_label = lv_label_create(ctx->root);
    lv_label_set_long_mode(ctx->info_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(ctx->info_label, ctx->vp_w - 24);
    lv_obj_set_style_text_color(ctx->info_label, lv_color_hex(0xB0BEC5), LV_PART_MAIN);
    lv_obj_align(ctx->info_label, LV_ALIGN_CENTER, 0, 8);

    lv_obj_update_layout(ctx->root);
}

static void app_update_info(app_ctx_t *ctx)
{
    char buf[192];
    const char *mode;
    if (ctx->nav_visible)
        mode = "Shell shows bottom nav - app region shorter; Cancel key can go to app.";
    else
        mode = "Nav hidden - app region grows; shell status bar still drawn by shell.";
    snprintf(buf, sizeof(buf), "Viewport %u x %u px\nShell nav: %s\n\n%s",
             ctx->vp_w, ctx->vp_h, ctx->nav_visible ? "VISIBLE" : "HIDDEN", mode);
    lv_label_set_text(ctx->info_label, buf);
}

static int app_send_nav_req(app_ctx_t *ctx, bool visible)
{
    runtime_msg_t m;
    memset(&m, 0, sizeof(m));
    m.type = MSG_APP_NAV_REQ;
    m.seq = ++ctx->seq;
    m.nav_visible = visible ? 1 : 0;
    return ipc_send_msg(ctx->sock, &m);
}

static int app_send_present(app_ctx_t *ctx, uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    runtime_msg_t m;
    memset(&m, 0, sizeof(m));
    m.type = MSG_APP_PRESENT;
    m.seq = ++ctx->seq;
    m.dirty_x = x;
    m.dirty_y = y;
    m.dirty_w = w;
    m.dirty_h = h;
    return ipc_send_msg(ctx->sock, &m);
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

int app_main(void)
{
    app_ctx_t ctx;
    runtime_msg_t in, out;
    int rc;
    int recv_fd = -1;
    memset(&ctx, 0, sizeof(ctx));
    ctx.back_enabled = true;

    ctx.sock = app_connect();
    if (ctx.sock < 0) return 1;

    memset(&out, 0, sizeof(out));
    out.type = MSG_APP_REGISTER;
    if (ipc_send_msg(ctx.sock, &out) != 0) return 1;

    memset(&in, 0, sizeof(in));
    rc = ipc_recv_msg_with_fd(ctx.sock, &in, &recv_fd);
    if (rc != 0 || in.type != MSG_SHELL_SURFACE || recv_fd < 0) return 1;

    ctx.memfd = recv_fd;
    ctx.vp_w = in.vp_w;
    ctx.vp_h = in.vp_h;
    ctx.stride = in.stride_bytes;
    ctx.nav_visible = in.nav_visible != 0;

    size_t map_size = (size_t)ctx.stride * (SCREEN_H - STATUS_H);
    ctx.surface = (uint8_t *)mmap(NULL, map_size, PROT_READ | PROT_WRITE, MAP_SHARED, ctx.memfd, 0);
    if (ctx.surface == MAP_FAILED) return 1;

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

    app_build_ui(&ctx);
    app_update_info(&ctx);

    printf("[app] started, viewport=%ux%u\n", ctx.vp_w, ctx.vp_h);

    while (1) {
        struct timespec ts = {0, 33 * 1000 * 1000};
        nanosleep(&ts, NULL);
        ctx.tick_ms += 33;

        if (ctx.tick_ms % 3000 < 33) {
            ctx.nav_visible = !ctx.nav_visible;
            app_send_nav_req(&ctx, ctx.nav_visible);
            app_update_info(&ctx);
            printf("[app] request nav %s\n", ctx.nav_visible ? "show" : "hide");
        }

        {
            struct pollfd pfd;
            pfd.fd = ctx.sock;
            pfd.events = POLLIN;
            if (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN)) {
                runtime_msg_t m;
                rc = ipc_recv_msg(ctx.sock, &m);
                if (rc == 1) return 0;
                if (rc == 0 && m.type == MSG_SHELL_VIEWPORT_UPDATE) {
                    ctx.vp_w = m.vp_w;
                    ctx.vp_h = m.vp_h;
                    ctx.nav_visible = m.nav_visible != 0;
                    lv_display_set_resolution(ctx.disp, ctx.vp_w, ctx.vp_h);
                    lv_obj_set_size(ctx.root, ctx.vp_w, ctx.vp_h);
                    lv_obj_set_width(ctx.info_label, ctx.vp_w - 24);
                    lv_obj_set_style_bg_color(ctx.root,
                                              ctx.nav_visible ? lv_color_hex(0x37474F) : lv_color_hex(0x455A64),
                                              LV_PART_MAIN);
                    app_update_info(&ctx);
                    lv_obj_update_layout(ctx.root);
                    printf("[app] viewport update -> %ux%u\n", ctx.vp_w, ctx.vp_h);
                }
            }
        }

        /*
         * Do not memcpy / MSG_APP_PRESENT the shared surface before lv_timer_handler().
         * app_render_frame() used to fill the whole buffer and flush first, which sent a
         * full-frame PRESENT with only the gradient ¡ª wiping LVGL labels on the shell.
         */
        lv_tick_inc(33);
        (void)lv_timer_handler();
    }
}
