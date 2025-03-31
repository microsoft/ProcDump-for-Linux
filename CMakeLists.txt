#
#
#    ProcDump-for-Linux
#
#    Copyright (c) Microsoft Corporation
#
#    All rights reserved.
#
#    MIT License
#
#    Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the ""Software""), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:
#
#    The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.
#
#    THE SOFTWARE IS PROVIDED *AS IS*, WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
#
#

#################################################################################
#
# CMakeLists.txt
#
# Build script
#
#################################################################################

cmake_minimum_required(VERSION 3.10)
cmake_policy(SET CMP0048 NEW)

#
# set the project name - version is MAJOR.MINOR.PATCH.RELEASE - releases start at 1
#
if (DEFINED ENV{VERSION})
  project(ProcDumpForLinux VERSION $ENV{VERSION})
else()
  project(ProcDumpForLinux VERSION 0.0.0)
endif()

set(PROJECT_VERSION_TWEAK 0)
file(READ "dist/changelog" CHANGE_LOG)

#
# package name
#
set(PACKAGE_NAME "procdump")

#
# MAN page
#
set(PROCDUMP_COMPRESS_MAN "procdump.1.gz")

add_custom_target(procDumpManPageCompress ALL
                  DEPENDS ${PROJECT_BINARY_DIR}/${PROCDUMP_COMPRESS_MAN}
                  )

if(${CMAKE_SYSTEM_NAME} STREQUAL "Linux")
  add_custom_command(OUTPUT ${PROJECT_BINARY_DIR}/${PROCDUMP_COMPRESS_MAN}
                    COMMAND gzip -9n -f -c "${CMAKE_SOURCE_DIR}/procdump.1" > ${PROJECT_BINARY_DIR}/${PROCDUMP_COMPRESS_MAN}
                    COMMENT "Compressing ProcDump man page"
                    DEPENDS "${CMAKE_SOURCE_DIR}/procdump.1"
                    )
else()
  add_custom_command(OUTPUT ${PROJECT_BINARY_DIR}/${PROCDUMP_COMPRESS_MAN}
                    COMMAND gzip -9n -f -c "${CMAKE_SOURCE_DIR}/procdump_mac.1" > ${PROJECT_BINARY_DIR}/${PROCDUMP_COMPRESS_MAN}
                    COMMENT "Compressing ProcDump man page"
                    DEPENDS "${CMAKE_SOURCE_DIR}/procdump.1"
                    )
endif()

#
# Change log
#
set(PROCDUMP_COMPRESS_CHANGELOG "changelog.gz")

add_custom_target(procDumpChangelogCompress ALL
                  DEPENDS ${PROJECT_BINARY_DIR}/${PROCDUMP_COMPRESS_CHANGELOG}
                  )

add_custom_command(OUTPUT ${PROJECT_BINARY_DIR}/${PROCDUMP_COMPRESS_CHANGELOG}
                   COMMAND cp "${CMAKE_SOURCE_DIR}/dist/changelog" ${PROJECT_BINARY_DIR} && gzip -f -c "${CMAKE_SOURCE_DIR}/dist/changelog" > ${PROJECT_BINARY_DIR}/${PROCDUMP_COMPRESS_CHANGELOG}
                   COMMENT "Compressing changelog"
                   DEPENDS "${CMAKE_SOURCE_DIR}/dist/changelog"
                  )

#
# Paths
#
set(profiler_INC ${CMAKE_SOURCE_DIR}/profiler/inc)
set(profiler_SRC ${CMAKE_SOURCE_DIR}/profiler/src)
set(procdump_INC ${CMAKE_SOURCE_DIR}/include)
set(procdump_SRC ${CMAKE_SOURCE_DIR}/src)
set(procdump_Test ${CMAKE_SOURCE_DIR}/tests/integration)
set(LD "/usr/bin/ld")
set(libbpf_SOURCE_DIR ${CMAKE_BINARY_DIR}/libbpf/src/libbpf)
set(procdump_ebpf_SOURCE_DIR ${CMAKE_SOURCE_DIR}/ebpf)
set(sym_SOURCE_DIR ${CMAKE_SOURCE_DIR}/sym)

#
# Compiler
#
set(CMAKE_CXX_COMPILER "clang++")

