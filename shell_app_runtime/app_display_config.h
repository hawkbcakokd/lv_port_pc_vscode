/*
 * App-process LVGL display buffering / render mode (compile-time).
 *
 * CMake (recommended), value 0..5:
 *   0 = LV_DISPLAY_RENDER_MODE_FULL,    single draw buffer (default)
 *   1 = LV_DISPLAY_RENDER_MODE_FULL,    double draw buffer
 *   2 = LV_DISPLAY_RENDER_MODE_PARTIAL, single strip (height APP_PARTIAL_BUF_LINES)
 *   3 = LV_DISPLAY_RENDER_MODE_PARTIAL, double strip (same strip size each)
 *   4 = LV_DISPLAY_RENDER_MODE_DIRECT,  single full-size buffer
 *   5 = LV_DISPLAY_RENDER_MODE_DIRECT,  double full-size buffer
 *
 * Migration: former mode "3" (DIRECT single) is now "4".
 *
 * Optional: -DAPP_PARTIAL_BUF_LINES=80  (PARTIAL strip height in pixels, <= max viewport height)
 */
#ifndef APP_DISPLAY_CONFIG_H
#define APP_DISPLAY_CONFIG_H

#ifndef APP_LV_DISPLAY_BUFFER_MODE
#define APP_LV_DISPLAY_BUFFER_MODE 0
#endif

#ifndef APP_PARTIAL_BUF_LINES
#define APP_PARTIAL_BUF_LINES 80
#endif

#define APP_LV_BUF_FULL_SINGLE 0
#define APP_LV_BUF_FULL_DOUBLE 1
#define APP_LV_BUF_PARTIAL_SINGLE 2
#define APP_LV_BUF_PARTIAL_DOUBLE 3
#define APP_LV_BUF_DIRECT_SINGLE 4
#define APP_LV_BUF_DIRECT_DOUBLE 5

#endif
