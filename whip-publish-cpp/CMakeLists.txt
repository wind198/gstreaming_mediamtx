cmake_minimum_required(VERSION 3.16)
project(whip-publish-cpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GST REQUIRED IMPORTED_TARGET gstreamer-1.0 glib-2.0 gobject-2.0)

add_executable(whip-publish-cpp src/main.cpp)
target_link_libraries(whip-publish-cpp PRIVATE PkgConfig::GST)
