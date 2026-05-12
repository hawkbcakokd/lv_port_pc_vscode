cmake_minimum_required(VERSION 3.10)
project(lvgl C CXX)

# Set the correct FreeRTOS port for your system (e.g., Posix for WSL)
set(FREERTOS_PORT GCC_POSIX CACHE STRING "Port for FreeRTOS on Posix environment")

unset(USE_FREERTOS CACHE)

option(USE_FREERTOS "Enable FreeRTOS" OFF) # Turn this on to enable FreeRTOS

if(USE_FREERTOS)
    message(STATUS "FreeRTOS is enabled")

    # FreeRTOS integration when USE_FREERTOS is enabled
    add_library(freertos_config INTERFACE)
    target_include_directories(freertos_config SYSTEM INTERFACE ${PROJECT_SOURCE_DIR}/config)
    target_compile_definitions(freertos_config INTERFACE projCOVERAGE_TEST=0)

    # Add FreeRTOS as a subdirectory
    add_subdirectory(FreeRTOS)

    # FreeRTOS-specific include directories
    include_directories(${PROJECT_SOURCE_DIR}/FreeRTOS/include)
    include_directories(${PROJECT_SOURCE_DIR}/FreeRTOS/portable/ThirdParty/GCC/Posix)
    include_directories(${PROJECT_SOURCE_DIR}/config)

    # Add FreeRTOS sources
    file(GLOB FREERTOS_SOURCES
        "${PROJECT_SOURCE_DIR}/FreeRTOS/*.c"
        "${PROJECT_SOURCE_DIR}/FreeRTOS/portable/MemMang/heap_4.c"
        "${PROJECT_SOURCE_DIR}/FreeRTOS/portable/ThirdParty/GCC/Posix/*.c"
    )
else()
    message(STATUS "FreeRTOS is disabled")
    set(FREERTOS_SOURCES "")  # No FreeRTOS sources if FreeRTOS is disabled
endif()

# Main include files of the project
include_directories(${PROJECT_SOURCE_DIR}/main/inc)

include_directories(${PROJECT_SOURCE_DIR}/shell_app_runtime)

# Define options for LVGL with default values (OFF)
option(LV_USE_DRAW_SDL "Use SDL draw unit" OFF)
option(LV_USE_LIBPNG "Use libpng to decode PNG" OFF)
option(LV_USE_LIBJPEG_TURBO "Use libjpeg turbo to decode JPEG" OFF)
option(LV_USE_FFMPEG "Use libffmpeg to display video using lv_ffmpeg" OFF)
option(LV_USE_FREETYPE "Use freetype library" OFF)

# Set C and C++ standards
set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Set the output path for the executable
set(EXECUTABLE_OUTPUT_PATH ${PROJECT_SOURCE_DIR}/bin)
set(WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})

# Find and include SDL2 library
find_package(SDL2 REQUIRED)

# Remove ARM-specific compile and linker options
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -march=native")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=native")

# Add compile definitions based on the selected options
add_compile_definitions($<$<BOOL:${LV_USE_DRAW_SDL}>:LV_USE_DRAW_SDL=1>)
add_compile_definitions($<$<BOOL:${LV_USE_LIBPNG}>:LV_USE_LIBPNG=1>)
add_compile_definitions($<$<BOOL:${LV_USE_LIBJPEG_TURBO}>:LV_USE_LIBJPEG_TURBO=1>)
add_compile_definitions($<$<BOOL:${LV_USE_FFMPEG}>:LV_USE_FFMPEG=1>)

# Add LVGL subdirectory
add_subdirectory(lvgl)
target_include_directories(lvgl PUBLIC ${PROJECT_SOURCE_DIR} ${SDL2_INCLUDE_DIRS})

# lv_conf.h lives next to lvgl/; #include "lvgl/lvgl.h" needs project root on the include path.
# Some generators/toolchains do not apply linked target INTERFACE includes during compile ¡ª set explicitly.
macro(lv_port_target_includes_and_link name)
    target_include_directories(${name} PRIVATE ${PROJECT_SOURCE_DIR} ${SDL2_INCLUDE_DIRS})
    target_link_libraries(${name} PRIVATE lvgl lvgl::examples lvgl::demos lvgl::thorvg ${SDL2_LIBRARIES} m pthread)
endmacro()

# Create the main executable, depending on the FreeRTOS option
if(USE_FREERTOS)
    add_executable(main
        ${PROJECT_SOURCE_DIR}/main/src/main.c
        ${PROJECT_SOURCE_DIR}/main/src/freertos_main.cpp
        ${PROJECT_SOURCE_DIR}/main/src/mouse_cursor_icon.c
        ${PROJECT_SOURCE_DIR}/main/src/FreeRTOS_Posix_Port.c
        ${FREERTOS_SOURCES}  # Add only if USE_FREERTOS is enabled
    )
    # Link FreeRTOS libraries
    target_link_libraries(main PRIVATE freertos_config FreeRTOS)
    target_compile_definitions(main PRIVATE LV_CONF_INCLUDE_SIMPLE)
    lv_port_target_includes_and_link(main)
