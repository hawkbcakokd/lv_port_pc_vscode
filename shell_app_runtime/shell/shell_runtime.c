/*
 * Shell / launcher process with foreground app compositor.
 */
#define _GNU_SOURCE
#include "protocol.h"
#include "ipc.h"
#include "lvgl/lvgl.h"
#include "lvgl/src/draw/lv_draw_buf.h"
#include "lvgl/src/drivers/sdl/lv_sdl_private.h"

#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <limits.h>
#if defined(__linux__)
#include <libgen.h>
#endif

#define MAX_APPS 3

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
    uint16_t id;
    const char *name;
    const char *bin_name;
    pid_t pid;
    int conn_fd;
    int memfd;
    uint8_t *surface;
    uint32_t stride;
    size_t surface_size;
    bool connected;
    bool launched;
    uint32_t bg_present_cnt; /* MSG_APP_PRESENT received while this app is not foreground (not composited) */
} shell_app_slot_t;

typedef struct shell_ctx_t shell_ctx_t;

typedef struct {
    shell_ctx_t *ctx;
    uint16_t app_id;
} launch_ud_t;

struct shell_ctx_t {
    uint16_t vp_x, vp_y, vp_w, vp_h;
    uint32_t seq;
    bool nav_visible;
    bool status_visible;
    bool launcher_mode;
    uint16_t fg_app_id;
    int listen_fd;
    lv_display_t *disp;
    lv_obj_t *root;
    lv_obj_t *status_bar;
    lv_obj_t *content_host;
    lv_obj_t *launcher;
    lv_obj_t *nav_bar;
    lv_obj_t *touch_proxy;
    lv_obj_t *status_label;
    bool status_dirty;
    uint32_t status_last_tick;
    launch_ud_t launch_ud[MAX_APPS];
    shell_app_slot_t apps[MAX_APPS];
};

static volatile sig_atomic_t g_stop;
static void on_sigint(int signo) { (void)signo; g_stop = 1; }

static void shell_status_paint(shell_ctx_t *ctx);
static void shell_status_mark_dirty(shell_ctx_t *ctx);

static shell_app_slot_t *shell_find_app(shell_ctx_t *ctx, uint16_t id)
{
    int i;
    for (i = 0; i < MAX_APPS; i++) if (ctx->apps[i].id == id) return &ctx->apps[i];
    return NULL;
}

static void shell_calc_viewport(shell_ctx_t *ctx)
{
    ctx->vp_x = 0;
    ctx->vp_y = ctx->status_visible ? STATUS_H : 0;
    ctx->vp_w = SCREEN_W;
    ctx->vp_h = (uint16_t)(SCREEN_H - ctx->vp_y - (ctx->nav_visible ? NAV_H : 0));
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
    if (listen(fd, 16) < 0) return -1;
    return fd;
}

static int shell_create_app_surface(shell_app_slot_t *app)
{
    app->stride = SCREEN_W * BPP;
    app->surface_size = (size_t)app->stride * SCREEN_H;
    app->memfd = memfd_create("app_surface", MFD_CLOEXEC);
    if (app->memfd < 0) return -1;
    if (ftruncate(app->memfd, (off_t)app->surface_size) < 0) return -1;
    app->surface = (uint8_t *)mmap(NULL, app->surface_size, PROT_READ | PROT_WRITE, MAP_SHARED, app->memfd, 0);
    if (app->surface == MAP_FAILED) return -1;
    memset(app->surface, 0, app->surface_size);
    return 0;
}

static void shell_send_msg(shell_ctx_t *ctx, shell_app_slot_t *app, runtime_msg_t *m)
{
    if (!app || app->conn_fd < 0) return;
    m->seq = ++ctx->seq;
    m->app_id = app->id;
    ipc_send_msg(app->conn_fd, m);
}

