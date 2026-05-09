/*
 * Minimal LVGL + SDL sanity check (no IPC, no fork).
 * Run: ./shell_sdl_smoke  ¡ª expect green full-screen window briefly.
 * Use to verify DISPLAY / WSLg / SDL2 before debugging shell/app_runtime.
 */
#include <stdio.h>
#include <stdlib.h>

#include "lvgl/lvgl.h"

int main(void)
{
    lv_init();

    lv_display_t *disp = lv_sdl_window_create(320, 480);
    if (disp == NULL) {
        fprintf(stderr, "shell_sdl_smoke: lv_sdl_window_create failed (SDL / display?)\n");
        return 2;
    }

    lv_obj_t *bg = lv_obj_create(lv_scr_act());
    lv_obj_set_size(bg, 320, 480);
    lv_obj_set_style_bg_color(bg, lv_color_hex(0x2E7D32), LV_PART_MAIN);
    lv_obj_t *l = lv_label_create(bg);
    lv_label_set_text(l, "SDL smoke OK");
    lv_obj_center(l);

    for (;;) {
        lv_tick_inc(16);
        lv_timer_handler();
    }

    return 0;
}