else()
    add_executable(main
        ${PROJECT_SOURCE_DIR}/main/src/main.c
        ${PROJECT_SOURCE_DIR}/main/src/mouse_cursor_icon.c
    )
    target_compile_definitions(main PRIVATE LV_CONF_INCLUDE_SIMPLE)
    lv_port_target_includes_and_link(main)

    # App LVGL draw buffer mode (see shell_app_runtime/app_display_config.h).
    # CACHE default applies only on first configure; existing build/CMakeCache.txt wins.
    # Override: cmake -DAPP_LV_DISPLAY_BUFFER_MODE=2 ..   or unset: cmake -UAPP_LV_DISPLAY_BUFFER_MODE ..
    # set(APP_LV_DISPLAY_BUFFER_MODE "2" CACHE STRING "App LVGL: 0=FULL1 1=FULL2 2=PARTIAL1 3=PARTIAL2 4=DIRECT1 5=DIRECT2")
    set(APP_PARTIAL_BUF_LINES "80" CACHE STRING "PARTIAL strip height in pixels")
    add_definitions(-DAPP_LV_DISPLAY_BUFFER_MODE=5 )

    add_executable(shell
        ${PROJECT_SOURCE_DIR}/main/src/main.c
        ${PROJECT_SOURCE_DIR}/main/src/mouse_cursor_icon.c
        ${PROJECT_SOURCE_DIR}/shell_app_runtime/shell/shell_runtime.c
        ${PROJECT_SOURCE_DIR}/shell_app_runtime/ipc.c
    )
    target_compile_definitions(shell PRIVATE LV_CONF_INCLUDE_SIMPLE SHELL_PROCESS)
    lv_port_target_includes_and_link(shell)

    # Foreground app: LVGL headless, flush goes to shared memfd + IPC (no SDL window in this process).
    add_executable(app_runtime
        ${PROJECT_SOURCE_DIR}/main/src/main.c
        ${PROJECT_SOURCE_DIR}/main/src/mouse_cursor_icon.c
        ${PROJECT_SOURCE_DIR}/shell_app_runtime/app/app_runtime.c
        ${PROJECT_SOURCE_DIR}/shell_app_runtime/ipc.c
    )
    target_compile_definitions(app_runtime PRIVATE LV_CONF_INCLUDE_SIMPLE APP_PROCESS APP_VARIANT=0
        APP_LV_DISPLAY_BUFFER_MODE=${APP_LV_DISPLAY_BUFFER_MODE} APP_PARTIAL_BUF_LINES=${APP_PARTIAL_BUF_LINES})
    lv_port_target_includes_and_link(app_runtime)

    add_executable(app_runtime_b
        ${PROJECT_SOURCE_DIR}/main/src/main.c
        ${PROJECT_SOURCE_DIR}/main/src/mouse_cursor_icon.c
        ${PROJECT_SOURCE_DIR}/shell_app_runtime/app/app_runtime.c
        ${PROJECT_SOURCE_DIR}/shell_app_runtime/ipc.c
    )
    target_compile_definitions(app_runtime_b PRIVATE LV_CONF_INCLUDE_SIMPLE APP_PROCESS APP_VARIANT=1
        APP_LV_DISPLAY_BUFFER_MODE=${APP_LV_DISPLAY_BUFFER_MODE} APP_PARTIAL_BUF_LINES=${APP_PARTIAL_BUF_LINES})
    lv_port_target_includes_and_link(app_runtime_b)

    add_executable(app_runtime_c
        ${PROJECT_SOURCE_DIR}/main/src/main.c
        ${PROJECT_SOURCE_DIR}/main/src/mouse_cursor_icon.c
        ${PROJECT_SOURCE_DIR}/shell_app_runtime/app/app_runtime.c
        ${PROJECT_SOURCE_DIR}/shell_app_runtime/ipc.c
    )
    target_compile_definitions(app_runtime_c PRIVATE LV_CONF_INCLUDE_SIMPLE APP_PROCESS APP_VARIANT=2
        APP_LV_DISPLAY_BUFFER_MODE=${APP_LV_DISPLAY_BUFFER_MODE} APP_PARTIAL_BUF_LINES=${APP_PARTIAL_BUF_LINES})
    lv_port_target_includes_and_link(app_runtime_c)

    add_executable(shell_sdl_smoke ${PROJECT_SOURCE_DIR}/tests/shell_sdl_smoke.c)
    target_compile_definitions(shell_sdl_smoke PRIVATE LV_CONF_INCLUDE_SIMPLE)
    lv_port_target_includes_and_link(shell_sdl_smoke)
endif()

# Only link freertos_config if the FreeRTOS directory exists
if(USE_FREERTOS)
    target_link_libraries(main freertos_config)
endif()

