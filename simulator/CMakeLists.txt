cmake_minimum_required(VERSION 3.16)
project(smart_dashboard_sim C)

set(CMAKE_C_STANDARD 11)

# ── SDL2 ──────────────────────────────────────────────────────────────────────
# MSYS2 UCRT64: pacman -S mingw-w64-ucrt-x86_64-SDL2
find_package(SDL2 REQUIRED)

# ── LVGL 9.4.0 ────────────────────────────────────────────────────────────────
include(FetchContent)
FetchContent_Declare(
    lvgl
    GIT_REPOSITORY https://github.com/lvgl/lvgl.git
    GIT_TAG        v9.4.0
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(lvgl)

# ── cJSON ─────────────────────────────────────────────────────────────────────
# cJSON declares cmake_minimum_required(2.8.5); CMake 4.x rejects < 3.5.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
set(ENABLE_CJSON_TEST OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    cjson
    GIT_REPOSITORY https://github.com/DaveGamble/cJSON.git
    GIT_TAG        v1.7.18
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(cjson)

# ── libcurl ────────────────────────────────────────────────────────────────────
# MSYS2 UCRT64: pacman -S mingw-w64-ucrt-x86_64-curl
find_package(CURL REQUIRED)

# ── LVGL configuration ────────────────────────────────────────────────────────
target_compile_definitions(lvgl PUBLIC
    LV_CONF_INCLUDE_SIMPLE=1
    LV_USE_SDL=1
)
target_include_directories(lvgl PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Strip ARM-only Helium/MVE assembly — GAS on x86 cannot assemble it
get_target_property(_lvgl_srcs lvgl SOURCES)
list(FILTER _lvgl_srcs EXCLUDE REGEX ".*[Hh]elium.*\\.S$")
set_target_properties(lvgl PROPERTIES SOURCES "${_lvgl_srcs}")

# ── Executable ────────────────────────────────────────────────────────────────
add_executable(smart_dashboard_sim
    main.c
    ../src/tab_registry.c
    ../src/shell.c
    ../src/dashboard_config.c
    ../src/clock_service.c
    ../src/tab_home.c
    ../src/home_summary.c
    ../src/home_events_panel.c
    ../src/home_calendar.c
    ../src/home_ha_bridge.c
    ../src/dashboard_lv_utils.c
    ../src/tab_entertainment.c
    ../src/now_playing.c
    ../src/media_art.c
    ../src/dashboard_icons.c
    ../src/ha_ws_client.c
    ../src/ha_credentials.c
    ../src/display_sleep.c
    ../src/sheet.c
    ../src/control_sheet.c
    ../src/date_picker.c
    ../src/time_picker.c
    ../src/keyboard.c
    ../src/calendar_service.c
    ../src/event_edit.c
    ../src/config_sheet.c
    ../src/platform/http_client_sim.c
    ../src/google_calendar.c
    ../src/text_sub.c
    ../assets/fonts/inter_60_600.c
)

target_compile_definitions(smart_dashboard_sim PRIVATE
    SIMULATOR=1     # also defined in lv_conf.h; duplicate is harmless
)

target_include_directories(smart_dashboard_sim PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}     # lv_conf.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../src   # dashboard_log.h and shared modules
    ${lvgl_SOURCE_DIR}              # lvgl.h
    ${cjson_SOURCE_DIR}             # cJSON.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/platform
)

target_link_libraries(smart_dashboard_sim PRIVATE
    SDL2::SDL2main  # must precede SDL2::SDL2 on Windows
    lvgl
    SDL2::SDL2
    cjson
    stdc++          # ThorVG (inside lvgl) is compiled as C++
    ws2_32          # WinSock2 for ha_ws_client (Simulator)
    CURL::libcurl
)

# Console subsystem so printf() works on MinGW
if (MINGW)
    target_link_options(smart_dashboard_sim PRIVATE -mconsole)
endif()
