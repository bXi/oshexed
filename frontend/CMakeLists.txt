# Engine checkout. Pass -DLUMINOVEAU_DIR=<path> (e.g. E:/luminoveau), set the LUMINOVEAU_DIR
# environment variable, or put it next to this project.
set(LUMINOVEAU_DIR "$ENV{LUMINOVEAU_DIR}" CACHE PATH "Path to the Luminoveau engine")
if(NOT EXISTS "${LUMINOVEAU_DIR}/CMakeLists.txt")
    foreach(candidate "${PROJECT_SOURCE_DIR}/luminoveau" "${PROJECT_SOURCE_DIR}/../luminoveau")
        if(EXISTS "${candidate}/CMakeLists.txt")
            set(LUMINOVEAU_DIR "${candidate}" CACHE PATH "Path to the Luminoveau engine" FORCE)
            break()
        endif()
    endforeach()
endif()
if(NOT EXISTS "${LUMINOVEAU_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "Luminoveau not found; pass -DLUMINOVEAU_DIR=<path> or set HXED_BUILD_APP=OFF")
endif()

# Same engine wiring as lumiSudoku: SDL3 callback main loop, RmlUi for everything. No ImGui: the developer views
# are RmlUi tabs (Monitor).
set(LUMINOVEAU_USE_CALLBACKS ON)
set(LUMINOVEAU_BUILD_RMLUI ON)
set(LUMINOVEAU_BUILD_IMGUI OFF)

# The engine turns Steam on by itself when the Steamworks SDK sits in its tree, so point it at a
# folder that doesn't exist.
set(HXED_NO_STEAM_SDK "${CMAKE_BINARY_DIR}/no-steam-sdk")
set(LUMINOVEAU_STEAM_SDK_DIR "${HXED_NO_STEAM_SDK}" CACHE PATH "Steamworks SDK root" FORCE)

add_subdirectory("${LUMINOVEAU_DIR}" "${CMAKE_BINARY_DIR}/luminoveau")

# RmlUi lays out the whole document after every data change, so live updates (slider drags, junction drags)
# lag when it is built unoptimised. Optimise it in Debug too; our own code keeps the chosen build type.
foreach(rml_target rmlui_core rmlui_debugger)
    if(TARGET ${rml_target})
        target_compile_options(${rml_target} PRIVATE $<$<AND:$<CONFIG:Debug>,$<CXX_COMPILER_ID:GNU,Clang>>:-O2>)
    endif()
endforeach()

# Native HiDPI: window sizes and mouse input in device pixels, matching the swapchain.
target_compile_definitions(luminoveau PUBLIC LUMI_USE_PHYSICAL_PIXELS)

add_executable(oshexed
    main.cpp
    app.cpp
    baseview.cpp
    contextmenu.cpp
    editorview.cpp
    embeddedassets.cpp
    firmwaretextures.cpp
    flashview.cpp
    globaleqview.cpp
    globalsettingsview.cpp
    irview.cpp
    modsview.cpp
    monitorview.cpp
    settings.cpp
    shellview.cpp
)
target_link_libraries(oshexed PRIVATE hxed_core luminoveau)
target_include_directories(oshexed PRIVATE "${PROJECT_SOURCE_DIR}" "${LUMINOVEAU_DIR}")
# Command-line mode (`oshexed --info FILE`, cli/hxed_dump.h).
if(TARGET hxed_cli)
    target_link_libraries(oshexed PRIVATE hxed_cli)
    target_compile_definitions(oshexed PRIVATE HXED_HAS_CLI)
endif()

if(WIN32)
    # The exe's icon (oshexed.rc -> oshexed.ico).
    enable_language(RC)
    target_sources(oshexed PRIVATE oshexed.rc)
    # A console program (not WIN32_EXECUTABLE): terminals wait for it, so command-line output and exit codes come
    # in order. Started from Explorer, main.cpp lets go of the console Windows made for it (see there).
    if(MINGW)
        target_link_libraries(oshexed PRIVATE -static-libgcc -static-libstdc++)
    endif()
endif()

# Lucide icon font (ISC licence) for the tab rail, fetched once at configure time, pinned by hash.
# Glyph codepoints in ui/*.rml belong to this version: check lucide.css when bumping it.
set(HXED_LUCIDE_VERSION 1.56.0)
set(HXED_LUCIDE_DIR "${CMAKE_BINARY_DIR}/third_party/lucide-${HXED_LUCIDE_VERSION}")
set(HXED_LUCIDE_URL "https://cdn.jsdelivr.net/npm/lucide-static@${HXED_LUCIDE_VERSION}")
if(NOT EXISTS "${HXED_LUCIDE_DIR}/lucide.ttf")
    file(DOWNLOAD "${HXED_LUCIDE_URL}/font/lucide.ttf" "${HXED_LUCIDE_DIR}/lucide.ttf"
        EXPECTED_HASH SHA256=6168bee50e05f951afd4696e687a87206aebdab0ed4d76bf926c8bed66c2e3f5 TLS_VERIFY ON)
endif()
if(NOT EXISTS "${HXED_LUCIDE_DIR}/LICENSE")
    file(DOWNLOAD "${HXED_LUCIDE_URL}/LICENSE" "${HXED_LUCIDE_DIR}/LICENSE"
        EXPECTED_HASH SHA256=b495047bd93a9b06913511076f504daba17d5bbeb3e0650f3bb53a4220329c57 TLS_VERIFY ON)
endif()

# What the app reads at runtime (through PhysFS, RmlUi's files too):
#   ui/                      our .rml/.rcss
#   assets/shaders/          the engine's RmlUi shaders (it writes them to assets/ at configure time)
#   assets/fonts/lucide.ttf  the icon font
# By default they are zipped into the executable and mounted from memory at startup (App::init, before
# InitWindow), so oshexed.exe stands alone; as lumiSudoku does it. The embedding uses the assembler's .incbin
# (embeddedassets.cpp): GCC and Clang, not MSVC. Otherwise, or with the option off, they're copied next to it.
option(OSHEXED_EMBED_ASSETS "Pack ui/ and assets/ into the executable" ON)
file(MAKE_DIRECTORY "${PROJECT_SOURCE_DIR}/assets")
if(OSHEXED_EMBED_ASSETS AND NOT MSVC AND NOT APPLE)
    file(GLOB_RECURSE HXED_PACKED_FILES CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/ui/*" "${PROJECT_SOURCE_DIR}/assets/*")
    set(HXED_PACK_DIR "${CMAKE_CURRENT_BINARY_DIR}/pack")
    set(HXED_ASSETS_ZIP "${CMAKE_CURRENT_BINARY_DIR}/oshexed_assets.zip")
    # Staged in one folder first: the zip's paths are what the code names ("ui/main.rml", "assets/fonts/...").
    add_custom_command(OUTPUT "${HXED_ASSETS_ZIP}"
        COMMAND ${CMAKE_COMMAND} -E rm -rf "${HXED_PACK_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${PROJECT_SOURCE_DIR}/ui" "${HXED_PACK_DIR}/ui"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${PROJECT_SOURCE_DIR}/assets" "${HXED_PACK_DIR}/assets"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${HXED_PACK_DIR}/assets/fonts"
        COMMAND ${CMAKE_COMMAND} -E copy "${HXED_LUCIDE_DIR}/lucide.ttf" "${HXED_PACK_DIR}/assets/fonts/lucide.ttf"
        COMMAND ${CMAKE_COMMAND} -E rm -f "${HXED_ASSETS_ZIP}"
        # Zipped from inside pack/ (made above; the command itself runs from the build folder, as pack/ is
        # removed and remade on every run).
        COMMAND ${CMAKE_COMMAND} -E chdir "${HXED_PACK_DIR}" ${CMAKE_COMMAND} -E tar cf "${HXED_ASSETS_ZIP}" --format=zip ui assets
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
        DEPENDS ${HXED_PACKED_FILES} "${HXED_LUCIDE_DIR}/lucide.ttf"
        COMMENT "Packing ui/ and assets/ into the executable"
        VERBATIM)
    add_custom_target(oshexed_assets DEPENDS "${HXED_ASSETS_ZIP}")
    add_dependencies(oshexed oshexed_assets)
    # .incbin reads the zip while assembling, so a changed zip must rebuild this object.
    set_source_files_properties(embeddedassets.cpp PROPERTIES OBJECT_DEPENDS "${HXED_ASSETS_ZIP}")
    target_compile_definitions(oshexed PRIVATE OSHEXED_ASSETS_ZIP="${HXED_ASSETS_ZIP}")
else()
    add_custom_target(oshexed_assets ALL
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${PROJECT_SOURCE_DIR}/ui" "$<TARGET_FILE_DIR:oshexed>/ui"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${PROJECT_SOURCE_DIR}/assets" "$<TARGET_FILE_DIR:oshexed>/assets"
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:oshexed>/assets/fonts"
        COMMAND ${CMAKE_COMMAND} -E copy "${HXED_LUCIDE_DIR}/lucide.ttf" "$<TARGET_FILE_DIR:oshexed>/assets/fonts/lucide.ttf")
    add_dependencies(oshexed oshexed_assets)
endif()

# The licences ship next to the executable either way (the icon font's must travel with the font).
add_custom_target(oshexed_licences ALL
    COMMAND ${CMAKE_COMMAND} -E copy "${PROJECT_SOURCE_DIR}/LICENSE" "$<TARGET_FILE_DIR:oshexed>/LICENSE.txt"
    COMMAND ${CMAKE_COMMAND} -E copy "${HXED_LUCIDE_DIR}/LICENSE" "$<TARGET_FILE_DIR:oshexed>/LICENSE-lucide.txt")
add_dependencies(oshexed oshexed_licences)