static void shell_send_viewport(shell_ctx_t *ctx, shell_app_slot_t *app)
{
    runtime_msg_t m;
    memset(&m, 0, sizeof(m));
    m.type = MSG_SHELL_VIEWPORT_UPDATE;
    m.vp_x = ctx->vp_x;
    m.vp_y = ctx->vp_y;
    m.vp_w = ctx->vp_w;
    m.vp_h = ctx->vp_h;
    m.stride_bytes = app->stride;
    m.nav_visible = ctx->nav_visible ? 1 : 0;
    shell_send_msg(ctx, app, &m);
}

static void shell_send_foreground(shell_ctx_t *ctx)
{
    int i;
    runtime_msg_t m;
    for (i = 0; i < MAX_APPS; i++) {
        shell_app_slot_t *app = &ctx->apps[i];
        if (!app->connected) continue;
        memset(&m, 0, sizeof(m));
        m.type = MSG_SHELL_SET_FOREGROUND;
        m.input_state = (!ctx->launcher_mode && ctx->fg_app_id == app->id) ? 1 : 0;
        shell_send_msg(ctx, app, &m);
    }
}

static void shell_send_home(shell_ctx_t *ctx)
{
    int i;
    runtime_msg_t m;
    for (i = 0; i < MAX_APPS; i++) {
        shell_app_slot_t *app = &ctx->apps[i];
        if (!app->connected) continue;
        memset(&m, 0, sizeof(m));
        m.type = MSG_SHELL_GO_HOME;
        shell_send_msg(ctx, app, &m);
    }
}

static void shell_blit(shell_ctx_t *ctx, shell_app_slot_t *app, const runtime_msg_t *m)
{
    shell_sdl_sw_display_data_t *ddata;
    uint32_t stride;
    uint16_t x1, y1, w, h, y;
    if (!app || !app->surface || !ctx->disp) return;
    ddata = (shell_sdl_sw_display_data_t *)lv_sdl_backend_get_display_data(ctx->disp);
    if (!ddata || !ddata->fb1) return;
    if (!ddata->fb_act) ddata->fb_act = ddata->fb1;
    stride = lv_draw_buf_width_to_stride(lv_display_get_horizontal_resolution(ctx->disp),
                                         lv_display_get_color_format(ctx->disp));
    x1 = m->dirty_x;
    y1 = m->dirty_y;
    w = m->dirty_w == 0 ? ctx->vp_w : m->dirty_w;
    h = m->dirty_h == 0 ? ctx->vp_h : m->dirty_h;
    if (x1 >= ctx->vp_w || y1 >= ctx->vp_h) return;
    if (x1 + w > ctx->vp_w) w = (uint16_t)(ctx->vp_w - x1);
    if (y1 + h > ctx->vp_h) h = (uint16_t)(ctx->vp_h - y1);
    if (w == 0 || h == 0) return;
    for (y = 0; y < h; y++) {
        uint8_t *dst = ddata->fb_act + (uint32_t)(ctx->vp_y + y1 + y) * stride + (uint32_t)(ctx->vp_x + x1) * BPP;
        uint8_t *src = app->surface + (uint32_t)(y1 + y) * app->stride + (uint32_t)x1 * BPP;
        memcpy(dst, src, (size_t)w * BPP);
    }
    lv_sdl_backend_ops.redraw(ctx->disp);
}

/* Status line: shows foreground app + per-app count of PRESENT while in background (proves apps keep rendering; shell still composites fg only). */
static void shell_status_paint(shell_ctx_t *ctx)
{
    char buf[384];
    int off = 0;
    uint32_t now;
    int i;
    const char *fgn = "-";
    if (!ctx->status_label) return;
    now = lv_tick_get();
    if (!ctx->status_dirty) return;
    if (ctx->status_last_tick != 0 && (now - ctx->status_last_tick) < 200) return;
    ctx->status_last_tick = now;
    ctx->status_dirty = false;

    if (ctx->launcher_mode || ctx->fg_app_id == 0)
        fgn = "launcher";
    else {
        shell_app_slot_t *fa = shell_find_app(ctx, ctx->fg_app_id);
        if (fa) fgn = fa->name;
    }
    /* Single line: fits fixed STATUS_H without changing viewport math */
    off = snprintf(buf, sizeof(buf), "Fg:%s | bgPRESENT", fgn);
    for (i = 0; i < MAX_APPS; i++) {
        if (!ctx->apps[i].connected) continue;
        off += snprintf(buf + off, sizeof(buf) - (size_t)off, " id%u:%u", ctx->apps[i].id, ctx->apps[i].bg_present_cnt);
        if (off >= (int)sizeof(buf) - 8) break;
    }
    lv_label_set_text(ctx->status_label, buf);
}

