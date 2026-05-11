# 合成、触摸、按键与共享内存安全（深入说明）

本文与当前实现 `shell_app_runtime/shell/shell_runtime.c`、`app/app_runtime.c`、`protocol.h` 对齐，并说明可扩展点。

---

## 1. 触摸和按键是在哪里捕获的？

### 1.1 当前工程（Shell = 唯一接 HAL 的 LVGL 进程）

- **物理触摸 / 鼠标**：由 **Board HAL**（例如 PC 上 SDL 创建的 `lv_sdl_mouse`）挂到 **Shell 进程的默认 `lv_display_t`** 上。
- **LVGL 事件分发**：输入设备驱动在 Shell 的主循环里被 LVGL 轮询；**命中哪个 `lv_obj` 由 Shell 这棵树上的焦点与层级决定**。
- **应用进程**：**没有** SDL 窗口、**没有**直接接触摸屏；应用里的 `lv_indev_t` 的 `read_cb` 从 **内存里的 `ptr_point` / `ptr_state`** 读数，这些数来自 **Socket 上收到的 `MSG_SHELL_INPUT_POINTER`**。

因此：**触摸在 Shell 进程里被 LVGL「捕获」（更准确地说是 Shell 的 LVGL 树接收 indev 事件）**；应用进程是 **逻辑上** 收到同一次交互的「副本」（经协议）。

### 1.2 按键（物理键盘 / 侧键）

- 若 HAL 把键盘也接到 Shell 的 display：同样在 **Shell** 里进 LVGL 的 `LV_INDEV_TYPE_KEYPAD` 或类似。
- 当前仓库 **协议里已有** `MSG_SHELL_INPUT_KEY` 与 `key_code` 字段，但 **Shell/App 尚未接线**。扩展方式见下文第 4 节。

---

## 2. Shell 捕获的触摸如何传给前台应用？（数据流）

```text
SDL/触摸 HAL
    → Shell 的 lv_indev（全局坐标，相对整屏）
        → 若命中 touch_proxy 且在 viewport 内
            → ev_touch：减去 (vp_x, vp_y) 得到「应用坐标系」
                → ipc_send_msg(conn_fg, MSG_SHELL_INPUT_POINTER, ...)
                    → 应用进程 poll(sock)
                        → 更新 ctx.ptr_point / ctx.ptr_state
                            → lv_timer_handler() 里 indev read_cb 读出
                                → LVGL 命中测试应用自己的控件
```

Shell 侧核心逻辑（节选，与仓库一致）：

```375:397:shell_app_runtime/shell/shell_runtime.c
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
```

应用侧：把消息写进「假指针」状态，再交给 LVGL：

```165:170:shell_app_runtime/app/app_runtime.c
static void app_pointer_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    app_ctx_t *ctx = (app_ctx_t *)lv_indev_get_user_data(indev);
    data->point = ctx->ptr_point;
    data->state = ctx->ptr_state;
}
```

```268:273:shell_app_runtime/app/app_runtime.c
                else if (m.type == MSG_SHELL_INPUT_POINTER) {
                    ctx.ptr_point.x = m.input_x;
                    ctx.ptr_point.y = m.input_y;
                    ctx.ptr_state = m.input_state ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
                }
```

**要点**：应用坐标系是 **`[0, vp_w) × [0, vp_h)`**，与共享 surface 里 LVGL 的 display 分辨率一致；Shell 负责 **从屏幕坐标减 viewport 原点**。

---

## 3. `viewport`、`touch_proxy`、`MSG_SHELL_INPUT_POINTER` 分别是什么？

| 名词 | 含义 |
|------|------|
| **viewport（视口）** | 逻辑矩形 **`(vp_x, vp_y, vp_w, vp_h)`**：在**整屏**上，中间留给「应用内容 + 可选 launcher」的区域。受状态栏高度 `STATUS_H`、导航栏 `NAV_H`、是否隐藏导航等影响，由 `shell_calc_viewport()` 计算。应用收到的 `MSG_SHELL_SURFACE` / `VIEWPORT_UPDATE` 里带同一组参数，用于 **改 LVGL 分辨率** 和 **blit 目标在 memfd 内的布局**。 |
| **touch_proxy** | Shell 根节点上的一个 **全透明** `lv_obj`，几何与 viewport 对齐（`shell_update_layout` 里 `set_pos/set_size` 与 `content_host` 相同）。**作用**：在「应用前台模式」下盖住中间区域，**拦截**触摸事件并转成 `MSG_SHELL_INPUT_POINTER`，避免点击穿透到已隐藏的 launcher 或误触其它层。 |
| **`MSG_SHELL_INPUT_POINTER`** | Runtime 协议里的一条消息：`type` 固定，`input_x/input_y` 为 **应用坐标系** 下的触摸位置，`input_state` 表示按下/抬起。Shell 只发给 **当前前台** 连接；应用不解析 SDL，只解析这条消息。 |

