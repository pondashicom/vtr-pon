cmake_minimum_required(VERSION 3.25)
project(vtr_pon_vst3_addon VERSION 0.3.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if((NOT WIN32 AND NOT APPLE) OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "This add-on supports 64-bit Windows and macOS.")
endif()
if(APPLE)
  enable_language(OBJCXX)
endif()
find_package(Qt6 6.8 REQUIRED COMPONENTS Widgets)
qt_standard_project_setup(REQUIRES 6.8)
set(VTRPON_VST3_SDK_DIR "" CACHE PATH "VST 3 SDK 3.8.0 or newer")
if(NOT EXISTS "${VTRPON_VST3_SDK_DIR}/CMakeLists.txt")
  message(FATAL_ERROR "Set VTRPON_VST3_SDK_DIR to the external VST 3 SDK.")
endif()
file(READ "${VTRPON_VST3_SDK_DIR}/CMakeLists.txt" _sdk_cmake)
string(REGEX MATCH "project\\([ \t\r\n]*vstsdk[ \t\r\n]+VERSION[ \t\r\n]+([0-9]+\\.[0-9]+\\.[0-9]+)"
  _version_match "${_sdk_cmake}")
if(NOT CMAKE_MATCH_1 OR CMAKE_MATCH_1 VERSION_LESS "3.8.0")
  message(FATAL_ERROR "VST 3 SDK 3.8.0 or newer is required.")
endif()
set(SMTG_ENABLE_VST3_PLUGIN_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SMTG_ENABLE_VST3_HOSTING_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SMTG_ENABLE_VSTGUI_SUPPORT OFF CACHE BOOL "" FORCE)
add_subdirectory("${VTRPON_VST3_SDK_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/sdk" EXCLUDE_FROM_ALL)
if(APPLE)
  set(vtrpon_module "${VTRPON_VST3_SDK_DIR}/public.sdk/source/vst/hosting/module_mac.mm")
  set_source_files_properties("${vtrpon_module}" src/vst3_addon_main.cpp
    PROPERTIES LANGUAGE OBJCXX COMPILE_OPTIONS "-fobjc-arc")
else()
  set(vtrpon_module "${VTRPON_VST3_SDK_DIR}/public.sdk/source/vst/hosting/module_win32.cpp")
endif()
qt_add_executable(vtr-pon-vst3-addon
  src/vst3_addon_main.cpp
  "${VTRPON_VST3_SDK_DIR}/public.sdk/source/vst/hosting/plugprovider.cpp"
  "${vtrpon_module}"
  "${VTRPON_VST3_SDK_DIR}/public.sdk/source/common/memorystream.cpp")
target_include_directories(vtr-pon-vst3-addon PRIVATE include "${VTRPON_VST3_SDK_DIR}")
target_link_libraries(vtr-pon-vst3-addon PRIVATE Qt6::Widgets sdk_hosting)
if(APPLE)
  target_link_libraries(vtr-pon-vst3-addon PRIVATE "-framework Cocoa")
  set_target_properties(vtr-pon-vst3-addon PROPERTIES MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/src/Info.plist.in")
else()
  target_link_libraries(vtr-pon-vst3-addon PRIVATE ole32 user32)
endif()
target_compile_definitions(vtr-pon-vst3-addon PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
if(MSVC)
  target_compile_options(vtr-pon-vst3-addon PRIVATE /utf-8)
endif()
set_target_properties(vtr-pon-vst3-addon PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/audio-addons")
if(APPLE)
  set(vtrpon_notice_dir "$<TARGET_BUNDLE_DIR:vtr-pon-vst3-addon>/Contents/Resources")
else()
  set(vtrpon_notice_dir "$<TARGET_FILE_DIR:vtr-pon-vst3-addon>")
endif()
add_custom_command(TARGET vtr-pon-vst3-addon POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${vtrpon_notice_dir}"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
    "${VTRPON_VST3_SDK_DIR}/LICENSE.txt"
    "${vtrpon_notice_dir}/VST3-SDK-LICENSE.txt"
  VERBATIM)
if(APPLE)
  add_custom_command(TARGET vtr-pon-vst3-addon POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E rm -f "$<TARGET_FILE_DIR:vtr-pon-vst3-addon>/VST3-SDK-LICENSE.txt"
    COMMAND /usr/bin/codesign --force --sign - --timestamp=none "$<TARGET_BUNDLE_DIR:vtr-pon-vst3-addon>"
    VERBATIM)
endif()
if(WIN32)
add_custom_command(TARGET vtr-pon-vst3-addon POST_BUILD
  COMMAND "$<TARGET_FILE:Qt6::windeployqt>" --release --no-translations
    --skip-plugin-types generic,imageformats,networkinformation,tls
    --no-compiler-runtime
    "$<TARGET_FILE:vtr-pon-vst3-addon>"
  VERBATIM)
endif()
if(APPLE)
  install(DIRECTORY "$<TARGET_BUNDLE_DIR:vtr-pon-vst3-addon>" DESTINATION audio-addons)
else()
  install(DIRECTORY "${CMAKE_BINARY_DIR}/audio-addons/" DESTINATION audio-addons)
endif()

# Never install developer fault-injection plug-ins into a user's VST3 folders.
option(VTRPON_VST3_BUILD_TEST_PLUGIN "Build macOS fault-injection VST3 bundle" OFF)
if(APPLE AND VTRPON_VST3_BUILD_TEST_PLUGIN)
  set(SMTG_RUN_VST_VALIDATOR OFF)
  set(SMTG_CREATE_MODULE_INFO OFF)
  set(SMTG_CREATE_PLUGIN_LINK OFF)
  smtg_add_vst3plugin(vtrpon-mac-vst3-fixture tests/mac_test_plugin.mm
    "${VTRPON_VST3_SDK_DIR}/public.sdk/source/main/macmain.cpp"
    "${VTRPON_VST3_SDK_DIR}/public.sdk/source/vst/vstsinglecomponenteffect.cpp")
  target_include_directories(vtrpon-mac-vst3-fixture PRIVATE "${VTRPON_VST3_SDK_DIR}")
  target_compile_options(vtrpon-mac-vst3-fixture PRIVATE "$<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>")
  target_link_libraries(vtrpon-mac-vst3-fixture PRIVATE sdk "-framework Cocoa")
  smtg_target_set_bundle(vtrpon-mac-vst3-fixture
    BUNDLE_IDENTIFIER "com.pondashi.vtrpon.vst3-test" COMPANY_NAME "VTR-PON")
endif()
