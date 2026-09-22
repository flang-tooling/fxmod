cmake_minimum_required(VERSION 3.16)
project(fxmod
  VERSION 0.1.0
  DESCRIPTION "Parse gfortran / Flang / (future) ifx Fortran module files and regenerate compilable Fortran source"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

find_package(ZLIB REQUIRED)

# On Linux, statically link libstdc++/libgcc into every executable. Without
# this, a binary built with whatever compiler happens to be first on PATH
# (which may not be the system's default one -- e.g. a self-built GCC in
# /usr/local ahead of the distro's /usr/bin one) can end up requiring a
# GLIBCXX symbol version newer than the system's own libstdc++.so.6
# provides, failing at *run* time with "version `GLIBCXX_3.4.NN' not
# found" on a machine that built it just fine. Static linking makes the
# binary self-contained instead of depending on which libstdc++ happens to
# be on the loader's path. Not needed on macOS, which doesn't hit this
# (Apple's libc++ is versioned differently) and whose linker treats these
# flags differently.
set(FXMOD_PORTABLE_LINK_OPTIONS "")
if(UNIX AND NOT APPLE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  set(FXMOD_PORTABLE_LINK_OPTIONS -static-libgcc -static-libstdc++)
endif()

add_library(fxmod
  src/sexpr.cpp
  src/detect.cpp
  src/gfortran/module.cpp
  src/gfortran/symbols.cpp
  src/gfortran/fortran_emitter.cpp
  src/flang/module.cpp
  src/ifx/module.cpp
  src/ifx/fortran_emitter.cpp
  src/module.cpp
  src/api.cpp
)
add_library(fxmod::fxmod ALIAS fxmod)

target_include_directories(fxmod
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(fxmod PRIVATE ZLIB::ZLIB)

target_compile_options(fxmod PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra>
)

add_executable(fxmod-cli tools/fxmod-cli.cpp)
target_link_libraries(fxmod-cli PRIVATE fxmod::fxmod)
target_link_options(fxmod-cli PRIVATE ${FXMOD_PORTABLE_LINK_OPTIONS})

option(FXMOD_BUILD_TESTS "Build fxmod tests" ON)
if(FXMOD_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
