# Shell/App Runtime Demo (Compile & Run)

这个 Demo 用于验证以下流程是否可行：

- Shell 进程独占显示（模拟物理屏初始化）
- App 进程只绘制内容区
- App flush 后上报 PRESENT 脏区
- Shell 合成状态栏/导航栏 + App 内容区
- App 可请求隐藏/显示导航栏，Shell 回推 viewport，App 自动重布局

> 说明：为了保证可独立编译运行，Demo 使用 `lvgl_stub` 保留了 LVGL 常用接口名（`lv_timer_handler` / `lv_obj_create` / `lv_scr_act` 等），用于验证**进程架构和刷新链路**。  
> 切换到真实 LVGL 时，只需替换 `lvgl_stub.*` 与对应初始化/驱动绑定。

## Build

```bash
cd examples/shell_app_runtime
make
```

## Run

```bash
./shell_runtime
```

Shell 会自动拉起 `./app_runtime`，运行后你会看到：

- 终端日志中的 `viewport update`
- app 每 3 秒请求一次导航栏显隐
- `/tmp/shell_frame_XXX.ppm` 周期性输出合成结果（可用图片工具查看）

## 关键文件

- `shell_runtime.c`：shell 进程，含“屏幕初始化、launcher/chrome、合成刷新”
- `app_runtime.c`：app 进程，含“内容区绘制、flush->present、动态请求导航栏”
- `protocol.h`：IPC 消息定义
- `ipc.c`：socket + fd 传递
- `lvgl_stub.c`：最小 LVGL 接口桩