---

## 4. 导航栏按键要「传给前台应用」怎么做？

### 4.1 当前实现：系统键在 Shell 内消费，不发给 App

`Back` / `Home` 绑定 `ev_home` → `shell_enter_launcher`，属于 **Shell 策略**，不经过应用。

```356:360:shell_app_runtime/shell/shell_runtime.c
static void ev_home(lv_event_t *e)
{
    shell_ctx_t *ctx = (shell_ctx_t *)lv_event_get_user_data(e);
    if (ctx) shell_enter_launcher(ctx);
}
```

当前仓库已实现：**SDL 键盘 → Shell `lv_indev_set_key_remap_cb` → 仅当前前台 `MSG_SHELL_INPUT_KEY` → 应用 `lv_group_send_data` → `lv_textarea`**（Launcher 模式不转发，按键仍留在 Shell 侧逻辑）。

### 4.2 若产品需要「软键事件也要应用知道」（例如游戏要响应 Back）

两种常见设计：

**A. 仍由 Shell 决定系统行为，但「通知」应用（推荐用于 Back）**

1. Shell 先 `shell_enter_launcher` 或 `shell_set_foreground`；
2. 再对**旧前台**发一条 **`MSG_SHELL_GO_HOME`** 或专用 **`MSG_SHELL_NAV`**（`key_code` = BACK），应用可做存档提示。

**B. 完全交给应用处理（适合非系统键）**

1. 导航栏某按钮点击 → 不调用 `shell_enter_launcher`；
2. 组包 **`MSG_SHELL_INPUT_KEY`**（协议已有 `key_code`），`shell_send_msg(ctx, fg, &m)`；
3. 应用 `poll` 收到后，调用 `lv_indev_send_key(...)` 或更新自定义状态机。

**极简扩展示例（Shell 伪代码）**

```c
/* 假设：导航栏「自定义键」要进前台应用 */
static void ev_softkey_to_app(lv_event_t *e)
{
    shell_ctx_t *ctx = lv_event_get_user_data(e);
    shell_app_slot_t *fg = shell_find_app(ctx, ctx->fg_app_id);
    runtime_msg_t m;

    if (!fg || !fg->connected || ctx->launcher_mode) return;
    memset(&m, 0, sizeof(m));
    m.type = MSG_SHELL_INPUT_KEY;
    m.key_code = 0x1001; /* 产品定义：软键1 */
    m.input_state = 1;   /* 按下；松开再发 0 可选 */
    shell_send_msg(ctx, fg, &m);
}
```

**应用侧伪代码**

```c
else if (m.type == MSG_SHELL_INPUT_KEY) {
    /* 方式1：喂给 LVGL 默认组 */
    lv_indev_t *kb = lv_indev_get_next(NULL);
    while (kb && lv_indev_get_type(kb) != LV_INDEV_TYPE_KEYPAD)
        kb = lv_indev_get_next(kb);
    if (kb) {
        lv_indev_send_event(kb, LV_EVENT_KEY, &m); /* API 以 LVGL 版本为准 */
    }
    /* 方式2：只更新业务状态 */
    // app_on_shell_key(m.key_code, m.input_state);
}
```

**设计建议**

- **Home / 系统安全相关**：建议 **始终在 Shell 内优先处理**，应用最多收到「你已失焦」类消息。
- **媒体键、Fn、软键盘**：适合 **`MSG_SHELL_INPUT_KEY`** 转发。

---

## 5. 调起前台应用后，Shell 要不要「隐藏」中间 `content_host`？

**当前实现：要隐藏 launcher 那一层，并显示 `touch_proxy`。**

```300:316:shell_app_runtime/shell/shell_runtime.c
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
    ...
}
```

**原因简述**

1. **`content_host` 隐藏**：中间像素不再来自 LVGL 子树（launcher 列表），而来自 **`shell_blit` 把应用 memfd 拷到 SDL framebuffer**；若仍显示 launcher，会盖住或逻辑混乱。
2. **`touch_proxy` 显示**：中间区域没有可点的 LVGL「应用控件」（应用在另一进程），必须在 Shell 上有一层 **透明点击面** 接收触摸并转发。
3. **状态栏 / 导航栏** 仍在，且创建顺序上 **导航栏在 `touch_proxy` 之后**，通常 **更靠上**（后建在上层），所以 **底部按钮仍由 Shell 处理**，不会误发给应用。

