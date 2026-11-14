add_executable(core_tests
    main.cpp
    test_buildtag.cpp
    test_bytes.cpp
    test_checksums.cpp
    test_device.cpp
    test_dspmeta.cpp
    test_editorclient.cpp
    test_graphics.cpp
    test_hxf.cpp
    test_hxfwrite.cpp
    test_iff.cpp
    test_dspbuild.cpp
    test_m4build.cpp
    test_ir.cpp
    test_json.cpp
    test_midi.cpp
    test_modpkg.cpp
    test_modeltables.cpp
    test_nam.cpp
    test_presets.cpp
    test_unitprofile.cpp
)
# hxed_miniz for compressing the synthetic .hxf in test_hxf.cpp.
target_link_libraries(core_tests PRIVATE hxed_core hxed_miniz)
target_include_directories(core_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
add_test(NAME core_tests COMMAND core_tests)
