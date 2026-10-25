cmake_minimum_required(VERSION 3.25)
project(vtr_pon_atem_addon VERSION 0.3.0 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8 OR (NOT WIN32 AND NOT APPLE))
  message(FATAL_ERROR "This add-on supports Windows x64 and macOS 64-bit.")
endif()
set(VTRPON_ATEM_SDK_DIR "" CACHE PATH "ATEM SDK platform directory (Windows or Mac OS X)")
set(_sdk_include "${VTRPON_ATEM_SDK_DIR}/include")
add_library(vtr-pon-atem-addon SHARED src/vtrpon_atem_switcher_addon.cpp)
target_include_directories(vtr-pon-atem-addon PRIVATE include)
target_compile_definitions(vtr-pon-atem-addon PRIVATE VTRPON_SWITCHER_ADDON_BUILD=1)
if(APPLE)
  if(NOT EXISTS "${_sdk_include}/BMDSwitcherAPI.h" OR
     NOT EXISTS "${_sdk_include}/BMDSwitcherAPIDispatch.cpp")
    message(FATAL_ERROR "Set VTRPON_ATEM_SDK_DIR to the ATEM SDK Mac OS X directory.")
  endif()
  # Compile the official loader in place; never copy SDK sources or the vendor
  # Runtime into this repository or the add-on payload. It retains its CFBundle
  # for process lifetime, including asynchronous USB teardown after Release().
  target_sources(vtr-pon-atem-addon PRIVATE "${_sdk_include}/BMDSwitcherAPIDispatch.cpp")
  target_include_directories(vtr-pon-atem-addon PRIVATE "${_sdk_include}")
  target_link_libraries(vtr-pon-atem-addon PRIVATE "-framework CoreFoundation")
  set_target_properties(vtr-pon-atem-addon PROPERTIES PREFIX ""
    CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN YES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/switcher-addons")
else()
  if(NOT EXISTS "${_sdk_include}/BMDSwitcherAPI.idl")
    message(FATAL_ERROR "Set VTRPON_ATEM_SDK_DIR to the ATEM SDK Windows directory.")
  endif()
  find_program(MIDL_EXECUTABLE midl REQUIRED)
  set(_generated "${CMAKE_CURRENT_BINARY_DIR}/generated")
  file(GLOB _idl_dependencies CONFIGURE_DEPENDS
    "${_sdk_include}/*.idl" "${_sdk_include}/Legacy/*.idl")
  add_custom_command(
    OUTPUT "${_generated}/BMDSwitcherAPI.h"
           "${_generated}/BMDSwitcherAPI_i.c"
           "${_generated}/BMDSwitcherAPI.tlb"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_generated}"
    COMMAND "${MIDL_EXECUTABLE}" /nologo /env x64 /out "${_generated}"
            /h BMDSwitcherAPI.h /iid BMDSwitcherAPI_i.c
            /tlb BMDSwitcherAPI.tlb "${_sdk_include}/BMDSwitcherAPI.idl"
    DEPENDS ${_idl_dependencies}
    WORKING_DIRECTORY "${_sdk_include}"
    VERBATIM)
  set_source_files_properties("${_generated}/BMDSwitcherAPI_i.c" PROPERTIES
    GENERATED TRUE LANGUAGE CXX)
  target_sources(vtr-pon-atem-addon PRIVATE "${_generated}/BMDSwitcherAPI_i.c")
  target_include_directories(vtr-pon-atem-addon PRIVATE include "${_generated}")
  target_compile_definitions(vtr-pon-atem-addon PRIVATE
    NOMINMAX WIN32_LEAN_AND_MEAN VTRPON_SWITCHER_ADDON_BUILD=1)
  target_link_libraries(vtr-pon-atem-addon PRIVATE ole32 oleaut32)
  if(MSVC)
    target_compile_options(vtr-pon-atem-addon PRIVATE /utf-8)
  endif()
  set_target_properties(vtr-pon-atem-addon PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/switcher-addons")
endif()
install(TARGETS vtr-pon-atem-addon RUNTIME DESTINATION switcher-addons LIBRARY DESTINATION switcher-addons)

include(CTest)
if(APPLE AND BUILD_TESTING)
  add_executable(atem-abi-test tests/atem_abi_test.cpp)
  target_include_directories(atem-abi-test PRIVATE include)
  target_link_libraries(atem-abi-test PRIVATE vtr-pon-atem-addon)
  add_test(NAME atem-abi-no-hardware COMMAND atem-abi-test)
endif()
