
/**
 * @file main
 *
 * Runtime is selected in shell_app_runtime/runtime.h:
 * - Shell binary: SHELL_PROCESS ¡ª SDL HAL + shell_main (compositor + LVGL chrome).
 * - App binary:   APP_PROCESS ¡ª LVGL headless for the shared middle region; no SDL.
 */

/*********************
 *      INCLUDES
 *********************/
#define _DEFAULT_SOURCE /* needed for usleep() */
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>
#include "lvgl/lvgl.h"
#include "lvgl/examples/lv_examples.h"
#include "lvgl/demos/lv_demos.h"
#include "glob.h"

// #include "runtime.h"

#if (defined(SHELL_PROCESS) || defined(APP_PROCESS))
#include "protocol.h"
#endif
/*********************
 *      DEFINES
 *********************/

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 *  STATIC PROTOTYPES
 **********************/
lv_display_t * hal_init(int32_t w, int32_t h);
#if defined(APP_PROCESS)
static void app_lv_init_hal_no_sdl(void);
#endif

/**********************
 *  STATIC VARIABLES
 **********************/

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

extern void freertos_main(void);

/*********************
 *      DEFINES
 *********************/

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 *      VARIABLES
 **********************/

/**********************
 *  STATIC PROTOTYPES
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

int main(int argc, char **argv)
{
  (void)argc; /*Unused*/
  (void)argv; /*Unused*/

  /*Initialize LVGL*/
  lv_init();

  /*Initialize the HAL (display, input devices, tick) for LVGL*/
#if defined(APP_PROCESS)
  /* App renders into shared memfd; no local SDL window or second HAL display. */
  app_lv_init_hal_no_sdl();
#elif defined(SHELL_PROCESS)
  /* SDL before shell_main: window + LVGL must run while we wait for app to connect. */
  hal_init(SCREEN_W, SCREEN_H);
#else
  hal_init(320, 480);
#endif

  #if LV_USE_OS == LV_OS_NONE

  #if (defined(SHELL_PROCESS))
  shell_main(argc, argv);
  #elif (defined(APP_PROCESS))
  app_main();
  #else

  lv_demo_widgets();

  while(1) {
    /* Periodically call the lv_task handler.
     * It could be done in a timer interrupt or an OS task too.*/
    lv_timer_handler();
    usleep(5 * 1000);
  }

  #endif

  #elif LV_USE_OS == LV_OS_FREERTOS

  /* Run FreeRTOS and create lvgl task */
  freertos_main();

  #endif

  return 0;
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

/**
 * Initialize the Hardware Abstraction Layer (HAL) for the LVGL graphics
 * library
 */
#if defined(APP_PROCESS)
static void app_lv_init_hal_no_sdl(void)
{
  lv_group_set_default(lv_group_create());
}
#endif

lv_display_t * hal_init(int32_t w, int32_t h)
{

  lv_group_set_default(lv_group_create());

  lv_display_t * disp = lv_sdl_window_create(w, h);

  lv_indev_t * mouse = lv_sdl_mouse_create();
  lv_indev_set_group(mouse, lv_group_get_default());
  lv_indev_set_display(mouse, disp);
  lv_display_set_default(disp);

  LV_IMAGE_DECLARE(mouse_cursor_icon); /*Declare the image file.*/
  lv_obj_t * cursor_obj;
  cursor_obj = lv_image_create(lv_screen_active()); /*Create an image object for the cursor */
  lv_image_set_src(cursor_obj, &mouse_cursor_icon);           /*Set the image source*/
  lv_indev_set_cursor(mouse, cursor_obj);             /*Connect the image  object to the driver*/

  lv_indev_t * mousewheel = lv_sdl_mousewheel_create();
  lv_indev_set_display(mousewheel, disp);
  lv_indev_set_group(mousewheel, lv_group_get_default());

  lv_indev_t * kb = lv_sdl_keyboard_create();
  lv_indev_set_display(kb, disp);
  lv_indev_set_group(kb, lv_group_get_default());

  return disp;
}