#
# Make procdump profiler
#
if(${CMAKE_SYSTEM_NAME} STREQUAL "Linux")
  message(STATUS "Building for Linux")
  # Figure out which architecture we are building for
  if(CMAKE_SYSTEM_PROCESSOR STREQUAL x86_64 OR CMAKE_SYSTEM_PROCESSOR STREQUAL amd64)
    set(CLRHOSTDEF -DHOST_AMD64 -DHOST_64BIT)
    set(ARCHITECTURE "amd64")
  elseif(CMAKE_SYSTEM_PROCESSOR STREQUAL x86 OR CMAKE_SYSTEM_PROCESSOR STREQUAL i686)
    set(CLRHOSTDEF -DHOST_X86)
  elseif(CMAKE_SYSTEM_PROCESSOR STREQUAL armv6 OR CMAKE_SYSTEM_PROCESSOR STREQUAL armv6l)
    set(CLRHOSTDEF -DHOST_ARM -DHOST_ARMV6)
  elseif(CMAKE_SYSTEM_PROCESSOR STREQUAL arm OR CMAKE_SYSTEM_PROCESSOR STREQUAL armv7-a OR CMAKE_SYSTEM_PROCESSOR STREQUAL armv7l OR CMAKE_SYSTEM_PROCESSOR STREQUAL armv8l)
    set(CLRHOSTDEF -DHOST_ARM)
  elseif(CMAKE_SYSTEM_PROCESSOR STREQUAL aarch64 OR CMAKE_SYSTEM_PROCESSOR STREQUAL arm64)
    set(CLRHOSTDEF -DHOST_ARM64 -DHOST_64BIT)
    set(ARCHITECTURE "arm64")
  elseif(CMAKE_SYSTEM_PROCESSOR STREQUAL loongarch64)
    set(CLRHOSTDEF -DHOST_LOONGARCH64 -DHOST_64BIT)
  elseif(CMAKE_SYSTEM_PROCESSOR STREQUAL riscv64)
    set(CLRHOSTDEF -DHOST_RISCV64 -DHOST_64BIT)
  elseif(CMAKE_SYSTEM_PROCESSOR STREQUAL s390x)
    set(CLRHOSTDEF -DHOST_S390X -DHOST_64BIT -DBIGENDIAN)
  elseif(CMAKE_SYSTEM_PROCESSOR STREQUAL mips64)
    set(CLRHOSTDEF -DHOST_MIPS64 -DHOST_64BIT=1)
  elseif(CMAKE_SYSTEM_PROCESSOR STREQUAL ppc64le)
    set(CLRHOSTDEF -DHOST_POWERPC64 -DHOST_64BIT)
  else()
    message(FATAL_ERROR "'${CMAKE_SYSTEM_PROCESSOR}' is an unsupported architecture.")
  endif()

  add_library(ProcDumpProfiler SHARED
              ${profiler_SRC}/ClassFactory.cpp
              ${profiler_SRC}/ProcDumpProfiler.cpp
              ${profiler_SRC}/dllmain.cpp
              ${profiler_SRC}/corprof_i.cpp
              ${profiler_SRC}/easylogging++.cc
              )

  target_compile_options(ProcDumpProfiler PRIVATE -DELPP_NO_DEFAULT_LOG_FILE -DELPP_THREAD_SAFE -g -pthread -Wno-pragma-pack -Wno-pointer-arith -Wno-conversion-null -Wno-write-strings -Wno-format-security -fPIC -fms-extensions ${CLRHOSTDEF} -DPAL_STDCPP_COMPAT -DPLATFORM_UNIX -std=c++11)
  set_target_properties(ProcDumpProfiler PROPERTIES PREFIX "")

  target_include_directories(ProcDumpProfiler PUBLIC
                            "${profiler_INC}"
                            "${procdump_INC}"
                            /usr/include
                            )

  if(NOT APPLE AND CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
    target_include_directories(ProcDumpProfiler PUBLIC
                               /usr/include/aarch64-linux-gnu
                              )
  endif()

  add_custom_command(OUTPUT ProcDumpProfiler.o
                    COMMAND "${LD}" -r -b binary -o "${PROJECT_BINARY_DIR}/ProcDumpProfiler.o" ProcDumpProfiler.so
                    COMMENT "Packing ProcDumpProfiler.so into ProcDumpProfiler.o"
                    DEPENDS ProcDumpProfiler
                    )
endif()

#
# Configure files
#
configure_file(${procdump_INC}/ProcDumpVersion.h.in ${PROJECT_BINARY_DIR}/ProcDumpVersion.h)
if(${CMAKE_SYSTEM_NAME} STREQUAL "Linux")
  configure_file(dist/DEBIAN.in/control.in DEBIANcontrol)
  configure_file(dist/SPECS.in/spec.in SPECS.spec)
endif()

#
# Make ProcDump
#
if(APPLE)
    # Create universal binary
    set(CMAKE_OSX_ARCHITECTURES "x86_64;arm64")
endif()

if(${CMAKE_SYSTEM_NAME} STREQUAL "Linux")
  message(STATUS "Building for Linux")
  add_executable(procdump
                ${procdump_SRC}/CoreDumpWriter.cpp
                ${procdump_SRC}/DotnetHelpers.cpp
                ${procdump_SRC}/Events.cpp
                ${procdump_SRC}/GenHelpers.cpp
                ${procdump_SRC}/Handle.cpp
                ${procdump_SRC}/Logging.cpp
                ${procdump_SRC}/Monitor.cpp
                ${procdump_SRC}/Procdump.cpp
                ${procdump_SRC}/ProcDumpConfiguration.cpp
                ${procdump_SRC}/Process.cpp
                ${procdump_SRC}/ProfilerHelpers.cpp
                ${procdump_SRC}/Restrack.cpp
                ${sym_SOURCE_DIR}/bcc_proc.cpp
                ${sym_SOURCE_DIR}/bcc_syms.cc
                ${sym_SOURCE_DIR}/bcc_elf.cpp
                ${sym_SOURCE_DIR}/bcc_perf_map.cpp
                ${sym_SOURCE_DIR}/bcc_zip.cpp
                ${PROJECT_BINARY_DIR}/ProcDumpProfiler.o
                )
else()
  add_executable(procdump
                ${procdump_SRC}/CoreDumpWriter.cpp
                #${procdump_SRC}/DotnetHelpers.cpp
                ${procdump_SRC}/Events.cpp
                ${procdump_SRC}/GenHelpers.cpp
                ${procdump_SRC}/Handle.cpp
                ${procdump_SRC}/Logging.cpp
                ${procdump_SRC}/Monitor.cpp
                ${procdump_SRC}/Procdump.cpp
                ${procdump_SRC}/ProcDumpConfiguration.cpp
                ${procdump_SRC}/Process.cpp
                #${procdump_SRC}/ProfilerHelpers.cpp
                #${procdump_SRC}/Restrack.cpp
                #${sym_SOURCE_DIR}/bcc_proc.cpp
                #${sym_SOURCE_DIR}/bcc_syms.cc
                #${sym_SOURCE_DIR}/bcc_elf.cpp
                #${sym_SOURCE_DIR}/bcc_perf_map.cpp
                #${sym_SOURCE_DIR}/bcc_zip.cpp
                #${PROJECT_BINARY_DIR}/ProcDumpProfiler.o
                )
endif()

target_compile_options(procdump PRIVATE -g -pthread -fstack-protector-all -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=2 -Werror -D_GNU_SOURCE -std=c++11 -O2)

target_include_directories(procdump PUBLIC
                           ${procdump_INC}
                           ${PROJECT_BINARY_DIR}
                           /usr/include
                           ${sym_SOURCE_DIR}
                           ${procdump_ebpf_SOURCE_DIR}
                          )

if(NOT APPLE AND CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
  message(STATUS "Adding include path for aarch64: /usr/include/aarch64-linux-gnu")
  target_include_directories(procdump PUBLIC
                             /usr/include/aarch64-linux-gnu
                            )
endif()

if(${CMAKE_SYSTEM_NAME} STREQUAL "Linux")
  add_dependencies(procdump libbpf procdump_ebpf)
  target_link_libraries(procdump ${libbpf_SOURCE_DIR}/src/libbpf.a elf z pthread)
else()
  target_link_libraries(procdump z pthread)
endif()

#
# Copy integration test directory
#
add_custom_target(copy_integration_test_dir ALL
                  COMMAND ${CMAKE_COMMAND} -E copy_directory
                  ${CMAKE_SOURCE_DIR}/tests/integration ${CMAKE_BINARY_DIR}/tests/integration
                  COMMENT "Copying the tests/integration directory to the build directory"
                 )

#
# Make test application
#
add_executable(ProcDumpTestApplication
               ${procdump_Test}/ProcDumpTestApplication.c
              )

target_compile_options(ProcDumpTestApplication PRIVATE -g -pthread -std=gnu99 -fstack-protector-all -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=2 -D_GNU_SOURCE -Werror -O2)

target_include_directories(ProcDumpTestApplication PUBLIC
                           /usr/include
                          )

target_link_libraries(ProcDumpTestApplication pthread)

#
# Make package(s)
#
if(${CMAKE_SYSTEM_NAME} STREQUAL "Linux")
  message("Architecture: ${ARCHITECTURE}")
  add_custom_target(deb
                  COMMAND "${CMAKE_SOURCE_DIR}/makePackages.sh" "${CMAKE_SOURCE_DIR}" "${PROJECT_BINARY_DIR}" "${PACKAGE_NAME}" "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${PROJECT_VERSION_PATCH}" "0" "deb" "${ARCHITECTURE}"
                  DEPENDS "${CMAKE_SOURCE_DIR}/dist" "${PROJECT_BINARY_DIR}/procdump"
                  )

  add_custom_target(rpm
                  COMMAND "${CMAKE_SOURCE_DIR}/makePackages.sh" "${CMAKE_SOURCE_DIR}" "${PROJECT_BINARY_DIR}" "${PACKAGE_NAME}" "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${PROJECT_VERSION_PATCH}" "0" "rpm"
                  DEPENDS "${CMAKE_SOURCE_DIR}/dist" "${PROJECT_BINARY_DIR}/procdump"
                  )
else()
  add_custom_target(brew
                  COMMAND "${CMAKE_SOURCE_DIR}/makePackages.sh" "${CMAKE_SOURCE_DIR}" "${PROJECT_BINARY_DIR}" "${PACKAGE_NAME}" "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${PROJECT_VERSION_PATCH}" "0" "brew"
                  DEPENDS "${CMAKE_SOURCE_DIR}/dist" "${PROJECT_BINARY_DIR}/procdump"
                  )
endif()

#
# Make ProcDump eBPF program
#
if(${CMAKE_SYSTEM_NAME} STREQUAL "Linux")
  # Fetch libbpf
  include(ExternalProject)

  ExternalProject_Add(libbpf
      GIT_REPOSITORY https://github.com/libbpf/libbpf.git
      GIT_TAG v1.2.2
      PREFIX ./libbpf
      CONFIGURE_COMMAND ""
      BUILD_COMMAND cd ../libbpf/src && bash -c "CFLAGS=\"-g -O2 -Werror -Wall -fPIC\" make"
      INSTALL_COMMAND ""
      )

  # set binaries and options for clang and llc
  set(CLANG "clang")
  set(LLC "llc")
  set(CLANG_OPTIONS -Wno-unused-value
                    -Wno-pointer-sign
                    -Wno-compare-distinct-pointer-types
                    -Wno-gnu-variable-sized-type-not-at-end
                    -Wno-address-of-packed-member
                    -Wno-tautological-compare
                    -Wno-unknown-warning-option
                    -g
                    )
  set(CLANG_DEFINES -D __KERNEL__
                    -D __BPF_TRACING__
                    -D __linux__
                    )

  if(NOT APPLE AND CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
    list(APPEND CLANG_DEFINES -D __TARGET_ARCH_arm64)
  endif()

  if(NOT APPLE AND CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
    list(APPEND CLANG_DEFINES -D __TARGET_ARCH_x86)
  endif()

  if (DEBUG_K)
      message("Using DEBUG_K Option...")
      list(APPEND CLANG_DEFINES -DDEBUG_K)
  endif()

  set(CLANG_INCLUDES
                    -I "/usr/include"
                    #-I "/usr/include/x86_64-linux-gnu"
                    -I "${CMAKE_SOURCE_DIR}"
                    -I "${CMAKE_BINARY_DIR}"
                    -I "${libbpf_SOURCE_DIR}/src"
                    )

  if(NOT APPLE AND CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
  list(APPEND CLANG_INCLUDES -I "/usr/include/aarch64-linux-gnu")
  endif()

  if(NOT APPLE AND CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
    list(APPEND CLANG_INCLUDES -I "/usr/include/x86_64-linux-gnu")
  endif()

  add_custom_target(procdump_ebpf
                    DEPENDS procdump_ebpf.o
                  )

  add_dependencies(procdump_ebpf libbpf)

  add_custom_command(OUTPUT procdump_ebpf.o
                    COMMAND "${CLANG}" -nostdinc -isystem `gcc -print-file-name=include` ${CLANG_INCLUDES} ${CLANG_DEFINES} -O2 ${CLANG_OPTIONS} -target bpf -fno-stack-protector -c "${procdump_ebpf_SOURCE_DIR}/procdump_ebpf.c" -o "procdump_ebpf.o" && bpftool gen object procdump.ebpf.o procdump_ebpf.o && bpftool gen skeleton "procdump.ebpf.o" name "procdump_ebpf" > "procdump_ebpf.skel.h"
                    COMMENT "Building EBPF object procdump_ebpf.o"
                    DEPENDS ${procdump_ebpf_SOURCE_DIR}/procdump_ebpf.c
                    )

  set_directory_properties(PROPERTIES ADDITIONAL_MAKE_CLEAN_FILES procdump.ebpf.o)
endif()
