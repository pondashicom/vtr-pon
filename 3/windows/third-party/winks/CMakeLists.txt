add_library(gstvtrponks MODULE
  gstksclock.c gstksvideodevice.c gstksvideosrc.c kshelpers.c ksvideohelpers.c)
set_target_properties(gstvtrponks PROPERTIES
  PREFIX "" AUTOMOC OFF AUTOUIC OFF
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
target_link_libraries(gstvtrponks PRIVATE
  PkgConfig::GSTREAMER ksuser uuid strmiids dxguid setupapi ole32)
target_compile_definitions(gstvtrponks PRIVATE
  PACKAGE="vtrponks" VERSION="1.28.5" GST_PACKAGE_NAME="VTR-PON"
  GST_PACKAGE_ORIGIN="https://gstreamer.freedesktop.org")
if(MSVC)
  target_compile_options(gstvtrponks PRIVATE /utf-8)
endif()
