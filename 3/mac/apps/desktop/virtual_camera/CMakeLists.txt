# The Frame Server DLL deliberately has no Qt or GStreamer dependency.
qt_add_library(vtr-pon-virtual-camera SHARED media_source.cpp protocol.h exports.def)
set_target_properties(vtr-pon-virtual-camera PROPERTIES
    AUTOMOC OFF AUTOUIC OFF
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
target_compile_definitions(vtr-pon-virtual-camera PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(vtr-pon-virtual-camera PRIVATE mfplat mfuuid mf mfsensorgroup ole32 shell32 gdi32 user32 uuid)

qt_add_library(vtrpon-virtual-camera-output STATIC output.cpp output.h protocol.h)
target_compile_definitions(vtrpon-virtual-camera-output PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(vtrpon-virtual-camera-output PRIVATE Qt6::Core d3d11 mfplat mfuuid ole32 shell32 advapi32 uuid)
if(MSVC)
    target_compile_options(vtr-pon-virtual-camera PRIVATE /utf-8)
    target_compile_options(vtrpon-virtual-camera-output PRIVATE /utf-8)
endif()

if(BUILD_TESTING)
    # Short-lived validation client, never required for camera operation.
    add_executable(virtual-camera-smoke smoke.cpp protocol.h)
    set_target_properties(virtual-camera-smoke PROPERTIES AUTOMOC OFF AUTOUIC OFF
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    target_compile_definitions(virtual-camera-smoke PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(virtual-camera-smoke PRIVATE mfplat mfuuid mfreadwrite mf ole32 shell32 gdi32 user32 uuid)
    add_dependencies(virtual-camera-smoke vtr-pon-virtual-camera)
    add_test(NAME virtual-camera-smoke COMMAND virtual-camera-smoke "$<TARGET_FILE:vtr-pon-virtual-camera>")
    set_tests_properties(virtual-camera-smoke PROPERTIES TIMEOUT 30)
    qt_add_executable(virtual-camera-gpu-smoke gpu_smoke.cpp)
    set_target_properties(virtual-camera-gpu-smoke PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    target_compile_definitions(virtual-camera-gpu-smoke PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(virtual-camera-gpu-smoke PRIVATE Qt6::Core d3d11 mfplat mfuuid ole32 shell32 advapi32 uuid)
    add_test(NAME virtual-camera-gpu-smoke COMMAND virtual-camera-gpu-smoke)
    set_tests_properties(virtual-camera-gpu-smoke PROPERTIES TIMEOUT 15 RUN_SERIAL TRUE)
    qt_add_executable(virtual-camera-lifecycle-test lifecycle_test.cpp test_mapping.h)
    set_target_properties(virtual-camera-lifecycle-test PROPERTIES
        AUTOMOC OFF AUTOUIC OFF
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    target_compile_definitions(virtual-camera-lifecycle-test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(virtual-camera-lifecycle-test PRIVATE
        mfplat mfuuid mf mfsensorgroup ole32 shell32 gdi32 user32 uuid)
    if(MSVC)
        target_compile_options(virtual-camera-lifecycle-test PRIVATE /utf-8)
    endif()
    add_test(NAME virtual-camera-lifecycle COMMAND virtual-camera-lifecycle-test)
    set_tests_properties(virtual-camera-lifecycle PROPERTIES TIMEOUT 20)
endif()
