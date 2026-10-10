cmake_minimum_required(VERSION 3.24)

project(VTRPON LANGUAGES CXX)
if(WIN32)
  enable_language(RC)
endif()

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/release/version.json")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/release/version.json" _vtrpon_version_json)
string(JSON VTRPON_APP_VERSION ERROR_VARIABLE _vtrpon_version_error
  GET "${_vtrpon_version_json}" version)
if(_vtrpon_version_error)
  message(FATAL_ERROR "Invalid release/version.json: ${_vtrpon_version_error}")
endif()
string(JSON VTRPON_RELEASE_READY ERROR_VARIABLE _vtrpon_ready_error
  GET "${_vtrpon_version_json}" releaseReady)
if(_vtrpon_ready_error)
  message(FATAL_ERROR "Invalid release/version.json: ${_vtrpon_ready_error}")
endif()
if(NOT VTRPON_APP_VERSION MATCHES
    "^([0-9]+)\\.([0-9]+)\\.([0-9]+)(-(beta|rc)\\.([1-9][0-9]*))?$")
  message(FATAL_ERROR "Unsupported VTR-PON version: ${VTRPON_APP_VERSION}")
endif()
set(_vtrpon_major "${CMAKE_MATCH_1}")
set(_vtrpon_minor "${CMAKE_MATCH_2}")
set(_vtrpon_patch "${CMAKE_MATCH_3}")
set(_vtrpon_phase "${CMAKE_MATCH_5}")
set(_vtrpon_sequence "${CMAKE_MATCH_6}")
foreach(_vtrpon_part IN ITEMS _vtrpon_major _vtrpon_minor _vtrpon_patch)
  if(${_vtrpon_part} MATCHES "^0[0-9]+$" OR ${_vtrpon_part} GREATER 65535)
    message(FATAL_ERROR "Invalid Windows version component in ${VTRPON_APP_VERSION}")
  endif()
endforeach()
if(_vtrpon_phase)
  set(VTRPON_WINDOWS_FILE_FLAGS VS_FF_PRERELEASE)
  if(_vtrpon_sequence GREATER 999)
    message(FATAL_ERROR "Prerelease sequence must be 1..999")
  endif()
  if(_vtrpon_phase STREQUAL "beta")
    math(EXPR _vtrpon_windows_revision "1000 + ${_vtrpon_sequence}")
  else()
    math(EXPR _vtrpon_windows_revision "2000 + ${_vtrpon_sequence}")
  endif()
else()
  set(VTRPON_WINDOWS_FILE_FLAGS 0x0L)
  set(_vtrpon_windows_revision 3000)
endif()
set(VTRPON_WINDOWS_VERSION
  "${_vtrpon_major}.${_vtrpon_minor}.${_vtrpon_patch}.${_vtrpon_windows_revision}")
string(REPLACE "." "," VTRPON_WINDOWS_VERSION_COMMAS "${VTRPON_WINDOWS_VERSION}")
set(VTRPON_INSTALLER_BASENAME "VTR-PON-${VTRPON_APP_VERSION}-windows-x64-setup")
if(DEFINED CACHE{VTRPON_APP_VERSION} AND
    NOT "$CACHE{VTRPON_APP_VERSION}" STREQUAL "${VTRPON_APP_VERSION}")
  message(STATUS "Replacing cached VTRPON_APP_VERSION with release/version.json value")
endif()
set(VTRPON_APP_VERSION "${VTRPON_APP_VERSION}" CACHE INTERNAL
  "VTR-PON version from release/version.json" FORCE)
configure_file(release/version.iss.in
  "${CMAKE_BINARY_DIR}/release/version.iss" @ONLY)
if(WIN32)
  set(VTRPON_WINDOWS_ICON
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons/vtr-pon3/windows/VTR-PON3.ico")
  configure_file(release/version.rc.in
    "${CMAKE_BINARY_DIR}/release/version.rc" @ONLY)
endif()

if(MSVC AND CMAKE_GENERATOR MATCHES "Ninja")
  # CMake 4.2 can decode UTF-8 Japanese /showIncludes output as the system
  # code page, leaving Ninja with a corrupt prefix and no header dependencies.
  # Detect it again without transcoding so header layout changes rebuild every
  # translation unit that allocates the changed class.
  set(_showincludes_dir "${CMAKE_BINARY_DIR}/CMakeFiles/VtrPonShowIncludes")
  file(MAKE_DIRECTORY "${_showincludes_dir}")
  file(WRITE "${_showincludes_dir}/foo.h" "\n")
  file(WRITE "${_showincludes_dir}/main.c" "#include \"foo.h\"\nint main(){}\n")
  execute_process(
    COMMAND "${CMAKE_CXX_COMPILER}" /nologo /showIncludes /c main.c
    WORKING_DIRECTORY "${_showincludes_dir}"
    OUTPUT_VARIABLE _showincludes_output
    ERROR_VARIABLE _showincludes_error
    RESULT_VARIABLE _showincludes_result
    ENCODING NONE)
  if(_showincludes_result EQUAL 0 AND
      _showincludes_output MATCHES
        "(^|\n)([^:\n][^:\n]+:[^:\n]*[^: \n][^: \n]:?[ \t]+)([A-Za-z]:\\\\|\\./|\\.\\\\|/)")
    set(CMAKE_CXX_CL_SHOWINCLUDES_PREFIX "${CMAKE_MATCH_2}")
    set(CMAKE_CL_SHOWINCLUDES_PREFIX "${CMAKE_MATCH_2}")
  else()
    message(FATAL_ERROR
      "Unable to detect the MSVC /showIncludes prefix: ${_showincludes_error}")
  endif()
endif()


set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Qt6 6.8 REQUIRED COMPONENTS Widgets Network Pdf LinguistTools)
qt_standard_project_setup(REQUIRES 6.8)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GSTREAMER REQUIRED IMPORTED_TARGET
  gstreamer-1.0
  gstreamer-base-1.0
  gstreamer-video-1.0
  gstreamer-pbutils-1.0)
pkg_check_modules(GSTREAMER_APP REQUIRED IMPORTED_TARGET
  gstreamer-app-1.0)
pkg_check_modules(GSTREAMER_AUDIO REQUIRED IMPORTED_TARGET
  gstreamer-audio-1.0)

find_path(VTRPON_ZLIB_INCLUDE_DIR zlib.h PATHS ${GSTREAMER_INCLUDE_DIRS} REQUIRED)
find_library(VTRPON_ZLIB_LIBRARY NAMES z zlib PATHS ${GSTREAMER_LIBRARY_DIRS} REQUIRED)
if(WIN32)
  get_filename_component(_vtrpon_zlib_lib_dir "${VTRPON_ZLIB_LIBRARY}" DIRECTORY)
  get_filename_component(_vtrpon_zlib_root "${_vtrpon_zlib_lib_dir}" DIRECTORY)
  find_file(VTRPON_ZLIB_RUNTIME NAMES z-1.dll zlib1.dll
    PATHS "${_vtrpon_zlib_root}/bin" NO_DEFAULT_PATH REQUIRED)
endif()
qt_add_library(vtrpon-addon-package STATIC
  apps/desktop/addon_package.cpp
  apps/desktop/addon_package.h)