# Custom target to run the executable
add_custom_target(run COMMAND ${EXECUTABLE_OUTPUT_PATH}/shell DEPENDS shell app_runtime app_runtime_b app_runtime_c)

# Conditionally include and link SDL2_image if LV_USE_DRAW_SDL is enabled
if(LV_USE_DRAW_SDL)
    set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_SOURCE_DIR}/cmake")
    find_package(SDL2_image REQUIRED)
    target_include_directories(lvgl PUBLIC ${SDL2_IMAGE_INCLUDE_DIRS})
    target_link_libraries(main ${SDL2_IMAGE_LIBRARIES})
    if(TARGET app_runtime)
        target_link_libraries(app_runtime ${SDL2_IMAGE_LIBRARIES})
    endif()
    if(TARGET app_runtime_b)
        target_link_libraries(app_runtime_b ${SDL2_IMAGE_LIBRARIES})
    endif()
    if(TARGET app_runtime_c)
        target_link_libraries(app_runtime_c ${SDL2_IMAGE_LIBRARIES})
    endif()
endif()

# Conditionally include and link libpng if LV_USE_LIBPNG is enabled
if(LV_USE_LIBPNG)
    find_package(PNG REQUIRED)
    target_include_directories(lvgl PUBLIC ${PNG_INCLUDE_DIRS})
    target_link_libraries(main ${PNG_LIBRARIES})
    if(TARGET app_runtime)
        target_link_libraries(app_runtime ${PNG_LIBRARIES})
    endif()
    if(TARGET app_runtime_b)
        target_link_libraries(app_runtime_b ${PNG_LIBRARIES})
    endif()
    if(TARGET app_runtime_c)
        target_link_libraries(app_runtime_c ${PNG_LIBRARIES})
    endif()
endif()

# Conditionally include and link libjpeg-turbo if LV_USE_LIBJPEG_TURBO is enabled
if(LV_USE_LIBJPEG_TURBO)
    find_package(JPEG REQUIRED)
    target_include_directories(lvgl PUBLIC ${JPEG_INCLUDE_DIRS})
    target_link_libraries(main ${JPEG_LIBRARIES})
    if(TARGET app_runtime)
        target_link_libraries(app_runtime ${JPEG_LIBRARIES})
    endif()
    if(TARGET app_runtime_b)
        target_link_libraries(app_runtime_b ${JPEG_LIBRARIES})
    endif()
    if(TARGET app_runtime_c)
        target_link_libraries(app_runtime_c ${JPEG_LIBRARIES})
    endif()
endif()

# Conditionally include and link FFmpeg libraries if LV_USE_FFMPEG is enabled
if(LV_USE_FFMPEG)
    target_link_libraries(main avformat avcodec avutil swscale z)
    if(TARGET app_runtime)
        target_link_libraries(app_runtime avformat avcodec avutil swscale z)
    endif()
    if(TARGET app_runtime_b)
        target_link_libraries(app_runtime_b avformat avcodec avutil swscale z)
    endif()
    if(TARGET app_runtime_c)
        target_link_libraries(app_runtime_c avformat avcodec avutil swscale z)
    endif()
endif()

# Conditionally include and link FreeType if LV_USE_FREETYPE is enabled
if(LV_USE_FREETYPE)
    find_package(Freetype REQUIRED)
    target_include_directories(lvgl PUBLIC ${FREETYPE_INCLUDE_DIRS})
    target_link_libraries(main ${FREETYPE_LIBRARIES})
    if(TARGET app_runtime)
        target_link_libraries(app_runtime ${FREETYPE_LIBRARIES})
    endif()
    if(TARGET app_runtime_b)
        target_link_libraries(app_runtime_b ${FREETYPE_LIBRARIES})
    endif()
    if(TARGET app_runtime_c)
        target_link_libraries(app_runtime_c ${FREETYPE_LIBRARIES})
    endif()
endif()

# Apply additional compile options if the build type is Debug
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    message(STATUS "Debug mode enabled")

    target_compile_options(lvgl PRIVATE
        -pedantic-errors
        -Wall
        -Wclobbered
        -Wdeprecated
        -Wdouble-promotion
        -Wempty-body
        -Wextra
        -Wformat-security
        -Wmaybe-uninitialized
        # -Wmissing-prototypes
        -Wpointer-arith
        -Wmultichar
        -Wno-pedantic # ignored for now, we convert functions to pointers for properties table.
        -Wreturn-type
        -Wshadow
        -Wshift-negative-value
        -Wsizeof-pointer-memaccess
        -Wtype-limits
        -Wundef
        -Wuninitialized
        -Wunreachable-code
        -Wfloat-conversion
        -Wstrict-aliasing
    )

if (ASAN)
    message(STATUS "AddressSanitizer enabled")

    # Add AddressSanitizer flags
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=address -fno-omit-frame-pointer")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=address -fno-omit-frame-pointer")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=address")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -fsanitize=address")
else()
    message(STATUS "AddressSanitizer disabled")
endif()
endif()
