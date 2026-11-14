include(FetchContent)

# miniz: zlib-compatible inflate and crc32 for the .hxf data chunk. Single C file; MIT.
FetchContent_Declare(miniz
    URL https://github.com/richgel999/miniz/releases/download/3.0.2/miniz-3.0.2.zip
    URL_HASH SHA256=ada38db0b703a56d3dd6d57bf84a9c5d664921d870d8fea4db153979fb5332c5
)
FetchContent_MakeAvailable(miniz)
add_library(hxed_miniz STATIC "${miniz_SOURCE_DIR}/miniz.c")
target_include_directories(hxed_miniz PUBLIC "${miniz_SOURCE_DIR}")
# Only inflate/deflate/crc32 are used; the zip archive API is not needed.
# MINIZ_NO_ZLIB_COMPATIBLE_NAMES: otherwise miniz.h #defines crc32, compress, ... and renames our own functions.
target_compile_definitions(hxed_miniz PUBLIC MINIZ_NO_ARCHIVE_APIS MINIZ_NO_TIME MINIZ_NO_ZLIB_COMPATIBLE_NAMES)

# zlib (zlib licence) for packing .hxf files: Line 6's data chunk is exactly zlib level 9 output, which miniz's
# deflate doesn't reproduce. Only the deflate side is built. Z_PREFIX renames its symbols (z_deflate, ...) so they
# can't clash with another zlib copy inside the engine's libraries.
FetchContent_Declare(zlib_src
    URL https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz
    URL_HASH SHA256=9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23
    SOURCE_SUBDIR no-cmake  # no CMakeLists there, so only the sources are fetched (zlib's own build isn't used)
)
FetchContent_MakeAvailable(zlib_src)
add_library(hxed_zlib STATIC
    "${zlib_src_SOURCE_DIR}/adler32.c"
    "${zlib_src_SOURCE_DIR}/compress.c"
    "${zlib_src_SOURCE_DIR}/crc32.c"
    "${zlib_src_SOURCE_DIR}/deflate.c"
    "${zlib_src_SOURCE_DIR}/trees.c"
    "${zlib_src_SOURCE_DIR}/zutil.c"
)
target_include_directories(hxed_zlib PUBLIC "${zlib_src_SOURCE_DIR}")
target_compile_definitions(hxed_zlib PUBLIC Z_PREFIX)

add_library(hxed_core STATIC
    appdata.cpp
    buildtag.cpp
    checksums.cpp
    device/devicelink.cpp
    device/editorclient.cpp
    device/fakeunit.cpp
    device/link.cpp
    device/updater.cpp
    device/usbio.cpp
    deflate.cpp
    dspbuild.cpp
    dspmeta.cpp
    firmware.cpp
    globalsettings.cpp
    graphics.cpp
    hlx.cpp
    hxf.cpp
    hxfwrite.cpp
    ir.cpp
    iff.cpp
    json.cpp
    http.cpp
    m4app.cpp
    m4build.cpp
    midi.cpp
    modbuild.cpp
    modeltables.cpp
    modpkg.cpp
    modspec.cpp
    modstore.cpp
    msgpack.cpp
    nam.cpp
    presets.cpp
    presetwriter.cpp
    text.cpp
    unitprofile.cpp
)
target_include_directories(hxed_core PUBLIC "${PROJECT_SOURCE_DIR}")

# The build tag's commit and dirty flag (MODTAG.md, core/modbuild.h): this source's git commit, read at configure
# time when it is a checkout (re-run CMake after committing); else 0000000, dirty.
set(OSHEXED_COMMIT "0000000")
set(OSHEXED_DIRTY 1)
find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --short=7 HEAD WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
                    OUTPUT_VARIABLE _oshexed_commit RESULT_VARIABLE _oshexed_git OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(_oshexed_git EQUAL 0)
        set(OSHEXED_COMMIT "${_oshexed_commit}")
        execute_process(COMMAND "${GIT_EXECUTABLE}" diff --quiet HEAD WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
                        RESULT_VARIABLE _oshexed_diff ERROR_QUIET)
        if(_oshexed_diff EQUAL 0)
            set(OSHEXED_DIRTY 0)
        endif()
    endif()
endif()
set_source_files_properties(modbuild.cpp PROPERTIES
    COMPILE_DEFINITIONS "OSHEXED_COMMIT=\"${OSHEXED_COMMIT}\";OSHEXED_DIRTY=${OSHEXED_DIRTY}")
find_package(Threads REQUIRED)
target_link_libraries(hxed_core PRIVATE hxed_miniz hxed_zlib PUBLIC Threads::Threads)
# USB on Windows goes through the OS's WinUSB driver (see core/device/usbio.h), MIDI through WinMM (core/midi.h);
# all ship with Windows.
if(WIN32)
    target_link_libraries(hxed_core PUBLIC setupapi winusb winmm winhttp)
endif()
if(MSVC)
    target_compile_options(hxed_core PRIVATE /W4)
else()
    target_compile_options(hxed_core PRIVATE -Wall -Wextra -Wpedantic)
endif()
