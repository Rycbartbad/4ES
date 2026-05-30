# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# -----------------------------------------------------------------------
# Workaround: GCC ICE (Segmentation Fault) in esp_lcd_panel_rgb.c
#             rgb_panel_draw_bitmap — ESP-IDF v5.2.x xtensa-gcc at -O2.
#
# MUST be placed BEFORE project(): IDF's project() hook creates component
# targets and applies COMPILE_OPTIONS to them at that moment.  Any flag
# appended after project() is ignored by already-created targets.
#
# -fno-tree-loop-optimize disables the one crashing GCC pass; all other
# -O2 optimisations stay active (negligible performance impact).
# -----------------------------------------------------------------------
idf_build_set_property(COMPILE_OPTIONS "-fno-tree-loop-optimize" APPEND)

project(Esp lego)
