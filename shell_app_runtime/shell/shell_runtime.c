/*
 * Shell / launcher process: owns SDL + full-screen LVGL (status bar, nav bar,
 * optional launcher UI in the content host). Allocates the shared middle-region
 * surface, forks or launches the foreground app, receives MSG_APP_PRESENT and
 * blits the app surface into the final framebuffer before redraw.
 */
#define _GNU_SOURCE
#include "protocol.h"
#include "ipc.h"
#include "lvgl/lvgl.h"
#include "lvgl/src/draw/lv_draw_buf.h"
#include "lvgl/src/drivers/sdl/lv_sdl_private.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <limits.h>
#if defined(__linux__)
#include <libgen.h>
#endif

/* Must match lvgl/src/drivers/sdl/lv_sdl_sw.c lv_sdl_sw_display_data_t (SDL sw backend only). */
typedef struct {
    void *texture;
    void *renderer;
    uint8_t *fb1;
    uint8_t *fb2;
    uint8_t *fb_act;
    uint8_t *buf1;
    uint8_t *buf2;
    uint8_t *rotated_buf;
    size_t rotated_buf_size;
} shell_sdl_sw_display_data_t;

typedef struct {
    int conn_fd;
    int app_pid;
    bool nav_visible;
    uint16_t vp_x, vp_y, vp_w, vp_h;
    uint32_t seq;

    int app_memfd;
    uint8_t *app_surface;
    uint32_t app_stride;
    size_t app_surface_size;

    lv_display_t *disp;

    lv_obj_t *root;
    lv_obj_t *status_bar;
    lv_obj_t *content_host;
    lv_obj_t *launcher;
    lv_obj_t *launcher_label;
    lv_obj_t *nav_bar;
} shell_ctx_t;

static volatile sig_atomic_t g_stop;

/* Default child: app_runtime next to this executable (bin/main + bin/app_runtime). */
static void shell_resolve_child_path(char *buf, size_t buflen, int argc, char **argv)
{
    if (argc >= 2 && argv[1] && argv[1][0]) {
        snprintf(buf, buflen, "%s", argv[1]);
        return;
    }
#if defined(__linux__)
    {
        char exe[PATH_MAX];
        ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
        if (n > 0) {
            char *copy;
            exe[n] = '\0';
            copy = strdup(exe);
            if (copy) {
                char *dir = dirname(copy);
                snprintf(buf, buflen, "%s/app_runtime", dir);
                free(copy);
                if (access(buf, X_OK) == 0)
                    return;
            }
        }
    }
#endif
    snprintf(buf, buflen, "./app_runtime");
}

static void on_sigint(int signo)
{
    (void)signo;
    g_stop = 1;
}

static void shell_calc_viewport(shell_ctx_t *ctx)
{
    ctx->vp_x = 0;
    ctx->vp_y = STATUS_H;
    ctx->vp_w = SCREEN_W;
    ctx->vp_h = (uint16_t)(SCREEN_H - STATUS_H - (ctx->nav_visible ? NAV_H : 0));
}

static void shell_blit_app_viewport(shell_ctx_t *ctx, lv_display_t *disp, const runtime_msg_t *m)
{
    shell_sdl_sw_display_data_t *ddata;
    uint32_t stride;
    uint16_t x1, y1, w, h;
    uint16_t y;

    if (!ctx->app_surface || !disp) return;

    ddata = (shell_sdl_sw_display_data_t *)lv_sdl_backend_get_display_data(disp);
    /* DIRECT mode: fb_act is only set at end of flush_cb; IPC can arrive first. */
    if (!ddata || !ddata->fb1) return;
    if (!ddata->fb_act)
        ddata->fb_act = ddata->fb1;

    stride = lv_draw_buf_width_to_stride(lv_display_get_horizontal_resolution(disp),
                                         lv_display_get_color_format(disp));

    x1 = m->dirty_x;
    y1 = m->dirty_y;
    w = m->dirty_w == 0 ? ctx->vp_w : m->dirty_w;
    h = m->dirty_h == 0 ? ctx->vp_h : m->dirty_h;

    if (x1 >= ctx->vp_w || y1 >= ctx->vp_h) return;
    if (x1 + w > ctx->vp_w) w = (uint16_t)(ctx->vp_w - x1);
    if (y1 + h > ctx->vp_h) h = (uint16_t)(ctx->vp_h - y1);

    for (y = 0; y < h; y++) {
        uint8_t *dst =
            ddata->fb_act + (uint32_t)(ctx->vp_y + y1 + y) * stride + (uint32_t)(ctx->vp_x + x1) * BPP;
        uint8_t *src = ctx->app_surface + (uint32_t)(y1 + y) * ctx->app_stride + (uint32_t)x1 * BPP;
        memcpy(dst, src, (size_t)w * BPP);
    }
}

