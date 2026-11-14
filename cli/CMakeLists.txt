# oshexed's command line (`oshexed --info FILE`, cli/hxed_dump.h). Output of the checks matches the Python tools line
# for line, so the firmware project's tools/oracle/compare.py can diff the two. The commands live in a library that the editor's exe runs
# (frontend/main.cpp): one binary for the editor and the command line.
add_library(hxed_cli STATIC hxed_dump.cpp)
target_link_libraries(hxed_cli PUBLIC hxed_core)
target_include_directories(hxed_cli PUBLIC "${PROJECT_SOURCE_DIR}")