static void shell_status_mark_dirty(shell_ctx_t *ctx)
{
    ctx->status_dirty = true;
}

static int shell_resolve_bin(char *buf, size_t buflen, const char *name)
{
#if defined(__linux__)
    char exe[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0) {
        exe[n] = '\0';
        char *copy = strdup(exe);
        if (copy) {
            char *dir = dirname(copy);
            snprintf(buf, buflen, "%s/%s", dir, name);
            free(copy);
            return 0;
        }
    }
#endif
    snprintf(buf, buflen, "./%s", name);
    return 0;
}

static void shell_launch(shell_app_slot_t *app)
{
    char path[PATH_MAX];
    pid_t pid;
    if (!app || app->launched) return;
    shell_resolve_bin(path, sizeof(path), app->bin_name);
    pid = fork();
    if (pid == 0) {
        char *args[] = {path, NULL};
        execvp(path, args);
        _exit(127);
    }
    if (pid > 0) {
        app->pid = pid;
        app->launched = true;
    }
}

static void shell_enter_launcher(shell_ctx_t *ctx)
{
    ctx->launcher_mode = true;
    ctx->fg_app_id = 0;
    lv_obj_clear_flag(ctx->content_host, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(ctx->launcher, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ctx->touch_proxy, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(ctx->root);
    shell_send_home(ctx);
    shell_send_foreground(ctx);
    ctx->status_dirty = true;
    ctx->status_last_tick = 0;
    shell_status_paint(ctx);
}

static void shell_set_foreground(shell_ctx_t *ctx, uint16_t app_id)
{
    shell_app_slot_t *app = shell_find_app(ctx, app_id);
    runtime_msg_t full;
    if (!app) return;
    shell_launch(app);
    ctx->launcher_mode = false;
    ctx->fg_app_id = app_id;
    lv_obj_add_flag(ctx->content_host, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ctx->launcher, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(ctx->touch_proxy, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(ctx->root);
    shell_send_foreground(ctx);
    if (app->connected) {
        memset(&full, 0, sizeof(full));
        shell_blit(ctx, app, &full);
    }
    ctx->status_dirty = true;
    ctx->status_last_tick = 0;
    shell_status_paint(ctx);
}

static void shell_update_layout(shell_ctx_t *ctx)
{
    shell_calc_viewport(ctx);
    if (ctx->status_visible) lv_obj_clear_flag(ctx->status_bar, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(ctx->status_bar, LV_OBJ_FLAG_HIDDEN);
    if (ctx->nav_visible) lv_obj_clear_flag(ctx->nav_bar, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(ctx->nav_bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(ctx->content_host, ctx->vp_x, ctx->vp_y);
    lv_obj_set_size(ctx->content_host, ctx->vp_w, ctx->vp_h);
    lv_obj_set_size(ctx->launcher, ctx->vp_w, ctx->vp_h);
    lv_obj_set_pos(ctx->touch_proxy, ctx->vp_x, ctx->vp_y);
    lv_obj_set_size(ctx->touch_proxy, ctx->vp_w, ctx->vp_h);
    lv_obj_update_layout(ctx->root);
    lv_obj_invalidate(ctx->root);
}

static void shell_set_nav(shell_ctx_t *ctx, bool visible)
{
    int i;
    runtime_msg_t full;
    shell_app_slot_t *fg;
    ctx->nav_visible = visible;
    shell_update_layout(ctx);
    for (i = 0; i < MAX_APPS; i++) if (ctx->apps[i].connected) shell_send_viewport(ctx, &ctx->apps[i]);
    /* Re-composite immediately so the middle band matches new viewport (reduces flash). */
    if (!ctx->launcher_mode && ctx->fg_app_id != 0) {
        fg = shell_find_app(ctx, ctx->fg_app_id);
        if (fg && fg->connected) {
            memset(&full, 0, sizeof(full));
            shell_blit(ctx, fg, &full);
        }
    }
}

static void ev_home(lv_event_t *e)
{
    shell_ctx_t *ctx = (shell_ctx_t *)lv_event_get_user_data(e);
    if (ctx) shell_enter_launcher(ctx);
}

static void ev_hide_nav(lv_event_t *e)
{
    shell_ctx_t *ctx = (shell_ctx_t *)lv_event_get_user_data(e);
    if (!ctx) return;
    shell_set_nav(ctx, false);
}

static void ev_launch(lv_event_t *e)
{
    launch_ud_t *ud = (launch_ud_t *)lv_event_get_user_data(e);
    if (ud && ud->ctx) shell_set_foreground(ud->ctx, ud->app_id);
}

static void ev_touch(lv_event_t *e)
{
    shell_ctx_t *ctx = (shell_ctx_t *)lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);
    shell_app_slot_t *fg;
    lv_indev_t *indev;
    lv_point_t p;
    runtime_msg_t m;
    if (!ctx || ctx->launcher_mode || ctx->fg_app_id == 0) return;
    fg = shell_find_app(ctx, ctx->fg_app_id);
    if (!fg || !fg->connected) return;
    if (code != LV_EVENT_PRESSED && code != LV_EVENT_PRESSING && code != LV_EVENT_RELEASED) return;
    indev = lv_event_get_indev(e);
    if (!indev) return;
    lv_indev_get_point(indev, &p);
    if (p.x < ctx->vp_x || p.y < ctx->vp_y) return;
    memset(&m, 0, sizeof(m));
    m.type = MSG_SHELL_INPUT_POINTER;
    m.input_x = (uint16_t)(p.x - ctx->vp_x);
    m.input_y = (uint16_t)(p.y - ctx->vp_y);
    m.input_state = code == LV_EVENT_RELEASED ? 0 : 1;
    shell_send_msg(ctx, fg, &m);
}

static lv_obj_t *mk_btn(lv_obj_t *parent, const char *text, lv_event_cb_t cb, shell_ctx_t *ctx)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_t *lb = lv_label_create(btn);
    lv_obj_set_size(btn, 92, NAV_H - 12);
    lv_label_set_text(lb, text);
    lv_obj_center(lb);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, ctx);
    return lb;
}

/* After shell LVGL redraws chrome, middle band is overwritten; composite foreground app again. */
static void shell_refr_ready_cb(lv_event_t *e)
{
    shell_ctx_t *ctx = (shell_ctx_t *)lv_event_get_user_data(e);
    runtime_msg_t full;
    shell_app_slot_t *app;
    if (!ctx || ctx->launcher_mode || ctx->fg_app_id == 0) return;
    app = shell_find_app(ctx, ctx->fg_app_id);
    if (!app || !app->connected || !app->surface) return;
    memset(&full, 0, sizeof(full));
    shell_blit(ctx, app, &full);
}

static void shell_create_ui(shell_ctx_t *ctx)
{
    lv_obj_t *list, *btn, *lb;
    int i;
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
    lv_obj_set_layout(ctx->status_bar, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(ctx->status_bar, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_left(ctx->status_bar, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_right(ctx->status_bar, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_top(ctx->status_bar, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(ctx->status_bar, 2, LV_PART_MAIN);
    ctx->status_label = lv_label_create(ctx->status_bar);
    lv_obj_set_width(ctx->status_label, SCREEN_W - 12);
    lv_label_set_long_mode(ctx->status_label, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    lv_obj_set_style_text_color(ctx->status_label, lv_color_hex(0xECEFF1), LV_PART_MAIN);
    lv_label_set_text(ctx->status_label, "Fg:- | bgPRESENT");

    ctx->content_host = lv_obj_create(ctx->root);
    lv_obj_set_pos(ctx->content_host, ctx->vp_x, ctx->vp_y);
    lv_obj_set_size(ctx->content_host, ctx->vp_w, ctx->vp_h);
    ctx->launcher = lv_obj_create(ctx->content_host);
    lv_obj_set_size(ctx->launcher, ctx->vp_w, ctx->vp_h);
    lv_obj_set_layout(ctx->launcher, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(ctx->launcher, LV_FLEX_FLOW_COLUMN);
    lb = lv_label_create(ctx->launcher);
    lv_label_set_text(lb, "Launcher: select foreground app");
    list = lv_obj_create(ctx->launcher);
    lv_obj_set_size(list, ctx->vp_w - 16, ctx->vp_h - 48);
    lv_obj_set_layout(list, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    for (i = 0; i < MAX_APPS; i++) {
        char text[64];
        snprintf(text, sizeof(text), "Start %s (id=%u)", ctx->apps[i].name, ctx->apps[i].id);
        btn = lv_button_create(list);
        lv_obj_set_width(btn, lv_pct(100));
        lb = lv_label_create(btn);
        lv_label_set_text(lb, text);
        lv_obj_center(lb);
        ctx->launch_ud[i].ctx = ctx;
        ctx->launch_ud[i].app_id = ctx->apps[i].id;
        lv_obj_add_event_cb(btn, ev_launch, LV_EVENT_CLICKED, &ctx->launch_ud[i]);
    }

    ctx->touch_proxy = lv_obj_create(ctx->root);
    lv_obj_set_style_bg_opa(ctx->touch_proxy, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(ctx->touch_proxy, 0, LV_PART_MAIN);
    lv_obj_add_flag(ctx->touch_proxy, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(ctx->touch_proxy, ev_touch, LV_EVENT_PRESSED, ctx);
    lv_obj_add_event_cb(ctx->touch_proxy, ev_touch, LV_EVENT_PRESSING, ctx);
    lv_obj_add_event_cb(ctx->touch_proxy, ev_touch, LV_EVENT_RELEASED, ctx);

    ctx->nav_bar = lv_obj_create(ctx->root);
    lv_obj_set_pos(ctx->nav_bar, 0, SCREEN_H - NAV_H);
    lv_obj_set_size(ctx->nav_bar, SCREEN_W, NAV_H);
    lv_obj_set_layout(ctx->nav_bar, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(ctx->nav_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ctx->nav_bar, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    mk_btn(ctx->nav_bar, "Back", ev_home, ctx);
    mk_btn(ctx->nav_bar, "Home", ev_home, ctx);
    /* Hides bottom navigation only (not status bar). App shows button A to show nav again. */
    mk_btn(ctx->nav_bar, "HideNav", ev_hide_nav, ctx);
    shell_update_layout(ctx);
}

static int shell_accept_app(shell_ctx_t *ctx)
{
    int fd;
    runtime_msg_t reg, out;
    shell_app_slot_t *app;
    fd = accept(ctx->listen_fd, NULL, NULL);
    if (fd < 0) return -1;
    memset(&reg, 0, sizeof(reg));
    if (ipc_recv_msg(fd, &reg) != 0 || reg.type != MSG_APP_REGISTER) {
        close(fd);
        return -1;
    }
    app = shell_find_app(ctx, reg.app_id);
    if (!app) {
        close(fd);
        return -1;
    }
    app->conn_fd = fd;
    app->connected = true;
    if (app->memfd < 0 && shell_create_app_surface(app) < 0) return -1;
    memset(&out, 0, sizeof(out));
    out.type = MSG_SHELL_SURFACE;
    out.vp_x = ctx->vp_x;
    out.vp_y = ctx->vp_y;
    out.vp_w = ctx->vp_w;
    out.vp_h = ctx->vp_h;
    out.stride_bytes = app->stride;
    out.nav_visible = ctx->nav_visible ? 1 : 0;
    out.app_id = app->id;
    ipc_send_msg_with_fd(app->conn_fd, &out, app->memfd);
    shell_send_viewport(ctx, app);
    shell_send_foreground(ctx);
    ctx->status_dirty = true;
    ctx->status_last_tick = 0;
    shell_status_paint(ctx);
    return 0;
}

int shell_main(int argc, char **argv)
{
    shell_ctx_t ctx;
    int i;
    (void)argc;
    (void)argv;
    memset(&ctx, 0, sizeof(ctx));
    ctx.nav_visible = true;
    ctx.status_visible = true;
    ctx.launcher_mode = true;
    ctx.listen_fd = -1;
    ctx.disp = lv_display_get_default();
    if (!ctx.disp) return 1;
    ctx.apps[0] = (shell_app_slot_t){1, "App A", "app_runtime", 0, -1, -1, NULL, 0, 0, false, false};
    ctx.apps[1] = (shell_app_slot_t){2, "App B", "app_runtime_b", 0, -1, -1, NULL, 0, 0, false, false};
    ctx.apps[2] = (shell_app_slot_t){3, "App C", "app_runtime_c", 0, -1, -1, NULL, 0, 0, false, false};
    shell_calc_viewport(&ctx);
    shell_create_ui(&ctx);
    ctx.status_dirty = true;
    ctx.status_last_tick = 0;
    shell_status_paint(&ctx);
    lv_display_add_event_cb(ctx.disp, shell_refr_ready_cb, LV_EVENT_REFR_READY, &ctx);
    signal(SIGINT, on_sigint);
    ctx.listen_fd = shell_listen();
    if (ctx.listen_fd < 0) return 1;

    while (!g_stop) {
        struct pollfd pfds[1 + MAX_APPS];
        int nfds = 0;
        lv_tick_inc(20);
        (void)lv_timer_handler();
        pfds[nfds].fd = ctx.listen_fd;
        pfds[nfds].events = POLLIN;
        nfds++;
        for (i = 0; i < MAX_APPS; i++) {
            if (ctx.apps[i].connected && ctx.apps[i].conn_fd >= 0) {
                pfds[nfds].fd = ctx.apps[i].conn_fd;
                pfds[nfds].events = POLLIN;
                nfds++;
            }
        }
        if (poll(pfds, nfds, 20) <= 0) {
            shell_status_paint(&ctx);
            continue;
        }
        if (pfds[0].revents & POLLIN) shell_accept_app(&ctx);
        {
            int idx = 1;
            for (i = 0; i < MAX_APPS; i++) {
                shell_app_slot_t *app = &ctx.apps[i];
                runtime_msg_t in;
                int rc;
                if (!app->connected || app->conn_fd < 0) continue;
                if (!(pfds[idx].revents & POLLIN)) {
                    idx++;
                    continue;
                }
                memset(&in, 0, sizeof(in));
                rc = ipc_recv_msg(app->conn_fd, &in);
                if (rc != 0) {
                    close(app->conn_fd);
                    app->conn_fd = -1;
                    app->connected = false;
                    shell_status_mark_dirty(&ctx);
                    idx++;
                    continue;
                }
                if (in.type == MSG_APP_PRESENT && !ctx.launcher_mode) {
                    if (in.app_id == ctx.fg_app_id)
                        shell_blit(&ctx, app, &in);
                    else {
                        app->bg_present_cnt++;
                        shell_status_mark_dirty(&ctx);
                    }
                } else if (in.type == MSG_APP_NAV_REQ && !ctx.launcher_mode && in.app_id == ctx.fg_app_id) {
                    shell_set_nav(&ctx, in.nav_visible != 0);
                }
                idx++;
            }
        }
        shell_status_paint(&ctx);
    }

    for (i = 0; i < MAX_APPS; i++) if (ctx.apps[i].pid > 0) kill(ctx.apps[i].pid, SIGTERM);
    for (i = 0; i < MAX_APPS; i++) {
        if (ctx.apps[i].pid > 0) waitpid(ctx.apps[i].pid, NULL, 0);
        if (ctx.apps[i].conn_fd >= 0) close(ctx.apps[i].conn_fd);
    }
    if (ctx.listen_fd >= 0) close(ctx.listen_fd);
    unlink(RUNTIME_SOCK_PATH);
    return 0;
}
