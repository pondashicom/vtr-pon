cmake_minimum_required(VERSION 3.24)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(VTRPON_RolandProtocol LANGUAGES CXX)
  enable_testing()
endif()
find_package(Qt6 REQUIRED COMPONENTS Core Network LinguistTools)
get_filename_component(VTRPON_ROLAND_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

# Generate metadata from the actual SwitcherDevice declaration only. This target
# exercises the real adapters without linking the media engine/Controller/GPU.
# Do not provide a second hand-maintained declaration or fake adapter implementation.
set(_device_header "${VTRPON_ROLAND_ROOT}/apps/desktop/switcher_control.h")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_device_header}")
file(READ "${_device_header}" _device_source)
string(FIND "${_device_source}" "class SwitcherDevice :" _device_start)
string(FIND "${_device_source}" "class SwitcherController final" _device_end)
if(_device_start LESS 0 OR _device_end LESS _device_start)
  message(FATAL_ERROR "Could not locate SwitcherDevice declaration")
endif()
math(EXPR _device_length "${_device_end} - ${_device_start}")
string(SUBSTRING "${_device_source}" ${_device_start} ${_device_length} _device_declaration)
set(_device_meta "${CMAKE_CURRENT_BINARY_DIR}/switcher_device_metadata.h")
file(WRITE "${_device_meta}"
  "#pragma once\n#include <QObject>\n#include <QString>\nnamespace vtrpon {\n${_device_declaration}\n}\n")

qt6_wrap_cpp(_device_moc "${_device_meta}" OPTIONS "-f${_device_header}")

add_executable(vtrpon-roland-protocol-test
  "${CMAKE_CURRENT_LIST_DIR}/main.cpp"
  "${VTRPON_ROLAND_ROOT}/tests/roland_switcher_trial_test.cpp"
  "${VTRPON_ROLAND_ROOT}/tests/roland_vr50hd_lan_switcher_test.cpp"
  "${VTRPON_ROLAND_ROOT}/tests/roland_winmm_output_test.cpp"
  "${VTRPON_ROLAND_ROOT}/apps/desktop/roland_switcher_profiles.cpp"
  "${VTRPON_ROLAND_ROOT}/apps/desktop/roland_remote_switcher.cpp"
  "${VTRPON_ROLAND_ROOT}/apps/desktop/roland_vr50hd_lan_switcher.cpp"
  "${VTRPON_ROLAND_ROOT}/apps/desktop/roland_v8hd_midi_switcher.cpp"
  "${VTRPON_ROLAND_ROOT}/apps/desktop/roland_v8hd_midi_switcher.h"
  "${VTRPON_ROLAND_ROOT}/apps/desktop/switcher_check_session.cpp"
  ${_device_moc})
set_target_properties(vtrpon-roland-protocol-test PROPERTIES AUTOMOC ON)
target_compile_features(vtrpon-roland-protocol-test PRIVATE cxx_std_20)
target_include_directories(vtrpon-roland-protocol-test PRIVATE
  "${VTRPON_ROLAND_ROOT}/apps/desktop")
target_link_libraries(vtrpon-roland-protocol-test PRIVATE Qt6::Core Qt6::Network)
if(WIN32)
  target_compile_definitions(vtrpon-roland-protocol-test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_link_libraries(vtrpon-roland-protocol-test PRIVATE winmm)
endif()
if(MSVC)
  target_compile_options(vtrpon-roland-protocol-test PRIVATE /utf-8)
endif()
add_test(NAME roland-protocol COMMAND vtrpon-roland-protocol-test --trial)
add_test(NAME roland-vr50-protocol COMMAND vtrpon-roland-protocol-test --vr50)
add_test(NAME roland-winmm-output COMMAND vtrpon-roland-protocol-test --winmm)
set_tests_properties(roland-protocol roland-vr50-protocol PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)
set_tests_properties(roland-winmm-output PROPERTIES TIMEOUT 15 SKIP_RETURN_CODE 77)

# Exercise the actual Windows language catalog on any Qt host as well.
set(_roland_catalogs)
foreach(_language en ja)
  set(_catalog "${CMAKE_CURRENT_BINARY_DIR}/roland_${_language}.qm")
  set(_ts "${VTRPON_ROLAND_ROOT}/apps/desktop/i18n/vtrpon_${_language}.ts")
  add_custom_command(OUTPUT "${_catalog}"
    COMMAND Qt6::lrelease -idbased "${_ts}" -qm "${_catalog}"
    DEPENDS "${_ts}" VERBATIM)
  list(APPEND _roland_catalogs "${_catalog}")
endforeach()
add_custom_target(vtrpon-roland-translations DEPENDS ${_roland_catalogs})
add_executable(vtrpon-roland-language-test
  "${VTRPON_ROLAND_ROOT}/tests/language_catalog_test.cpp"
  "${VTRPON_ROLAND_ROOT}/apps/desktop/language_text.cpp")
target_compile_features(vtrpon-roland-language-test PRIVATE cxx_std_20)
target_include_directories(vtrpon-roland-language-test PRIVATE "${VTRPON_ROLAND_ROOT}/apps/desktop")
target_link_libraries(vtrpon-roland-language-test PRIVATE Qt6::Core)
add_dependencies(vtrpon-roland-language-test vtrpon-roland-translations)
if(MSVC)
  target_compile_options(vtrpon-roland-language-test PRIVATE /utf-8)
endif()
add_test(NAME roland-language-catalog COMMAND vtrpon-roland-language-test ${_roland_catalogs})
set_tests_properties(roland-language-catalog PROPERTIES TIMEOUT 15)