target_include_directories(vtrpon-addon-package PUBLIC apps/desktop PRIVATE ${VTRPON_ZLIB_INCLUDE_DIR})
target_link_libraries(vtrpon-addon-package PUBLIC Qt6::Core PRIVATE ${VTRPON_ZLIB_LIBRARY})
if(WIN32)
  target_compile_definitions(vtrpon-addon-package PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_link_libraries(vtrpon-addon-package PRIVATE shell32 advapi32)
endif()
if(MSVC)
  target_compile_options(vtrpon-addon-package PRIVATE /utf-8)
endif()
qt_add_executable(vtr-pon-addon-manager apps/desktop/addon_manager_main.cpp)
target_link_libraries(vtr-pon-addon-manager PRIVATE vtrpon-addon-package)
if(WIN32)
  add_custom_command(TARGET vtr-pon-addon-manager POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
      "${VTRPON_ZLIB_RUNTIME}" "$<TARGET_FILE_DIR:vtr-pon-addon-manager>")
  install(FILES "${VTRPON_ZLIB_RUNTIME}" DESTINATION . COMPONENT Runtime)
endif()
if(MSVC)
  target_compile_options(vtr-pon-addon-manager PRIVATE /utf-8)
endif()
install(TARGETS vtr-pon-addon-manager RUNTIME DESTINATION . COMPONENT Runtime)

qt_add_library(vtrpon-gpu-configuration STATIC
  apps/desktop/gpu_configuration.cpp
  apps/desktop/gpu_configuration.h)
target_include_directories(vtrpon-gpu-configuration PUBLIC apps/desktop)
target_link_libraries(vtrpon-gpu-configuration PUBLIC Qt6::Core PRIVATE PkgConfig::GSTREAMER)
if(WIN32)
  target_link_libraries(vtrpon-gpu-configuration PRIVATE dxgi)
  target_compile_definitions(vtrpon-gpu-configuration PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
if(MSVC)
  target_compile_options(vtrpon-gpu-configuration PRIVATE /utf-8)
endif()

if(WIN32)
  pkg_check_modules(GSTREAMER_D3D11 REQUIRED IMPORTED_TARGET
    gstreamer-d3d11-1.0)
endif()

qt_add_executable(vtr-pon WIN32
  apps/desktop/about_window.cpp
  apps/desktop/about_window.h
  apps/desktop/controller_window_scaling.cpp
  apps/desktop/controller_window_scaling.h
  apps/desktop/application_log.cpp
  apps/desktop/background_io_queue.h
  apps/desktop/async_device_monitor.h
  apps/desktop/application_log.h
  apps/desktop/log_viewer.cpp
  apps/desktop/controller_operations.h
  apps/desktop/audio_output_settings.cpp
  apps/desktop/audio_output_settings.h
  apps/desktop/audio_output_coordinator.cpp
  apps/desktop/audio_output_coordinator.h
  apps/desktop/edit_pfl_output.cpp
  apps/desktop/edit_pfl_output.h
  apps/desktop/edit_pfl_protocol.h
  apps/desktop/edit_pfl_renderer.h
  apps/desktop/bgm_playout_controller.cpp
  apps/desktop/bgm_playout_controller.h
  apps/desktop/audio_fader_widget.cpp
  apps/desktop/audio_fader_widget.h
  apps/desktop/audio_fader_scale.h
  apps/desktop/addon_management_page.cpp
  apps/desktop/addon_management_page.h
  apps/desktop/audio_meter_widget.cpp
  apps/desktop/audio_meter_widget.h
  apps/desktop/atem_addon_switcher.cpp
  apps/desktop/atem_addon_switcher.h
  apps/desktop/controller_shortcuts.cpp
  apps/desktop/controller_shortcuts.h
  apps/desktop/midi_control_input.cpp
  apps/desktop/midi_control_input.h
  apps/desktop/midi_control_settings.h
  apps/desktop/midi_message_queue.h
  apps/desktop/midi_control_settings_page.cpp
  apps/desktop/midi_control_settings_page.h
  apps/desktop/controller_settings_dialog.cpp
  apps/desktop/controller_settings.h
  apps/desktop/controller_settings_io.cpp
  apps/desktop/controller_settings_io.h
  apps/desktop/controller_settings_dialog.h
  apps/desktop/settings_style.cpp
  apps/desktop/settings_style.h
  apps/desktop/document_add_method_dialog.cpp
  apps/desktop/document_add_method_dialog.h
  apps/desktop/document_import_queue.cpp
  apps/desktop/document_import_queue.h
  apps/desktop/document_video_encoder.cpp
  apps/desktop/document_video_encoder.h
  apps/desktop/dual_deck_player.cpp
  apps/desktop/dual_deck_player.h
  apps/desktop/deck_source_session.cpp
  apps/desktop/deck_source_session.h
  apps/desktop/bgm_playback.cpp
  apps/desktop/bgm_playback.h
  apps/desktop/dsk_playback.cpp
  apps/desktop/dsk_playback.h
  apps/desktop/edit_panel.cpp
  apps/desktop/edit_panel.h
  apps/desktop/edit_player.cpp
  apps/desktop/edit_player.h
  apps/desktop/edit_session_controller.cpp
  apps/desktop/edit_session_controller.h
  apps/desktop/hardware_panel_widget.h
  apps/desktop/image_add_dialog.cpp
  apps/desktop/image_add_dialog.h
  apps/desktop/image_add_registration.cpp
  apps/desktop/image_add_registration.h
  apps/desktop/input_control_panel.cpp
  apps/desktop/input_control_panel.h
  apps/desktop/live_audio_source.h
  apps/desktop/macro_runner.cpp
  apps/desktop/macro_runner.h
  apps/desktop/macro_editor_window.cpp
  apps/desktop/macro_editor_window.h
  apps/desktop/main.cpp
  apps/desktop/main_controller.cpp
  apps/desktop/main_controller.h
  apps/desktop/update_catalog.cpp
  apps/desktop/update_catalog.h
  apps/desktop/update_notification.cpp
  apps/desktop/update_notification.h
  apps/desktop/application_shutdown_deadline.cpp
  apps/desktop/application_shutdown_deadline.h
  apps/desktop/media_import_queue.cpp
  apps/desktop/media_import_queue.h
  apps/desktop/media_duration_probe.cpp
  apps/desktop/media_duration_probe.h
  apps/desktop/media_thumbnailer.cpp
  apps/desktop/media_thumbnailer.h
  apps/desktop/network_time_clock.cpp
  apps/desktop/network_time_clock.h
  apps/desktop/system_load_widget.cpp
  apps/desktop/system_load_widget.h
  apps/desktop/playlist_document_io.cpp
  apps/desktop/playlist_document_io.h
  apps/desktop/playlist_item.h
  apps/desktop/playlist_item_display.h
  apps/desktop/playlist_item_playout_state_machine.cpp
  apps/desktop/playlist_item_playout_state_machine.h
  apps/desktop/macro_document.cpp
  apps/desktop/playlist_model.cpp
  apps/desktop/playlist_play_sequence.cpp
  apps/desktop/playlist_media_availability.cpp
  apps/desktop/playlist_model.h
  apps/desktop/playlist_panel.cpp
  apps/desktop/playlist_context_menu.cpp
  apps/desktop/playlist_context_menu.h
  apps/desktop/playlist_input_widgets.cpp
  apps/desktop/playlist_input_widgets.h
  apps/desktop/playlist_item_delegate.cpp
  apps/desktop/playlist_item_delegate.h
  apps/desktop/playlist_item_operations.cpp
  apps/desktop/playlist_item_operations.h
  apps/desktop/playlist_thumbnail_store.cpp
  apps/desktop/playlist_thumbnail_store.h
  apps/desktop/playlist_view_roles.h
  apps/desktop/playlist_panel.h
  apps/desktop/program_audio_controller.cpp
  apps/desktop/program_audio_controller.h
  apps/desktop/program_audio_output.cpp
  apps/desktop/program_audio_output.h
  apps/desktop/audio_delay.cpp
  apps/desktop/audio_effects.cpp
  apps/desktop/audio_effects.h
  apps/desktop/audio_equalizer.cpp
  apps/desktop/program_audio_bridge.cpp
  apps/desktop/program_audio_bridge.h
  apps/desktop/program_audio_meter.cpp
  apps/desktop/program_audio_meter.h
  apps/desktop/program_panel.cpp
  apps/desktop/program_panel.h
  apps/desktop/program_playout_controller.cpp
  apps/desktop/program_playout_controller.h
  apps/desktop/roland_v8hd_midi_switcher.cpp
  apps/desktop/roland_v8hd_midi_switcher.h
  apps/desktop/switcher_control.cpp
  apps/desktop/switcher_control.h
  apps/desktop/switcher_addon_api.h
  apps/desktop/timed_still_playout_clock.cpp
  apps/desktop/timed_still_playout_clock.h
  apps/desktop/range_marker_slider.cpp
  apps/desktop/range_marker_slider.h
  apps/desktop/skins/controller_skin.cpp
  apps/desktop/skins/controller_skin.h
  apps/desktop/language_manager.cpp
  apps/desktop/language_manager.h
  apps/desktop/language_text.cpp
  apps/desktop/language_text.h
  apps/desktop/user_info.cpp
  apps/desktop/user_info.h
  apps/desktop/uvc_audio_mapping.cpp
  apps/desktop/uvc_audio_mapping.h
  apps/desktop/uvc_device_monitor.cpp
  apps/desktop/uvc_device_monitor.h
  apps/desktop/uvc_source_session_manager.cpp
  apps/desktop/uvc_source_session_manager.h
  apps/desktop/view_menu_controller.cpp
  apps/desktop/view_menu_controller.h
  apps/desktop/operator_countdown_timer.h
  apps/desktop/operator_timer_settings_dialog.h
  apps/desktop/controller_scaling.cpp
  apps/desktop/controller_scaling.h)

set(VTRPON_STYLE_RESOURCES
    apps/desktop/settings.qss
    apps/desktop/skins/hardware/controller.qss
    apps/desktop/skins/hardware/controller_black_brushed_metal.svg
    apps/desktop/skins/hardware/controller_brushed_metal.svg
    apps/desktop/skins/hardware/controller_panel_brushed_metal.svg
    apps/desktop/skins/hardware/controller_walnut_trim.svg
    apps/desktop/skins/material-dark/controller.qss
    apps/desktop/skins/material-dark/check.svg
    apps/desktop/skins/material-dark/chevron_down.svg
    apps/desktop/skins/vtr-pon-clasic/controller.qss
    apps/desktop/skins/vtr-pon-clasic/spin_up.svg
    apps/desktop/skins/vtr-pon-clasic/spin_down.svg)

qt_add_resources(vtr-pon "vtrpon_styles"
  PREFIX "/vtrpon"
  FILES ${VTRPON_STYLE_RESOURCES})

set(VTRPON_APP_ICON_RESOURCES
    assets/icons/vtr-pon3/windows/png/VTR-PON3_16x16.png
    assets/icons/vtr-pon3/windows/png/VTR-PON3_32x32.png
    assets/icons/vtr-pon3/windows/png/VTR-PON3_48x48.png
    assets/icons/vtr-pon3/windows/png/VTR-PON3_64x64.png
    assets/icons/vtr-pon3/windows/png/VTR-PON3_128x128.png
    assets/icons/vtr-pon3/windows/png/VTR-PON3_256x256.png)
qt_add_resources(vtr-pon "vtrpon_app_icon"
  PREFIX "/vtrpon"
  FILES ${VTRPON_APP_ICON_RESOURCES})

if(WIN32)
  target_sources(vtr-pon PRIVATE
    apps/desktop/powerpoint_add_method_dialog.cpp
    apps/desktop/powerpoint_add_method_dialog.h
    apps/desktop/powerpoint_import_service.cpp
    apps/desktop/powerpoint_import_service.h
    apps/desktop/powerpoint_source_session_manager.cpp
    apps/desktop/powerpoint_source_session_manager.h
    apps/desktop/pdf_document_backend.cpp
    apps/desktop/pdf_document_backend.h
    apps/desktop/pdf_import_service.cpp
    apps/desktop/pdf_import_service.h
    apps/desktop/pdf_source_session_manager.cpp
    apps/desktop/pdf_source_session_manager.h
    apps/desktop/program_gpu_compositor.cpp
    apps/desktop/program_gpu_compositor.h
    apps/desktop/windows.manifest
    "${CMAKE_BINARY_DIR}/release/version.rc")
  target_compile_definitions(vtr-pon PRIVATE
    VTRPON_PDF_SUPPORT=1
    VTRPON_POWERPOINT_SUPPORT=1)
endif()

target_link_libraries(vtr-pon PRIVATE
  vtrpon-addon-package
  Qt6::Network
  Qt6::Pdf
  Qt6::Widgets
  PkgConfig::GSTREAMER
  PkgConfig::GSTREAMER_APP)

if(WIN32)
  target_link_libraries(vtr-pon PRIVATE
    PkgConfig::GSTREAMER_D3D11
    d3d11
    d3dcompiler
    dwmapi
    pdh
    dxgi)
  target_link_libraries(vtr-pon PRIVATE
    gdi32
    ole32
    oleaut32
    user32
    winmm)
endif()

target_compile_definitions(vtr-pon PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_definitions(vtr-pon PRIVATE VTRPON_APP_VERSION="${VTRPON_APP_VERSION}")

if(WIN32)
  qt_add_executable(vtr-pon-pfl-output
    apps/desktop/edit_pfl_output_main.cpp
    apps/desktop/edit_pfl_protocol.h
    apps/desktop/edit_pfl_renderer.h)
  target_compile_definitions(vtr-pon-pfl-output PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_link_libraries(vtr-pon-pfl-output PRIVATE Qt6::Core ole32 uuid)
  add_dependencies(vtr-pon vtr-pon-pfl-output)
endif()

add_custom_target(vtrpon-test-pattern-assets
  COMMAND ${CMAKE_COMMAND} -E make_directory
    "$<TARGET_FILE_DIR:vtr-pon>/assets/testpatterns"
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/testpatterns/media"
    "$<TARGET_FILE_DIR:vtr-pon>/assets/testpatterns/media"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/testpatterns/catalog.json"
    "$<TARGET_FILE_DIR:vtr-pon>/assets/testpatterns/catalog.json"
  VERBATIM)
add_dependencies(vtr-pon vtrpon-test-pattern-assets)
add_custom_target(vtrpon-license-files
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
    "$<TARGET_FILE_DIR:vtr-pon>/LICENSE"
  VERBATIM)
add_dependencies(vtr-pon vtrpon-license-files)
qt_add_translations(
  TARGETS vtr-pon
  SOURCE_TARGETS vtr-pon
  TS_FILES
    apps/desktop/i18n/vtrpon_en.ts
    apps/desktop/i18n/vtrpon_ja.ts
  RESOURCE_PREFIX "/i18n"
  LRELEASE_OPTIONS -idbased)

include(CTest)
if(BUILD_TESTING)
  qt_add_executable(update-catalog-test
    tests/update_catalog_test.cpp
    apps/desktop/update_catalog.cpp
    apps/desktop/update_catalog.h)
  target_include_directories(update-catalog-test PRIVATE apps/desktop)
  target_link_libraries(update-catalog-test PRIVATE Qt6::Core)
  if(MSVC)
    target_compile_options(update-catalog-test PRIVATE /utf-8)
  endif()
  add_test(NAME update-catalog COMMAND update-catalog-test)
  qt_add_executable(update-window-test
    tests/update_window_test.cpp
    apps/desktop/update_notification.cpp
    apps/desktop/update_notification.h
    apps/desktop/update_catalog.cpp
    apps/desktop/update_catalog.h)
  target_include_directories(update-window-test PRIVATE apps/desktop)
  target_link_libraries(update-window-test PRIVATE Qt6::Widgets Qt6::Network)
  if(MSVC)
    target_compile_options(update-window-test PRIVATE /utf-8)
  endif()
  add_test(NAME update-window COMMAND update-window-test)
  set_tests_properties(update-window PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=windows")

  qt_add_executable(addon-package-test tests/addon_package_test.cpp)
  if(WIN32)
    add_dependencies(addon-package-test vtr-pon-addon-manager)
  endif()
  target_include_directories(addon-package-test PRIVATE ${VTRPON_ZLIB_INCLUDE_DIR})
  target_link_libraries(addon-package-test PRIVATE
    vtrpon-addon-package ${VTRPON_ZLIB_LIBRARY})
  if(MSVC)
    target_compile_options(addon-package-test PRIVATE /utf-8)
  endif()
  add_test(NAME addon-package COMMAND addon-package-test)
endif()

if(WIN32)
  qt_add_executable(vtr-pon-operator-monitor WIN32
    apps/desktop/operator_monitor_main.cpp)
  qt_add_resources(vtr-pon-operator-monitor "vtrpon_operator_monitor_icon"
    PREFIX "/vtrpon"
    FILES ${VTRPON_APP_ICON_RESOURCES})
  target_link_libraries(vtr-pon-operator-monitor PRIVATE Qt6::Widgets)
  target_compile_definitions(vtr-pon-operator-monitor PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  add_dependencies(vtr-pon vtr-pon-operator-monitor)
  qt_add_library(vtrpon-operator-monitor-output STATIC
    apps/desktop/operator_monitor_output.cpp
    apps/desktop/operator_monitor_output.h)
  target_link_libraries(vtrpon-operator-monitor-output PRIVATE Qt6::Core d3d11)
  target_compile_definitions(vtrpon-operator-monitor-output PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  if(BUILD_TESTING)
    qt_add_executable(operator-monitor-isolation-test
      tests/operator_countdown_timer_test.h
      tests/operator_monitor_isolation_test.cpp
      apps/desktop/skins/controller_skin.cpp
      apps/desktop/program_gpu_compositor.cpp
      apps/desktop/view_menu_controller.cpp
      apps/desktop/system_load_widget.cpp
      apps/desktop/controller_scaling.cpp
      apps/desktop/language_text.cpp)
    target_include_directories(operator-monitor-isolation-test PRIVATE apps/desktop)
    target_compile_definitions(operator-monitor-isolation-test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    if(MSVC)
      target_compile_options(operator-monitor-isolation-test PRIVATE /utf-8)
    endif()
    target_link_libraries(operator-monitor-isolation-test PRIVATE
      Qt6::Widgets vtrpon-gpu-configuration vtrpon-operator-monitor-output
      PkgConfig::GSTREAMER PkgConfig::GSTREAMER_APP PkgConfig::GSTREAMER_D3D11
      d3d11 d3dcompiler dxgi gdi32 user32 pdh psapi)
    add_dependencies(operator-monitor-isolation-test vtr-pon-operator-monitor)
    add_test(NAME operator-monitor-isolation COMMAND operator-monitor-isolation-test)
    set_tests_properties(operator-monitor-isolation PROPERTIES
      TIMEOUT 90 ENVIRONMENT "QT_QPA_PLATFORM=windows" RUN_SERIAL TRUE)
  endif()
endif()

if(BUILD_TESTING)
  qt_add_executable(gpu-configuration-test tests/gpu_configuration_test.cpp)
  target_link_libraries(gpu-configuration-test PRIVATE vtrpon-gpu-configuration PkgConfig::GSTREAMER)
  add_test(NAME gpu-configuration COMMAND gpu-configuration-test)
  set_tests_properties(gpu-configuration PROPERTIES TIMEOUT 60)
endif()

if(BUILD_TESTING)
  qt_add_executable(test-pattern-store-test tests/test_pattern_store_test.cpp)
  target_include_directories(test-pattern-store-test PRIVATE apps/desktop)
  target_link_libraries(test-pattern-store-test PRIVATE Qt6::Core)
  add_dependencies(test-pattern-store-test vtrpon-test-pattern-assets)
  add_test(NAME test-pattern-store COMMAND test-pattern-store-test)
  qt_add_executable(program-audio-meter-test
    tests/program_audio_meter_test.cpp
    apps/desktop/program_audio_meter.cpp
    apps/desktop/program_audio_meter.h)
  target_include_directories(program-audio-meter-test PRIVATE apps/desktop)
  target_compile_definitions(program-audio-meter-test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_link_libraries(program-audio-meter-test PRIVATE Qt6::Gui PkgConfig::GSTREAMER)
  add_test(NAME program-audio-meter COMMAND program-audio-meter-test)
  set_tests_properties(program-audio-meter PROPERTIES TIMEOUT 30)

  qt_add_executable(program-audio-output-test
    tests/program_audio_output_test.cpp
    apps/desktop/audio_delay.cpp
    apps/desktop/audio_effects.cpp
    apps/desktop/audio_equalizer.cpp
    apps/desktop/program_audio_bridge.cpp
    apps/desktop/program_audio_output.cpp
    apps/desktop/program_audio_output.h)
  target_include_directories(program-audio-output-test PRIVATE apps/desktop)
  target_compile_definitions(program-audio-output-test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_link_libraries(program-audio-output-test PRIVATE Qt6::Core PkgConfig::GSTREAMER)
  add_test(NAME program-audio-output COMMAND program-audio-output-test)
  set_tests_properties(program-audio-output PROPERTIES TIMEOUT 60)

  qt_add_executable(program-audio-bridge-test
    tests/program_audio_bridge_test.cpp
    apps/desktop/audio_delay.cpp
    apps/desktop/audio_effects.cpp
    apps/desktop/audio_equalizer.cpp
    apps/desktop/program_audio_bridge.cpp
    apps/desktop/program_audio_bridge.h
    apps/desktop/program_audio_output.cpp
    apps/desktop/bgm_playback.cpp)
  target_include_directories(program-audio-bridge-test PRIVATE apps/desktop)
  target_compile_definitions(program-audio-bridge-test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_link_libraries(program-audio-bridge-test PRIVATE Qt6::Core Qt6::Gui PkgConfig::GSTREAMER)
  add_test(NAME program-audio-bridge COMMAND program-audio-bridge-test)
  set_tests_properties(program-audio-bridge PROPERTIES TIMEOUT 90)

  qt_add_executable(controller-settings-storage-test
    tests/controller_settings_storage_test.cpp
    apps/desktop/controller_settings.h)
  target_include_directories(controller-settings-storage-test PRIVATE apps/desktop)
  target_link_libraries(controller-settings-storage-test PRIVATE Qt6::Core)
  add_test(NAME controller-settings-storage COMMAND controller-settings-storage-test)

  qt_add_executable(midi-control-test
    tests/midi_control_test.cpp
    apps/desktop/midi_control_input.cpp
    apps/desktop/midi_control_input.h
    apps/desktop/midi_message_queue.h
    apps/desktop/midi_control_settings_page.cpp
    apps/desktop/midi_control_settings_page.h
    apps/desktop/controller_settings_io.cpp
    apps/desktop/controller_operations.h
    apps/desktop/controller_scaling.cpp
    apps/desktop/language_text.cpp)
  target_include_directories(midi-control-test PRIVATE apps/desktop)
  target_link_libraries(midi-control-test PRIVATE Qt6::Widgets)
  if(WIN32)
    target_compile_definitions(midi-control-test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(midi-control-test PRIVATE winmm)
  endif()
  if(MSVC)
    target_compile_options(midi-control-test PRIVATE /utf-8)
  endif()
  add_dependencies(midi-control-test release_translations)
  add_test(NAME midi-control COMMAND midi-control-test "${CMAKE_CURRENT_BINARY_DIR}/vtrpon_ja.qm" "${CMAKE_CURRENT_BINARY_DIR}/midi-settings-ja.png")
  set_tests_properties(midi-control PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=windows")

  qt_add_executable(application-log-test
    tests/application_log_test.cpp
    apps/desktop/controller_operations.h
    apps/desktop/application_log.cpp
    apps/desktop/log_viewer.cpp
    apps/desktop/language_text.cpp
    apps/desktop/language_manager.cpp)
  target_include_directories(application-log-test PRIVATE apps/desktop)
  target_link_libraries(application-log-test PRIVATE Qt6::Widgets PkgConfig::GSTREAMER)
  if(MSVC)
    target_link_libraries(application-log-test PRIVATE delayimp)
    target_link_options(application-log-test PRIVATE
      /DELAYLOAD:gstreamer-1.0-0.dll
      /DELAYLOAD:glib-2.0-0.dll
      /DELAYLOAD:gobject-2.0-0.dll)
  endif()
  add_test(NAME application-log COMMAND application-log-test)

  qt_add_executable(controller-operations-test
    tests/controller_operations_test.cpp
    apps/desktop/controller_operations.h)
  target_include_directories(controller-operations-test PRIVATE apps/desktop)
  target_link_libraries(controller-operations-test PRIVATE Qt6::Core)
  add_test(NAME controller-operations COMMAND controller-operations-test)

  qt_add_executable(macro-runner-test
    tests/macro_runner_test.cpp
    apps/desktop/macro_runner.cpp
    apps/desktop/macro_runner.h
    apps/desktop/macro_document.cpp
    apps/desktop/controller_operations.h
    apps/desktop/playlist_model.cpp
    apps/desktop/playlist_play_sequence.cpp
    apps/desktop/playlist_media_availability.cpp
    apps/desktop/playlist_model.h)
  target_include_directories(macro-runner-test PRIVATE apps/desktop)
  target_link_libraries(macro-runner-test PRIVATE Qt6::Core)
  if(MSVC)
    target_compile_options(macro-runner-test PRIVATE /utf-8)
  endif()
  add_test(NAME macro-runner COMMAND macro-runner-test)

  qt_add_executable(macro-editor-window-test
    tests/macro_editor_window_test.cpp
    apps/desktop/controller_scaling.cpp
    apps/desktop/macro_editor_window.cpp
    apps/desktop/macro_editor_window.h
    apps/desktop/macro_runner.cpp
    apps/desktop/macro_runner.h
    apps/desktop/macro_document.cpp
    apps/desktop/controller_operations.h
    apps/desktop/playlist_model.cpp
    apps/desktop/playlist_play_sequence.cpp
    apps/desktop/playlist_media_availability.cpp
    apps/desktop/playlist_model.h
    apps/desktop/network_time_clock.cpp
    apps/desktop/language_manager.cpp
    apps/desktop/language_text.cpp)
  target_include_directories(macro-editor-window-test PRIVATE apps/desktop)
  target_link_libraries(macro-editor-window-test PRIVATE Qt6::Widgets Qt6::Network)
  if(MSVC)
    target_compile_options(macro-editor-window-test PRIVATE /utf-8)
  endif()
  add_test(NAME macro-editor-window COMMAND macro-editor-window-test)
  set_tests_properties(macro-editor-window PROPERTIES ENVIRONMENT
    "QT_QPA_PLATFORM=offscreen;QT_QPA_PLATFORM_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::QOffscreenIntegrationPlugin>")
  set_tests_properties(macro-runner macro-editor-window PROPERTIES TIMEOUT 30)

  qt_add_executable(playlist-item-operations-test
    tests/playlist_item_operations_test.cpp
    apps/desktop/controller_operations.h
    apps/desktop/playlist_item_operations.cpp
    apps/desktop/playlist_item_operations.h
    apps/desktop/macro_document.cpp
    apps/desktop/playlist_model.cpp
    apps/desktop/playlist_play_sequence.cpp
    apps/desktop/playlist_media_availability.cpp
    apps/desktop/playlist_model.h)
  target_include_directories(playlist-item-operations-test PRIVATE apps/desktop)
  target_link_libraries(playlist-item-operations-test PRIVATE Qt6::Core)
  add_test(NAME playlist-item-operations COMMAND playlist-item-operations-test)

  if(WIN32)
    qt_add_executable(program-playout-transition-test
      tests/controller_shortcuts_test.cpp
      apps/desktop/controller_shortcuts.cpp
      apps/desktop/controller_shortcuts.h
      tests/document_import_queue_test.cpp
      apps/desktop/document_import_queue.cpp
      apps/desktop/document_import_queue.h
      apps/desktop/document_video_encoder.cpp
      apps/desktop/pdf_import_service.cpp
      apps/desktop/powerpoint_import_service.cpp
      tests/operator_timer_panel_test.h
      apps/desktop/view_menu_controller.cpp
      apps/desktop/system_load_widget.cpp
      tests/program_playout_transition_test.cpp
      apps/desktop/uvc_device_monitor.cpp
      apps/desktop/uvc_device_monitor.h
      tests/switcher_control_test.cpp
      apps/desktop/switcher_control.cpp
      apps/desktop/switcher_control.h
      apps/desktop/roland_v8hd_midi_switcher.cpp
      apps/desktop/roland_v8hd_midi_switcher.h
      apps/desktop/atem_addon_switcher.cpp
      apps/desktop/atem_addon_switcher.h
      apps/desktop/controller_operations.h
      apps/desktop/program_playout_controller.cpp
      apps/desktop/program_audio_controller.cpp
      apps/desktop/bgm_playout_controller.cpp
      apps/desktop/program_panel.cpp
      apps/desktop/playlist_panel.cpp
      apps/desktop/playlist_context_menu.cpp
      apps/desktop/playlist_context_menu.h
      apps/desktop/playlist_input_widgets.cpp
      apps/desktop/playlist_input_widgets.h
      apps/desktop/playlist_item_delegate.cpp
      apps/desktop/playlist_item_delegate.h
      apps/desktop/playlist_item_operations.cpp
      apps/desktop/playlist_item_operations.h
      apps/desktop/playlist_thumbnail_store.cpp
      apps/desktop/playlist_thumbnail_store.h
      apps/desktop/playlist_view_roles.h
      apps/desktop/macro_document.cpp
      apps/desktop/playlist_model.cpp
      apps/desktop/playlist_play_sequence.cpp
      apps/desktop/playlist_media_availability.cpp
      apps/desktop/playlist_item_playout_state_machine.cpp
      apps/desktop/timed_still_playout_clock.cpp
      apps/desktop/dual_deck_player.cpp
      apps/desktop/deck_source_session.cpp
      apps/desktop/bgm_playback.cpp
      apps/desktop/dsk_playback.cpp
      apps/desktop/program_audio_output.cpp
      apps/desktop/audio_delay.cpp
      apps/desktop/audio_effects.cpp
      apps/desktop/audio_equalizer.cpp
      apps/desktop/program_audio_bridge.cpp
      apps/desktop/program_audio_meter.cpp
      apps/desktop/program_gpu_compositor.cpp
      apps/desktop/audio_output_settings.cpp
      apps/desktop/audio_fader_widget.cpp
      apps/desktop/audio_meter_widget.cpp
      apps/desktop/range_marker_slider.cpp
      apps/desktop/controller_scaling.cpp
      apps/desktop/skins/controller_skin.cpp
      apps/desktop/language_text.cpp
      apps/desktop/user_info.cpp
      apps/desktop/network_time_clock.cpp
      apps/desktop/media_thumbnailer.cpp
      apps/desktop/media_duration_probe.cpp
      apps/desktop/uvc_audio_mapping.cpp
      apps/desktop/uvc_source_session_manager.cpp
      apps/desktop/powerpoint_source_session_manager.cpp
      apps/desktop/pdf_source_session_manager.cpp
      apps/desktop/pdf_document_backend.cpp)
    target_include_directories(program-playout-transition-test PRIVATE apps/desktop)
    qt_add_resources(program-playout-transition-test "timer_test_styles"
      PREFIX "/vtrpon"
      FILES
        apps/desktop/skins/hardware/controller.qss
        apps/desktop/skins/hardware/controller_black_brushed_metal.svg
        apps/desktop/skins/hardware/controller_brushed_metal.svg
        apps/desktop/skins/hardware/controller_panel_brushed_metal.svg
        apps/desktop/skins/hardware/controller_walnut_trim.svg
        apps/desktop/skins/material-dark/controller.qss
        apps/desktop/skins/material-dark/check.svg
        apps/desktop/skins/material-dark/chevron_down.svg
        apps/desktop/skins/vtr-pon-clasic/controller.qss)
    target_compile_definitions(program-playout-transition-test PRIVATE
      NOMINMAX WIN32_LEAN_AND_MEAN
      VTRPON_PDF_SUPPORT=1 VTRPON_POWERPOINT_SUPPORT=1)
    target_link_libraries(program-playout-transition-test PRIVATE
      Qt6::Widgets Qt6::Network Qt6::Pdf
      PkgConfig::GSTREAMER PkgConfig::GSTREAMER_APP PkgConfig::GSTREAMER_D3D11
      d3d11 d3dcompiler dwmapi dxgi gdi32 ole32 oleaut32 user32 winmm pdh psapi)
    add_test(NAME program-playout-transition COMMAND program-playout-transition-test)
    add_test(NAME document-import-queue
      COMMAND program-playout-transition-test --document-import-queue-only)
    set_tests_properties(document-import-queue PROPERTIES
      TIMEOUT 30 ENVIRONMENT "QT_QPA_PLATFORM=windows")
    set_tests_properties(program-playout-transition PROPERTIES
      TIMEOUT 120 ENVIRONMENT "QT_QPA_PLATFORM=windows")

    qt_add_executable(deck-source-session-test
      tests/deck_source_session_test.cpp
      apps/desktop/audio_delay.cpp
      apps/desktop/program_audio_bridge.cpp
      apps/desktop/deck_source_session.cpp
      apps/desktop/deck_source_session.h
      apps/desktop/uvc_source_session_manager.cpp
      apps/desktop/uvc_source_session_manager.h
      apps/desktop/powerpoint_source_session_manager.cpp
      apps/desktop/pdf_source_session_manager.cpp
      apps/desktop/pdf_document_backend.cpp
      apps/desktop/program_gpu_compositor.cpp)
    target_include_directories(deck-source-session-test PRIVATE apps/desktop)
    target_compile_definitions(deck-source-session-test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(deck-source-session-test PRIVATE
      Qt6::Widgets
      Qt6::Pdf
      PkgConfig::GSTREAMER
      PkgConfig::GSTREAMER_APP
      PkgConfig::GSTREAMER_D3D11
      d3d11 d3dcompiler dwmapi dxgi gdi32 ole32 oleaut32 user32)
    add_test(NAME deck-source-session COMMAND deck-source-session-test)
    set_tests_properties(deck-source-session PROPERTIES
      TIMEOUT 60
      ENVIRONMENT "QT_QPA_PLATFORM=windows")

    qt_add_executable(dual-deck-player-contract-test
      tests/dual_deck_player_contract_test.cpp
      apps/desktop/audio_delay.cpp
      apps/desktop/audio_effects.cpp
      apps/desktop/audio_equalizer.cpp
      apps/desktop/program_audio_bridge.cpp
      apps/desktop/dual_deck_player.cpp
      apps/desktop/deck_source_session.cpp
      apps/desktop/bgm_playback.cpp
      apps/desktop/dsk_playback.cpp
      apps/desktop/program_audio_output.cpp
      apps/desktop/program_audio_meter.cpp
      apps/desktop/audio_output_settings.cpp
      apps/desktop/audio_output_settings.h
      apps/desktop/uvc_source_session_manager.cpp
      apps/desktop/uvc_source_session_manager.h
      apps/desktop/powerpoint_source_session_manager.cpp
      apps/desktop/pdf_source_session_manager.cpp
      apps/desktop/pdf_document_backend.cpp
      apps/desktop/program_gpu_compositor.cpp)
    target_include_directories(dual-deck-player-contract-test PRIVATE apps/desktop)
    target_compile_definitions(dual-deck-player-contract-test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(dual-deck-player-contract-test PRIVATE
      Qt6::Widgets
      Qt6::Pdf
      PkgConfig::GSTREAMER
      PkgConfig::GSTREAMER_APP
      PkgConfig::GSTREAMER_D3D11
      d3d11 d3dcompiler dwmapi dxgi gdi32 ole32 oleaut32 user32)
    add_test(NAME dual-deck-player-contract COMMAND dual-deck-player-contract-test)
    set_tests_properties(dual-deck-player-contract PROPERTIES
      TIMEOUT 90
      ENVIRONMENT "QT_QPA_PLATFORM=windows")

    qt_add_executable(aux-playback-test
      tests/aux_playback_test.cpp
      apps/desktop/audio_delay.cpp
      apps/desktop/audio_equalizer.cpp
      apps/desktop/program_audio_bridge.cpp
      apps/desktop/bgm_playback.cpp
      apps/desktop/dsk_playback.cpp
      apps/desktop/program_gpu_compositor.cpp)
    target_include_directories(aux-playback-test PRIVATE apps/desktop)
    target_compile_definitions(aux-playback-test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(aux-playback-test PRIVATE
      Qt6::Widgets
      PkgConfig::GSTREAMER
      PkgConfig::GSTREAMER_APP
      PkgConfig::GSTREAMER_D3D11
      d3d11 d3dcompiler dwmapi dxgi user32)
    add_test(NAME aux-playback COMMAND aux-playback-test)
    set_tests_properties(aux-playback PROPERTIES
      TIMEOUT 60
      ENVIRONMENT "QT_QPA_PLATFORM=windows")

    qt_add_executable(edit-pfl-audio-test
      tests/edit_pfl_audio_test.cpp
      apps/desktop/audio_delay.cpp
      apps/desktop/audio_effects.cpp
      apps/desktop/audio_equalizer.cpp
      apps/desktop/edit_player.cpp
      apps/desktop/edit_player.h
      apps/desktop/edit_pfl_output.cpp
      apps/desktop/edit_pfl_output.h
      apps/desktop/audio_output_settings.cpp
      apps/desktop/audio_output_settings.h
      apps/desktop/language_text.cpp)
    target_include_directories(edit-pfl-audio-test PRIVATE apps/desktop)
    target_compile_definitions(edit-pfl-audio-test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(edit-pfl-audio-test PRIVATE Qt6::Widgets PkgConfig::GSTREAMER vtrpon-vst-bridge)
    add_dependencies(edit-pfl-audio-test vtr-pon-pfl-output)
    add_test(NAME edit-pfl-audio COMMAND edit-pfl-audio-test)
    set_tests_properties(edit-pfl-audio PROPERTIES TIMEOUT 30)
  endif()
  qt_add_executable(language-catalog-test
    tests/language_catalog_test.cpp
    apps/desktop/language_text.cpp
    apps/desktop/language_text.h)

  target_include_directories(language-catalog-test PRIVATE
    apps/desktop)
  target_link_libraries(language-catalog-test PRIVATE
    Qt6::Core)
  add_dependencies(language-catalog-test release_translations)

  add_test(NAME language-catalog
    COMMAND language-catalog-test
      "${CMAKE_CURRENT_BINARY_DIR}/vtrpon_en.qm"
      "${CMAKE_CURRENT_BINARY_DIR}/vtrpon_ja.qm")

  qt_add_executable(playlist-item-playout-state-machine-test
    tests/playlist_item_playout_state_machine_test.cpp
    apps/desktop/playlist_item.h
    apps/desktop/playlist_item_playout_state_machine.cpp
    apps/desktop/playlist_item_playout_state_machine.h
    apps/desktop/macro_document.cpp
    apps/desktop/playlist_model.cpp
    apps/desktop/playlist_play_sequence.cpp
    apps/desktop/playlist_media_availability.cpp
    apps/desktop/playlist_model.h)

  target_include_directories(playlist-item-playout-state-machine-test PRIVATE
    apps/desktop)
  target_link_libraries(playlist-item-playout-state-machine-test PRIVATE
    Qt6::Core)

  add_test(NAME playlist-item-playout-state-machine
    COMMAND playlist-item-playout-state-machine-test)

  qt_add_executable(timed-still-playout-clock-test
    tests/timed_still_playout_clock_test.cpp
    apps/desktop/playlist_item.h
    apps/desktop/playlist_item_playout_state_machine.cpp
    apps/desktop/playlist_item_playout_state_machine.h
    apps/desktop/macro_document.cpp
    apps/desktop/playlist_model.cpp
    apps/desktop/playlist_play_sequence.cpp
    apps/desktop/playlist_media_availability.cpp
    apps/desktop/playlist_model.h
    apps/desktop/timed_still_playout_clock.cpp
    apps/desktop/timed_still_playout_clock.h)

  target_include_directories(timed-still-playout-clock-test PRIVATE
    apps/desktop)
  target_link_libraries(timed-still-playout-clock-test PRIVATE
    Qt6::Core)

  add_test(NAME timed-still-playout-clock
    COMMAND timed-still-playout-clock-test)

  qt_add_executable(network-time-clock-test
    tests/network_time_clock_test.cpp
    apps/desktop/network_time_clock.cpp
    apps/desktop/network_time_clock.h)

  target_include_directories(network-time-clock-test PRIVATE
    apps/desktop)
  target_link_libraries(network-time-clock-test PRIVATE
    Qt6::Core
    Qt6::Network)

  add_test(NAME network-time-clock
    COMMAND network-time-clock-test)

  qt_add_executable(background-io-queue-test
    tests/background_io_queue_test.cpp
    apps/desktop/application_shutdown_deadline.cpp
    apps/desktop/playlist_document_io.cpp
    apps/desktop/macro_document.cpp
    apps/desktop/playlist_model.cpp
    apps/desktop/playlist_play_sequence.cpp
    apps/desktop/playlist_media_availability.cpp)
  target_include_directories(background-io-queue-test PRIVATE apps/desktop)
  target_link_libraries(background-io-queue-test PRIVATE Qt6::Core)
  add_test(NAME background-io-queue COMMAND background-io-queue-test)
  set_tests_properties(background-io-queue PROPERTIES TIMEOUT 10)

  qt_add_executable(device-monitor-worker-test tests/device_monitor_worker_test.cpp)
  target_include_directories(device-monitor-worker-test PRIVATE apps/desktop)
  target_link_libraries(device-monitor-worker-test PRIVATE Qt6::Core PkgConfig::GSTREAMER)
  add_test(NAME device-monitor-worker COMMAND device-monitor-worker-test)
  set_tests_properties(device-monitor-worker PROPERTIES TIMEOUT 10)
  if(MSVC)
    target_compile_options(background-io-queue-test PRIVATE /utf-8)
    target_compile_options(device-monitor-worker-test PRIVATE /utf-8)
  endif()

  qt_add_executable(playlist-document-io-test
    tests/playlist_document_io_test.cpp
    apps/desktop/playlist_document_io.cpp
    apps/desktop/playlist_document_io.h
    apps/desktop/playlist_item.h
    apps/desktop/macro_document.cpp
    apps/desktop/playlist_model.cpp
    apps/desktop/playlist_play_sequence.cpp
    apps/desktop/playlist_media_availability.cpp
    apps/desktop/playlist_model.h)

  target_include_directories(playlist-document-io-test PRIVATE
    apps/desktop)
  target_link_libraries(playlist-document-io-test PRIVATE
    Qt6::Core)

  add_test(NAME playlist-document-io
    COMMAND playlist-document-io-test)

  qt_add_executable(media-import-queue-test
    tests/media_import_queue_test.cpp
    apps/desktop/media_import_queue.cpp
    apps/desktop/media_import_queue.h
    apps/desktop/media_duration_probe.cpp
    apps/desktop/macro_document.cpp
    apps/desktop/playlist_model.cpp
    apps/desktop/playlist_play_sequence.cpp
    apps/desktop/playlist_media_availability.cpp
    apps/desktop/playlist_model.h
    apps/desktop/playlist_item_playout_state_machine.cpp)
  target_include_directories(media-import-queue-test PRIVATE apps/desktop)
  target_link_libraries(media-import-queue-test PRIVATE Qt6::Core PkgConfig::GSTREAMER)
  add_test(NAME media-import-queue COMMAND media-import-queue-test)

  qt_add_executable(media-thumbnailer-test
    tests/media_thumbnailer_test.cpp
    apps/desktop/media_duration_probe.cpp
    apps/desktop/media_duration_probe.h
    apps/desktop/media_thumbnailer.cpp
    apps/desktop/media_thumbnailer.h
    apps/desktop/pdf_document_backend.cpp
    apps/desktop/pdf_document_backend.h
    apps/desktop/pdf_source_session_manager.cpp
    apps/desktop/pdf_source_session_manager.h
    apps/desktop/playlist_item.h)

  target_include_directories(media-thumbnailer-test PRIVATE
    apps/desktop)
  target_compile_definitions(media-thumbnailer-test PRIVATE
    VTRPON_PDF_SUPPORT=1)
  target_link_libraries(media-thumbnailer-test PRIVATE
    Qt6::Gui
    Qt6::Pdf
    PkgConfig::GSTREAMER
    PkgConfig::GSTREAMER_APP)

  add_test(NAME media-thumbnailer
    COMMAND media-thumbnailer-test)
  if(WIN32)
    set_tests_properties(media-thumbnailer PROPERTIES
      ENVIRONMENT "QT_QPA_PLATFORM=windows")
  else()
    set_tests_properties(media-thumbnailer PROPERTIES
      ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
  endif()

  qt_add_executable(playlist-panel-drag-drop-test
    apps/desktop/controller_window_scaling.cpp
    apps/desktop/controller_window_scaling.h
    apps/desktop/controller_operations.h
    tests/playlist_panel_drag_drop_test.cpp
    apps/desktop/view_menu_controller.cpp
    apps/desktop/view_menu_controller.h
    apps/desktop/controller_scaling.cpp
    apps/desktop/controller_scaling.h
    apps/desktop/audio_output_settings.cpp
    apps/desktop/audio_output_settings.h
    apps/desktop/controller_settings_dialog.cpp
    apps/desktop/addon_management_page.cpp
    apps/desktop/addon_management_page.h
    apps/desktop/controller_settings_io.cpp
    apps/desktop/controller_settings_io.h
    apps/desktop/controller_settings_dialog.h
    apps/desktop/settings_style.cpp
    apps/desktop/settings_style.h
    apps/desktop/atem_addon_switcher.cpp
    apps/desktop/atem_addon_switcher.h
    apps/desktop/language_manager.cpp
    apps/desktop/language_manager.h
    apps/desktop/media_thumbnailer.cpp
    apps/desktop/media_thumbnailer.h
    apps/desktop/network_time_clock.cpp
    apps/desktop/network_time_clock.h
    apps/desktop/playlist_item.h
    apps/desktop/macro_document.cpp
    apps/desktop/playlist_model.cpp
    apps/desktop/playlist_play_sequence.cpp
    apps/desktop/playlist_media_availability.cpp
    apps/desktop/playlist_model.h
    apps/desktop/playlist_panel.cpp
    apps/desktop/playlist_context_menu.cpp
    apps/desktop/playlist_context_menu.h
    apps/desktop/playlist_input_widgets.cpp
    apps/desktop/playlist_input_widgets.h
    apps/desktop/playlist_item_delegate.cpp
    apps/desktop/playlist_item_delegate.h
    apps/desktop/playlist_item_operations.cpp
    apps/desktop/playlist_item_operations.h
    apps/desktop/playlist_thumbnail_store.cpp
    apps/desktop/playlist_thumbnail_store.h
    apps/desktop/playlist_view_roles.h
    apps/desktop/playlist_panel.h
    apps/desktop/roland_v8hd_midi_switcher.cpp
    apps/desktop/roland_v8hd_midi_switcher.h
    apps/desktop/switcher_control.cpp
    apps/desktop/switcher_control.h
    apps/desktop/switcher_addon_api.h
    apps/desktop/language_text.cpp
    apps/desktop/language_text.h
    apps/desktop/skins/controller_skin.cpp
    apps/desktop/skins/controller_skin.h
    apps/desktop/user_info.cpp
    apps/desktop/user_info.h
    apps/desktop/uvc_audio_mapping.cpp
    apps/desktop/uvc_audio_mapping.h
    apps/desktop/uvc_device_monitor.cpp
    apps/desktop/uvc_device_monitor.h)

  target_include_directories(playlist-panel-drag-drop-test PRIVATE
    apps/desktop)
  target_link_libraries(playlist-panel-drag-drop-test PRIVATE
    vtrpon-addon-package
    Qt6::Network
    Qt6::Widgets
    PkgConfig::GSTREAMER
    PkgConfig::GSTREAMER_APP)
  if(WIN32)
    target_compile_definitions(playlist-panel-drag-drop-test PRIVATE
      NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(playlist-panel-drag-drop-test PRIVATE winmm)
  endif()
  add_dependencies(playlist-panel-drag-drop-test release_translations)
  qt_add_resources(playlist-panel-drag-drop-test "settings_test_styles"
    PREFIX "/vtrpon"
    FILES ${VTRPON_STYLE_RESOURCES})

  add_test(NAME playlist-panel-drag-drop
    COMMAND playlist-panel-drag-drop-test)
  if(WIN32)
    set_tests_properties(playlist-panel-drag-drop PROPERTIES
      ENVIRONMENT "QT_QPA_PLATFORM=windows")
  else()
    set_tests_properties(playlist-panel-drag-drop PROPERTIES
      ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
  endif()

  qt_add_executable(edit-panel-duration-control-test
    apps/desktop/midi_control_input.cpp
    apps/desktop/midi_control_input.h
    apps/desktop/controller_window_scaling.cpp
    apps/desktop/controller_window_scaling.h
    apps/desktop/controller_operations.h
    tests/edit_panel_duration_control_test.cpp
    apps/desktop/audio_fader_widget.cpp
    apps/desktop/audio_fader_widget.h
    apps/desktop/audio_meter_widget.cpp
    apps/desktop/audio_meter_widget.h
    apps/desktop/controller_scaling.cpp
    apps/desktop/controller_scaling.h
    apps/desktop/edit_panel.cpp
    apps/desktop/edit_panel.h
    apps/desktop/language_text.cpp
    apps/desktop/language_text.h
    apps/desktop/program_panel.cpp
    apps/desktop/program_panel.h
    apps/desktop/range_marker_slider.cpp
    apps/desktop/range_marker_slider.h
    apps/desktop/skins/controller_skin.cpp
    apps/desktop/skins/controller_skin.h
    apps/desktop/user_info.cpp
    apps/desktop/user_info.h)

  target_include_directories(edit-panel-duration-control-test PRIVATE
    apps/desktop)
  target_link_libraries(edit-panel-duration-control-test PRIVATE
    Qt6::Widgets)
  if(WIN32)
    target_compile_definitions(edit-panel-duration-control-test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(edit-panel-duration-control-test PRIVATE winmm)
  endif()
  add_test(NAME midi-master-control COMMAND edit-panel-duration-control-test --midi-master-only)
  set_tests_properties(midi-master-control PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=windows")
  add_dependencies(edit-panel-duration-control-test release_translations)

  if(CMAKE_CONFIGURATION_TYPES)
    # Widget tests load QM files beside the executable. Multi-config builds
    # generate them in the build root; refresh the active runtime directory
    # every build, including translation-only changes.
    add_custom_target(vtrpon-test-translations
      COMMAND "${CMAKE_COMMAND}" -E make_directory
        "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>"
      COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_CURRENT_BINARY_DIR}/vtrpon_en.qm"
        "${CMAKE_CURRENT_BINARY_DIR}/vtrpon_ja.qm"
        "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>"
      VERBATIM)
    add_dependencies(vtrpon-test-translations release_translations)
    add_dependencies(playlist-panel-drag-drop-test vtrpon-test-translations)
    add_dependencies(edit-panel-duration-control-test vtrpon-test-translations)
  endif()

  add_test(NAME edit-panel-duration-control
    COMMAND edit-panel-duration-control-test
      "${CMAKE_CURRENT_SOURCE_DIR}/apps/desktop/skins/hardware/controller.qss"
      "${CMAKE_CURRENT_SOURCE_DIR}/apps/desktop/skins/material-dark/controller.qss"
      "${CMAKE_CURRENT_SOURCE_DIR}/apps/desktop/skins/vtr-pon-clasic/controller.qss")
  if(WIN32)
    set_tests_properties(edit-panel-duration-control PROPERTIES
      ENVIRONMENT "QT_QPA_PLATFORM=windows")
  else()
    set_tests_properties(edit-panel-duration-control PROPERTIES
      ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
  endif()

  if(WIN32)
    qt_add_executable(image-add-contract-test
      tests/image_add_contract_test.cpp
      apps/desktop/controller_scaling.cpp
      apps/desktop/controller_scaling.h
      apps/desktop/image_add_dialog.cpp
      apps/desktop/image_add_dialog.h
      apps/desktop/image_add_registration.cpp
      apps/desktop/image_add_registration.h
      apps/desktop/language_text.cpp
      apps/desktop/language_text.h
      apps/desktop/playlist_item.h)

    target_include_directories(image-add-contract-test PRIVATE
      apps/desktop)
    target_compile_definitions(image-add-contract-test PRIVATE
      NOMINMAX
      WIN32_LEAN_AND_MEAN)
    target_link_libraries(image-add-contract-test PRIVATE
      Qt6::Widgets)

    add_test(NAME image-add-contract
      COMMAND image-add-contract-test)
    set_tests_properties(image-add-contract
      PROPERTIES
      ENVIRONMENT "QT_QPA_PLATFORM=windows")

    qt_add_executable(powerpoint-import-contract-test
      tests/powerpoint_import_contract_test.cpp
      apps/desktop/controller_scaling.cpp
      apps/desktop/controller_scaling.h
      apps/desktop/document_add_method_dialog.cpp
      apps/desktop/document_add_method_dialog.h
      apps/desktop/document_video_encoder.cpp
      apps/desktop/document_video_encoder.h
      apps/desktop/language_text.cpp
      apps/desktop/language_text.h
      apps/desktop/media_duration_probe.cpp
      apps/desktop/media_duration_probe.h
      apps/desktop/media_thumbnailer.cpp
      apps/desktop/media_thumbnailer.h
      apps/desktop/playlist_item.h
      apps/desktop/powerpoint_add_method_dialog.cpp
      apps/desktop/powerpoint_add_method_dialog.h
      apps/desktop/powerpoint_import_service.cpp
      apps/desktop/powerpoint_import_service.h
      apps/desktop/powerpoint_source_session_manager.cpp
      apps/desktop/powerpoint_source_session_manager.h)

    target_include_directories(powerpoint-import-contract-test PRIVATE
      apps/desktop)
    target_compile_definitions(powerpoint-import-contract-test PRIVATE
      NOMINMAX
      VTRPON_POWERPOINT_SUPPORT=1
      WIN32_LEAN_AND_MEAN)
    target_link_libraries(powerpoint-import-contract-test PRIVATE
      Qt6::Widgets
      PkgConfig::GSTREAMER
      PkgConfig::GSTREAMER_APP
      dwmapi
      gdi32
      ole32
      oleaut32
      user32)

    add_test(NAME powerpoint-import-contract
      COMMAND powerpoint-import-contract-test)
    set_tests_properties(powerpoint-import-contract PROPERTIES
      ENVIRONMENT "QT_QPA_PLATFORM=windows")

    qt_add_executable(pdf-import-contract-test
      tests/pdf_import_contract_test.cpp
      apps/desktop/controller_scaling.cpp
      apps/desktop/controller_scaling.h
      apps/desktop/document_add_method_dialog.cpp
      apps/desktop/document_add_method_dialog.h
      apps/desktop/document_video_encoder.cpp
      apps/desktop/document_video_encoder.h
      apps/desktop/language_text.cpp
      apps/desktop/language_text.h
      apps/desktop/media_duration_probe.cpp
      apps/desktop/media_duration_probe.h
      apps/desktop/pdf_document_backend.cpp
      apps/desktop/pdf_document_backend.h
      apps/desktop/pdf_import_service.cpp
      apps/desktop/pdf_import_service.h
      apps/desktop/pdf_source_session_manager.cpp
      apps/desktop/pdf_source_session_manager.h
      apps/desktop/playlist_item.h)

    target_include_directories(pdf-import-contract-test PRIVATE
      apps/desktop)
    target_compile_definitions(pdf-import-contract-test PRIVATE
      NOMINMAX
      WIN32_LEAN_AND_MEAN)
    target_link_libraries(pdf-import-contract-test PRIVATE
      Qt6::Pdf
      Qt6::Widgets
      PkgConfig::GSTREAMER
      PkgConfig::GSTREAMER_APP)
    add_dependencies(pdf-import-contract-test vtr-pon)

    add_test(NAME pdf-import-contract
      COMMAND pdf-import-contract-test)
    add_test(NAME pdf-import-video-smoke
      COMMAND pdf-import-contract-test --video-smoke)
    set_tests_properties(pdf-import-contract pdf-import-video-smoke PROPERTIES
      ENVIRONMENT "QT_QPA_PLATFORM=windows")
  endif()
endif()

if(MSVC)
  list(GET GSTREAMER_LIBRARY_DIRS 0 GSTREAMER_LIBRARY_DIR)
  get_filename_component(GSTREAMER_RUNTIME_ROOT "${GSTREAMER_LIBRARY_DIR}" DIRECTORY)

  target_compile_definitions(vtr-pon PRIVATE
    VTRPON_GSTREAMER_BIN_DIR=L\"${GSTREAMER_RUNTIME_ROOT}/bin\")
  target_link_libraries(vtr-pon PRIVATE delayimp)
  target_link_options(vtr-pon PRIVATE
    /DELAYLOAD:gstbase-1.0-0.dll
    /DELAYLOAD:gstvideo-1.0-0.dll
    /DELAYLOAD:gstreamer-1.0-0.dll
    /DELAYLOAD:gobject-2.0-0.dll
    /DELAYLOAD:glib-2.0-0.dll
    /DELAYLOAD:gstapp-1.0-0.dll
    /DELAYLOAD:gstaudio-1.0-0.dll
    /DELAYLOAD:gstd3d11-1.0-0.dll
    /DELAYLOAD:gstpbutils-1.0-0.dll)
  target_compile_options(vtr-pon PRIVATE /utf-8)
  if(BUILD_TESTING)
    target_compile_definitions(pdf-import-contract-test PRIVATE
      VTRPON_GSTREAMER_BIN_DIR=L"${GSTREAMER_RUNTIME_ROOT}/bin")
    target_link_libraries(pdf-import-contract-test PRIVATE delayimp)
    target_link_options(pdf-import-contract-test PRIVATE
      /DELAYLOAD:gstreamer-1.0-0.dll
      /DELAYLOAD:gobject-2.0-0.dll
      /DELAYLOAD:glib-2.0-0.dll
      /DELAYLOAD:gstapp-1.0-0.dll
      /DELAYLOAD:gstpbutils-1.0-0.dll)
    set_tests_properties(program-audio-output program-audio-meter program-audio-bridge
      deck-source-session dual-deck-player-contract aux-playback program-playout-transition
      document-import-queue
      media-thumbnailer media-import-queue playlist-panel-drag-drop
      powerpoint-import-contract pdf-import-contract pdf-import-video-smoke
      PROPERTIES
      ENVIRONMENT_MODIFICATION
        "PATH=path_list_prepend:${GSTREAMER_RUNTIME_ROOT}/bin")
    target_compile_options(language-catalog-test PRIVATE /utf-8)
    target_compile_options(program-playout-transition-test PRIVATE /utf-8)
    target_compile_options(playlist-document-io-test PRIVATE /utf-8)
    target_compile_options(media-thumbnailer-test PRIVATE /utf-8)
    target_compile_options(media-import-queue-test PRIVATE /utf-8)
    target_compile_options(playlist-panel-drag-drop-test PRIVATE /utf-8)
    target_compile_options(edit-panel-duration-control-test PRIVATE /utf-8)
    target_compile_options(image-add-contract-test PRIVATE /utf-8)
    target_compile_options(powerpoint-import-contract-test PRIVATE /utf-8)
    target_compile_options(pdf-import-contract-test PRIVATE /utf-8)
  endif()
endif()

if(WIN32)
  add_custom_command(TARGET vtr-pon POST_BUILD
    COMMAND "$<TARGET_FILE:Qt6::windeployqt>"
      --no-translations
      "$<TARGET_FILE:vtr-pon>"
    COMMENT "Deploying Qt runtime dependencies"
    VERBATIM)
  if(BUILD_TESTING)
    add_custom_command(TARGET image-add-contract-test POST_BUILD
      COMMAND "$<TARGET_FILE:Qt6::windeployqt>"
        --no-translations
        "$<TARGET_FILE:image-add-contract-test>"
      COMMENT "Deploying image contract-test Qt runtime dependencies"
      VERBATIM)
    add_custom_command(TARGET pdf-import-contract-test POST_BUILD
      COMMAND "$<TARGET_FILE:Qt6::windeployqt>"
        --no-translations
        "$<TARGET_FILE:pdf-import-contract-test>"
      COMMENT "Deploying PDF contract-test Qt runtime dependencies"
      VERBATIM)
  endif()
endif()

# The settings UI and all users of the compositor share the startup GPU policy.
if(WIN32)
  add_subdirectory(apps/desktop/virtual_camera)
  add_dependencies(vtr-pon vtr-pon-virtual-camera)
  if(TARGET operator-monitor-isolation-test)
    target_link_libraries(operator-monitor-isolation-test PRIVATE vtrpon-virtual-camera-output)
  endif()
endif()
foreach(gpu_consumer IN ITEMS
    vtr-pon program-playout-transition-test deck-source-session-test
    dual-deck-player-contract-test aux-playback-test playlist-panel-drag-drop-test)
  if(TARGET ${gpu_consumer})
    target_link_libraries(${gpu_consumer} PRIVATE vtrpon-gpu-configuration)
    if(WIN32)
      target_link_libraries(${gpu_consumer} PRIVATE vtrpon-operator-monitor-output vtrpon-virtual-camera-output)
    endif()
  endif()
endforeach()
if(WIN32 AND BUILD_TESTING)
  set_tests_properties(gpu-configuration PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${GSTREAMER_RUNTIME_ROOT}/bin")
endif()

# Shared Windows RAW capture is embedded in each consumer; no external plugin is required.
qt_add_library(vtrpon-wasapi-raw-audio STATIC
  apps/desktop/wasapi_raw_audio_source.cpp
  apps/desktop/wasapi_capture_endpoint.h
  apps/desktop/wasapi_raw_audio_source.h)
target_link_libraries(vtrpon-wasapi-raw-audio PRIVATE
  PkgConfig::GSTREAMER PkgConfig::GSTREAMER_APP PkgConfig::GSTREAMER_AUDIO)
target_compile_definitions(vtrpon-wasapi-raw-audio PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
if(WIN32)
  target_link_libraries(vtrpon-wasapi-raw-audio PRIVATE ole32 uuid)
endif()
if(MSVC)
  target_compile_options(vtrpon-wasapi-raw-audio PRIVATE /utf-8)
endif()
foreach(raw_audio_consumer IN ITEMS
    vtr-pon program-playout-transition-test deck-source-session-test
    dual-deck-player-contract-test playlist-panel-drag-drop-test)
  if(TARGET ${raw_audio_consumer})
    target_link_libraries(${raw_audio_consumer} PRIVATE vtrpon-wasapi-raw-audio)
  endif()
endforeach()

# Native initialization/error/stop checks avoid physical devices by default.
if(WIN32 AND BUILD_TESTING)
  qt_add_executable(wasapi-raw-audio-source-test tests/wasapi_raw_audio_source_test.cpp)
  target_include_directories(wasapi-raw-audio-source-test PRIVATE apps/desktop)
  target_link_libraries(wasapi-raw-audio-source-test PRIVATE
    vtrpon-wasapi-raw-audio PkgConfig::GSTREAMER PkgConfig::GSTREAMER_APP)
  add_test(NAME wasapi-raw-audio-source COMMAND wasapi-raw-audio-source-test)
  set_tests_properties(wasapi-raw-audio-source PROPERTIES TIMEOUT 20)
endif()

# The host bridge contains no vendor headers or libraries. All media consumers
# use the same bounded publication and receive services.
qt_add_library(vtrpon-network-video STATIC
  apps/desktop/network_video.cpp
  apps/desktop/network_video.h
  apps/desktop/network_video_output.cpp
  apps/desktop/network_video_source.h
  apps/desktop/network_video_addon_api.h)
target_link_libraries(vtrpon-network-video PRIVATE
  Qt6::Core PkgConfig::GSTREAMER PkgConfig::GSTREAMER_APP)
target_compile_definitions(vtrpon-network-video PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
if(WIN32)
  target_link_libraries(vtrpon-network-video PRIVATE d3d11)
endif()
if(MSVC)
  target_compile_options(vtrpon-network-video PRIVATE /utf-8)
endif()
foreach(network_video_consumer IN ITEMS
    vtr-pon program-playout-transition-test deck-source-session-test
    dual-deck-player-contract-test aux-playback-test playlist-panel-drag-drop-test
    program-audio-output-test program-audio-bridge-test operator-monitor-isolation-test)
  if(TARGET ${network_video_consumer})
    target_link_libraries(${network_video_consumer} PRIVATE vtrpon-network-video)
  endif()
endforeach()
if(TARGET playlist-panel-drag-drop-test)
  target_sources(playlist-panel-drag-drop-test PRIVATE
    apps/desktop/input_control_panel.cpp apps/desktop/input_control_panel.h)
endif()

# Runtime diagnosis never shares the Controller's vendor/runtime sessions.
qt_add_executable(vtr-pon-addon-runtime-probe apps/desktop/addon_runtime_probe.cpp)
target_link_libraries(vtr-pon-addon-runtime-probe PRIVATE Qt6::Core)
if(MSVC)
  target_compile_options(vtr-pon-addon-runtime-probe PRIVATE /utf-8)
endif()
add_dependencies(vtr-pon vtr-pon-addon-runtime-probe)
add_dependencies(vtr-pon vtr-pon-addon-manager)
if(WIN32)
  add_custom_command(TARGET vtr-pon POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
      "${VTRPON_ZLIB_RUNTIME}" "$<TARGET_FILE_DIR:vtr-pon>")
endif()
if(TARGET playlist-panel-drag-drop-test)
  add_dependencies(playlist-panel-drag-drop-test vtr-pon-addon-runtime-probe)
endif()
install(TARGETS vtr-pon-addon-runtime-probe RUNTIME DESTINATION . COMPONENT Runtime)

# The Controller and PCM bridge deliberately have no dependency on the VST SDK.
qt_add_library(vtrpon-vst-bridge STATIC
  apps/desktop/vst_service.cpp
  apps/desktop/vst_service.h
  apps/desktop/vst_scan.h
  apps/desktop/vst_protocol.h)
target_include_directories(vtrpon-vst-bridge PUBLIC apps/desktop)
target_link_libraries(vtrpon-vst-bridge PUBLIC Qt6::Core)
target_compile_definitions(vtrpon-vst-bridge PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
qt_add_library(vtrpon-vst-ui STATIC apps/desktop/vst_editor.cpp apps/desktop/vst_editor.h)
target_link_libraries(vtrpon-vst-ui PUBLIC vtrpon-vst-bridge Qt6::Widgets)
if(MSVC)
  target_compile_options(vtrpon-vst-bridge PRIVATE /utf-8)
  target_compile_options(vtrpon-vst-ui PRIVATE /utf-8)
endif()
foreach(vst_consumer IN ITEMS
    vtr-pon program-playout-transition-test deck-source-session-test
    dual-deck-player-contract-test aux-playback-test playlist-panel-drag-drop-test
    program-audio-output-test program-audio-bridge-test edit-panel-duration-control-test)
  if(TARGET ${vst_consumer})
    target_link_libraries(${vst_consumer} PRIVATE vtrpon-vst-ui)
  endif()
endforeach()

# Separately distributed DeckLink media DLL: no vendor SDK in the host build.
qt_add_library(vtrpon-decklink STATIC apps/desktop/decklink.cpp apps/desktop/decklink.h
  apps/desktop/decklink_output.cpp apps/desktop/decklink_addon_api.h apps/desktop/decklink_media_api.h
  apps/desktop/decklink_output_api.h)
target_include_directories(vtrpon-decklink PUBLIC apps/desktop)
target_link_libraries(vtrpon-decklink PRIVATE Qt6::Core Qt6::Gui PkgConfig::GSTREAMER PkgConfig::GSTREAMER_APP)
target_compile_definitions(vtrpon-decklink PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
if(WIN32)
  target_link_libraries(vtrpon-decklink PRIVATE d3d11)
endif()
if(MSVC)
  target_compile_options(vtrpon-decklink PRIVATE /utf-8)
endif()
foreach(decklink_consumer IN ITEMS vtr-pon program-playout-transition-test deck-source-session-test
    dual-deck-player-contract-test aux-playback-test playlist-panel-drag-drop-test
    program-audio-output-test program-audio-bridge-test operator-monitor-isolation-test edit-panel-duration-control-test)
  if(TARGET ${decklink_consumer})
    target_link_libraries(${decklink_consumer} PRIVATE vtrpon-decklink)
  endif()
endforeach()

if(WIN32 AND BUILD_TESTING)
  # The fixture is SDK-free and built solely for this test, never installed.
  add_library(decklink-host-test-addon SHARED
    tests/decklink/mock_addon.cpp)
  target_include_directories(decklink-host-test-addon PRIVATE
    apps/desktop)
  target_compile_definitions(decklink-host-test-addon PRIVATE VTRPON_DECKLINK_ADDON_BUILD NOMINMAX _CRT_SECURE_NO_WARNINGS)
  set_target_properties(decklink-host-test-addon PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/decklink-test-only")
  qt_add_executable(decklink-test tests/decklink_test.cpp)
  target_link_libraries(decklink-test PRIVATE vtrpon-decklink Qt6::Core Qt6::Gui
    PkgConfig::GSTREAMER PkgConfig::GSTREAMER_APP d3d11)
  target_compile_definitions(decklink-test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  add_dependencies(decklink-test decklink-host-test-addon)
  add_test(NAME decklink-media-host COMMAND decklink-test "$<TARGET_FILE:decklink-host-test-addon>")
  set_tests_properties(decklink-media-host PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)
endif()

if(WIN32 AND CMAKE_CONFIGURATION_TYPES)
  # Keep the established development launch path stable with VS/multi-config
  # builds. Only host-owned runtime files are copied; add-ons stay separate.
  add_custom_command(TARGET vtr-pon POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
      "$<TARGET_FILE:vtr-pon>" "${CMAKE_BINARY_DIR}/vtr-pon.exe"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
      "$<TARGET_FILE:vtr-pon-addon-manager>" "${CMAKE_BINARY_DIR}/vtr-pon-addon-manager.exe"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
      "$<TARGET_FILE:vtr-pon-addon-runtime-probe>" "${CMAKE_BINARY_DIR}/vtr-pon-addon-runtime-probe.exe"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
      "$<TARGET_FILE:vtr-pon-pfl-output>" "${CMAKE_BINARY_DIR}/vtr-pon-pfl-output.exe"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
      "$<TARGET_FILE:vtr-pon-operator-monitor>" "${CMAKE_BINARY_DIR}/vtr-pon-operator-monitor.exe"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
      "$<TARGET_FILE:vtr-pon-virtual-camera>" "${CMAKE_BINARY_DIR}/vtr-pon-virtual-camera.dll"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
      "${VTRPON_ZLIB_RUNTIME}" "${CMAKE_BINARY_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
      "$<TARGET_FILE_DIR:vtr-pon>/assets/testpatterns" "${CMAKE_BINARY_DIR}/assets/testpatterns"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
      "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE" "${CMAKE_BINARY_DIR}/LICENSE"
    COMMAND "$<TARGET_FILE:Qt6::windeployqt>"
      --no-translations --dir "${CMAKE_BINARY_DIR}" "$<TARGET_FILE:vtr-pon>"
    COMMENT "Updating the fixed development runtime in the build root"
    VERBATIM)
endif()
