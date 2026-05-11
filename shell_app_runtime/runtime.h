#ifndef _RUNTIME_H
#define _RUNTIME_H

/*
 * Multi-process display model (Linux test harness):
 *
 * - Shell owns the real framebuffer / SDL window. It draws the status bar and
 *   bottom navigation in LVGL. The middle band is a shared "public" region: shell
 *   allocates it (e.g. memfd), maps it, and passes the fd to the current
 *   foreground app only.
 *
 * - Foreground app runs LVGL with flush_cb that does NOT talk to the physical
 *   display: it copies tiles into the shared region and notifies the shell
 *   (MSG_APP_PRESENT). The shell composites that region into the final image
 *   and presents it, so only one app's pixels appear in the middle at a time.
 *   Background apps may keep flushing + sending PRESENT; the shell counts those
 *   (for debug) but only blits the foreground id.
 *
 * CMake builds both programs in one pass (see CMakeLists.txt):
 *   bin/main         ¡ª target compiles with -DSHELL_PROCESS
 *   bin/app_runtime  ¡ª target compiles with -DAPP_PROCESS
 *
 * You no longer need to edit this file or rebuild twice. If you compile a
 * single file outside CMake, define one of the two macros yourself.
 */

extern int shell_main(int argc, char **argv);
extern int app_main(void);


#endif