static void shell_refr_ready_cb(lv_event_t *e)
{
    shell_ctx_t *ctx = lv_event_get_user_data(e);
    lv_display_t *disp = lv_event_get_target(e);
    runtime_msg_t full;
    if (!ctx || !ctx->app_surface || !disp) return;
    memset(&full, 0, sizeof(full));
    shell_blit_app_viewport(ctx, disp, &full);
    lv_sdl_backend_ops.redraw(disp);
}

static void shell_create_ui(shell_ctx_t *ctx)
{
    ctx->root = lv_obj_create(lv_scr_act());
    lv_obj_set_size(ctx->root, SCREEN_W, SCREEN_H);
    lv_obj_set_style_bg_color(ctx->root, lv_color_hex(0x121212), LV_PART_MAIN);
    lv_obj_set_style_border_width(ctx->root, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(ctx->root, 0, LV_PART_MAIN);

    ctx->status_bar = lv_obj_create(ctx->root);
    lv_obj_set_pos(ctx->status_bar, 0, 0);
    lv_obj_set_size(ctx->status_bar, SCREEN_W, STATUS_H);
    lv_obj_set_style_bg_color(ctx->status_bar, lv_color_hex(0x0D47A1), LV_PART_MAIN);
    lv_obj_set_style_border_width(ctx->status_bar, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(ctx->status_bar, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(ctx->status_bar, 4, LV_PART_MAIN);
    {
        lv_obj_t *t = lv_label_create(ctx->status_bar);
        lv_label_set_text(t, "Shell · status bar (always shell-owned)");
        lv_obj_set_style_text_color(t, lv_color_hex(0xE3F2FD), LV_PART_MAIN);
    }

    ctx->content_host = lv_obj_create(ctx->root);
    lv_obj_set_pos(ctx->content_host, ctx->vp_x, ctx->vp_y);
    lv_obj_set_size(ctx->content_host, ctx->vp_w, ctx->vp_h);
    lv_obj_set_style_bg_color(ctx->content_host, lv_color_hex(0x37474F), LV_PART_MAIN);
    lv_obj_set_style_border_width(ctx->content_host, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(ctx->content_host, lv_color_hex(0x78909C), LV_PART_MAIN);
    lv_obj_set_style_pad_all(ctx->content_host, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(ctx->content_host, 0, LV_PART_MAIN);

    ctx->launcher = lv_obj_create(ctx->content_host);
    lv_obj_set_size(ctx->launcher, ctx->vp_w, ctx->vp_h);
    lv_obj_set_style_bg_color(ctx->launcher, lv_color_hex(0x455A64), LV_PART_MAIN);
    lv_obj_set_style_border_width(ctx->launcher, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(ctx->launcher, 8, LV_PART_MAIN);
    ctx->launcher_label = lv_label_create(ctx->launcher);
    lv_label_set_text(ctx->launcher_label,
                      "Launcher / app grid\n(before starting app)\n\nPick an app from list (demo)");
    lv_obj_set_style_text_color(ctx->launcher_label, lv_color_hex(0xECEFF1), LV_PART_MAIN);
    lv_label_set_long_mode(ctx->launcher_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(ctx->launcher_label, ctx->vp_w - 16);
    lv_obj_align(ctx->launcher_label, LV_ALIGN_TOP_LEFT, 0, 0);

    ctx->nav_bar = lv_obj_create(ctx->root);
    lv_obj_set_pos(ctx->nav_bar, 0, SCREEN_H - NAV_H);
    lv_obj_set_size(ctx->nav_bar, SCREEN_W, NAV_H);
    lv_obj_set_style_bg_color(ctx->nav_bar, lv_color_hex(0x212121), LV_PART_MAIN);
    lv_obj_set_style_border_width(ctx->nav_bar, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(ctx->nav_bar, 6, LV_PART_MAIN);
    lv_obj_set_layout(ctx->nav_bar, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(ctx->nav_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ctx->nav_bar, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    {
        lv_obj_t *z;
        z = lv_button_create(ctx->nav_bar);
        lv_obj_set_size(z, 88, NAV_H - 12);
        lv_obj_set_style_bg_color(z, lv_color_hex(0xC62828), LV_PART_MAIN);
        lv_obj_t *lb = lv_label_create(z);
        lv_label_set_text(lb, "Back");
        lv_obj_center(lb);

        z = lv_button_create(ctx->nav_bar);
        lv_obj_set_size(z, 88, NAV_H - 12);
        lv_obj_set_style_bg_color(z, lv_color_hex(0x2E7D32), LV_PART_MAIN);
        lb = lv_label_create(z);
        lv_label_set_text(lb, "Home");
        lv_obj_center(lb);

        z = lv_button_create(ctx->nav_bar);
        lv_obj_set_size(z, 88, NAV_H - 12);
        lv_obj_set_style_bg_color(z, lv_color_hex(0xEF6C00), LV_PART_MAIN);
        lb = lv_label_create(z);
        lv_label_set_text(lb, "Cancel");
        lv_obj_center(lb);
    }

    lv_obj_update_layout(ctx->root);
}

static int shell_listen(void)
{
    int fd;
    struct sockaddr_un addr;
    unlink(RUNTIME_SOCK_PATH);

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, RUNTIME_SOCK_PATH, sizeof(addr.sun_path) - 1);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) return -1;
    if (listen(fd, 1) < 0) return -1;
    return fd;
}

static int shell_create_app_surface(shell_ctx_t *ctx)
{
    /* 最大内容区：隐藏 nav 时 */
    uint16_t max_h = (uint16_t)(SCREEN_H - STATUS_H);
    ctx->app_stride = SCREEN_W * BPP;
    ctx->app_surface_size = (size_t)ctx->app_stride * max_h;
    ctx->app_memfd = memfd_create("app_surface", MFD_CLOEXEC);
    if (ctx->app_memfd < 0) return -1;
    if (ftruncate(ctx->app_memfd, (off_t)ctx->app_surface_size) < 0) return -1;
    ctx->app_surface = (uint8_t *)mmap(NULL, ctx->app_surface_size, PROT_READ | PROT_WRITE,
                                       MAP_SHARED, ctx->app_memfd, 0);
    return ctx->app_surface == MAP_FAILED ? -1 : 0;
}

static void shell_send_viewport(shell_ctx_t *ctx)
{
    runtime_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = MSG_SHELL_VIEWPORT_UPDATE;
    msg.seq = ++ctx->seq;
    msg.vp_x = ctx->vp_x;
    msg.vp_y = ctx->vp_y;
    msg.vp_w = ctx->vp_w;
    msg.vp_h = ctx->vp_h;
    msg.stride_bytes = ctx->app_stride;
    msg.nav_visible = ctx->nav_visible ? 1 : 0;
    ipc_send_msg(ctx->conn_fd, &msg);
}

static void shell_set_nav_visible(shell_ctx_t *ctx, bool visible)
{
    ctx->nav_visible = visible;
    shell_calc_viewport(ctx);

    if (visible) lv_obj_clear_flag(ctx->nav_bar, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(ctx->nav_bar, LV_OBJ_FLAG_HIDDEN);

    lv_obj_set_size(ctx->content_host, ctx->vp_w, ctx->vp_h);
    lv_obj_set_size(ctx->launcher, ctx->vp_w, ctx->vp_h);
    if (ctx->launcher_label) lv_obj_set_width(ctx->launcher_label, ctx->vp_w - 16);
    lv_obj_update_layout(ctx->root);
    lv_obj_invalidate(ctx->root);
    shell_send_viewport(ctx);
    printf("[shell] nav %s, viewport=%ux%u\n", visible ? "show" : "hide", ctx->vp_w, ctx->vp_h);
}

static void shell_composite_present(shell_ctx_t *ctx, const runtime_msg_t *m)
{
    if (!ctx->disp) return;
    shell_blit_app_viewport(ctx, ctx->disp, m);
    lv_sdl_backend_ops.redraw(ctx->disp);
}

static void shell_dump_frame_ppm(shell_ctx_t *ctx, uint32_t frame_no)
{
    char path[64];
    FILE *fp;
    uint32_t x, y;
    shell_sdl_sw_display_data_t *ddata;
    uint32_t stride;
    uint8_t *base;

    if (!ctx->disp) return;
    ddata = (shell_sdl_sw_display_data_t *)lv_sdl_backend_get_display_data(ctx->disp);
    if (!ddata || !ddata->fb1) return;
    if (!ddata->fb_act)
        ddata->fb_act = ddata->fb1;

    stride = lv_draw_buf_width_to_stride(lv_display_get_horizontal_resolution(ctx->disp),
                                         lv_display_get_color_format(ctx->disp));
    base = ddata->fb_act;

    snprintf(path, sizeof(path), "/tmp/shell_frame_%03u.ppm", frame_no);
    fp = fopen(path, "wb");
    if (!fp) return;
    fprintf(fp, "P6\n%d %d\n255\n", SCREEN_W, SCREEN_H);
    for (y = 0; y < SCREEN_H; y++) {
        for (x = 0; x < SCREEN_W; x++) {
            uint32_t p = *(uint32_t *)(base + y * stride + x * BPP);
            uint8_t rgb[3];
            rgb[0] = (uint8_t)((p >> 16) & 0xFF);
            rgb[1] = (uint8_t)((p >> 8) & 0xFF);
            rgb[2] = (uint8_t)(p & 0xFF);
            fwrite(rgb, 1, 3, fp);
        }
    }
    fclose(fp);
}

int shell_main(int argc, char **argv)
{
    int listen_fd, rc;
    pid_t pid;
    shell_ctx_t ctx;
    uint32_t frames = 0;
    char child_path[PATH_MAX];

    memset(&ctx, 0, sizeof(ctx));
    ctx.nav_visible = true;
    ctx.disp = lv_display_get_default();
    if (ctx.disp == NULL) {
        fprintf(stderr, "[shell] no default display (hal_init missing?)\n");
        return 1;
    }

    shell_calc_viewport(&ctx);
    shell_create_ui(&ctx);
    lv_display_add_event_cb(ctx.disp, shell_refr_ready_cb, LV_EVENT_REFR_READY, &ctx);

    signal(SIGINT, on_sigint);
    listen_fd = shell_listen();
    if (listen_fd < 0) return 1;

    shell_resolve_child_path(child_path, sizeof(child_path), argc, argv);

    pid = fork();
    if (pid == 0) {
        char *args[] = {child_path, NULL};
        execvp(child_path, args);
        perror("execvp app_runtime");
        _exit(127);
    }
    if (pid < 0) {
        perror("fork");
        close(listen_fd);
        return 1;
    }
    ctx.app_pid = pid;

    /* Pump LVGL/SDL while the child starts so the window is not blank until accept(). */
    for (;;) {
        struct pollfd acc = {listen_fd, POLLIN, 0};
        if (g_stop) {
            close(listen_fd);
            kill(ctx.app_pid, SIGTERM);
            waitpid(ctx.app_pid, NULL, 0);
            return 1;
        }
        lv_tick_inc(10);
        (void)lv_timer_handler();
        if (poll(&acc, 1, 10) > 0 && (acc.revents & POLLIN))
            break;
    }
    ctx.conn_fd = accept(listen_fd, NULL, NULL);
    close(listen_fd);
    if (ctx.conn_fd < 0) return 1;

    if (shell_create_app_surface(&ctx) < 0) return 1;

    runtime_msg_t in, out;
    memset(&in, 0, sizeof(in));
    rc = ipc_recv_msg(ctx.conn_fd, &in);
    if (rc != 0 || in.type != MSG_APP_REGISTER) return 1;

    memset(&out, 0, sizeof(out));
    out.type = MSG_SHELL_SURFACE;
    out.seq = ++ctx.seq;
    out.vp_x = ctx.vp_x;
    out.vp_y = ctx.vp_y;
    out.vp_w = ctx.vp_w;
    out.vp_h = ctx.vp_h;
    out.stride_bytes = ctx.app_stride;
    out.nav_visible = 1;
    ipc_send_msg_with_fd(ctx.conn_fd, &out, ctx.app_memfd);
    shell_send_viewport(&ctx);

    /*
     * Middle region pixels come only from the app (memfd + MSG_APP_PRESENT).
     * Keep content_host + launcher hidden so LVGL does not repaint an opaque panel
     * over the viewport every frame (that was covering the composited app → flicker).
     */
    lv_obj_add_flag(ctx.launcher, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ctx.content_host, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(ctx.root);

    printf("[shell] started, app=%d, viewport=%ux%u\n", ctx.app_pid, ctx.vp_w, ctx.vp_h);

    /* One full LVGL pass so fb_act / texture are valid before first MSG_APP_PRESENT. */
    lv_timer_ready(lv_display_get_refr_timer(ctx.disp));
    lv_tick_inc(20);
    (void)lv_timer_handler();

    while (!g_stop) {
        lv_tick_inc(20);
        (void)lv_timer_handler();

        struct pollfd pfd;
        pfd.fd = ctx.conn_fd;
        pfd.events = POLLIN;
        if (poll(&pfd, 1, 20) > 0 && (pfd.revents & POLLIN)) {
            for (;;) {
                memset(&in, 0, sizeof(in));
                rc = ipc_recv_msg(ctx.conn_fd, &in);
                if (rc != 0) {
                    g_stop = 1;
                    break;
                }
                if (in.type == MSG_APP_PRESENT) {
                    shell_composite_present(&ctx, &in);
                    frames++;
                    if (frames % 30 == 1) shell_dump_frame_ppm(&ctx, frames / 30);
                } else if (in.type == MSG_APP_NAV_REQ) {
                    shell_set_nav_visible(&ctx, in.nav_visible != 0);
                }
                pfd.revents = 0;
                if (poll(&pfd, 1, 0) <= 0 || !(pfd.revents & POLLIN)) break;
            }
        }
    }

    kill(ctx.app_pid, SIGTERM);
    waitpid(ctx.app_pid, NULL, 0);
    close(ctx.conn_fd);
    unlink(RUNTIME_SOCK_PATH);
    return 0;
}