回到 Launcher 时：`shell_enter_launcher` 会 `clear_flag(content_host)`、`clear_flag(launcher)`、`add_flag(touch_proxy, HIDDEN)`，触摸再回到 launcher 列表按钮。

---

## 6. 合成（Compositor）在做什么？

- **Shell LVGL**：画状态栏、导航栏等 **chrome**。
- **中间矩形**：每一帧（或 dirty 时）把 **前台应用** 在 **共享 memfd** 里写入的像素 **`memcpy` 到最终 framebuffer 的 `(vp_x, vp_y)+dirty`**（见 `shell_blit`）。
- **非前台** 的 `MSG_APP_PRESENT`：当前实现里 **不 blit**（可计数用于调试）。

这是典型的 **「单合成器 + 多 client surface」** 的 CPU 版实现，只是协议是你们自定义的而不是 Wayland。

---

## 7. 共享内存与多进程安全

### 7.1 当前模型的信任边界

- **memfd 由 Shell 创建**（`memfd_create` + `ftruncate`），通过 **`sendmsg(..., SCM_RIGHTS)`** 只在 accept 时交给 **对应 app 的一条连接**。
- 每个应用 **只拿到自己的 fd**（在实现正确的前提下），**看不到**其他应用的 surface fd。
- 应用 `mmap(MAP_SHARED)` 后，与 Shell 映射 **同一块物理页**；写冲突只发生在 **「应用写」与「Shell 读 memcpy」** 之间，通常单 writer + 单 reader 用 **PRESENT 作为屏障** 即可（更严格可用 `eventfd`/fence，产品迭代再加）。

### 7.2 主要风险与缓解

| 风险 | 说明 | 缓解 |
|------|------|------|
| **伪造 app_id** | 恶意进程连 socket 声称自己是 app 2 | Unix 套接字上 **`SO_PEERCRED` / `getpeereid()`** 校验 UID；或 **抽象命名 socket + 文件权限 0600**；白名单只允许本机服务用户连接。 |
| **越界写 surface** | 应用 memcpy 写出 stride×height | Shell **blit 时裁剪** dirty 到 `vp_w/vp_h`；应用侧 LVGL resolution 与 viewport 一致；可选 **mprotect** 仅映射需要大小（复杂）。 |
| **PRESENT 伪造** | 乱发大 dirty 拖垮 Shell | Shell **拒绝** `dirty` 超 viewport；**节流** blit；后台 present **丢弃**。 |
| **协议注入** | 任意进程连 `/tmp/...sock` | **运行时路径改到 `/run/产品名/`**；systemd `PrivateTmp`；或 **仅 root/同组** 可连。 |
| **TOCTOU** | 读 mem 时应用正在写 | 简单产品：**只在收到 PRESENT 后读 dirty 区**；高要求：双缓冲 + 版本号在消息里。 |

### 7.3 与「多个应用」的关系

- **多个应用 = 多个 memfd（或多个独立映射区）**，Shell 表里 **`app_id → surface`**；**永远只把前台 surface blit 到屏**。
- **安全目标**不是「应用互相看不见内存」（那是沙箱/OS 级能力），而是 **「非授权进程不能冒充、不能读他人 fd、Shell 不被畸形消息打挂」**；上述条目按产品等级逐项加固即可。

---

## 8. 小结表

| 问题 | 结论 |
|------|------|
| 触摸在哪捕获？ | **Shell 进程**的 HAL → LVGL indev → 常由 **touch_proxy** 接住中间区。 |
| 如何到应用？ | **`MSG_SHELL_INPUT_POINTER`** → 应用更新 `ptr_point/state` → `lv_indev` read_cb。 |
| 导航键给应用？ | 系统键可 **Shell 自消费**；若要给应用，用 **`MSG_SHELL_INPUT_KEY`**（协议已预留）或专用 nav 消息。 |
| 是否隐藏中间 content？ | **前台应用模式：隐藏 `content_host`（含 launcher），显示 `touch_proxy`**；像素来自 **blit**。 |
| touch_proxy / viewport / MSG？ | 见 **第 3 节** 表格。 |
| 共享内存安全？ | **fd 只发一次给对应 client + blit 裁剪 + 连接鉴权 + sock 权限** 为基线；高安全再加 peer cred、双缓冲等。 |

如需把 **`MSG_SHELL_INPUT_KEY`** 在仓库里真正接通（Shell SDL 键盘 + App 侧 `lv_indev`），可以单开一个小 PR 专门做这一条链路。
