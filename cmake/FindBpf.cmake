# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

#[=======================================================================[.rst:
FindBpf
----------

Find libbpf headers and library, and bpftool executable.

Imported Targets
^^^^^^^^^^^^^^^^

``Bpf::libbpf``
  The libbpf library, if found.
``Bpf::bpftool``
  The bpftool executable, if found.

Result Variables
^^^^^^^^^^^^^^^^

This will define some or all of the following variables
in your project (depending on components selected):

``Bpf_FOUND``
  true if (the requested version of) libbpf and/or the bpftool
  program are available.
``Bpf_VERSION``
  the version of libbpf.
``Bpf_LIBRARIES``
  the libraries to link against to use libbpf.
``Bpf_INCLUDE_DIRS``
  where to find the libbpf headers.
``Bpf_COMPILE_OPTIONS``
  this should be passed to target_compile_options(), if the
  target is not used for linking
``Bpf_BPFTOOL_EXECUTABLE``
  the location of the bpftool binary.

#]=======================================================================]

if(NOT Bpf_FIND_COMPONENTS)
  set(Bpf_FIND_COMPONENTS libbpf bpftool)
endif()
set(_required)

if(libbpf IN_LIST Bpf_FIND_COMPONENTS)
  # Use pkg-config to get the directories and then use these values
  # in the FIND_PATH() and FIND_LIBRARY() calls
  find_package(PkgConfig QUIET)
  pkg_check_modules(PKG_Bpf QUIET libbpf)

  set(Bpf_COMPILE_OPTIONS ${PKG_Bpf_CFLAGS_OTHER})
  set(Bpf_VERSION ${PKG_Bpf_VERSION})

  find_path(Bpf_INCLUDE_DIR
    NAMES
      bpf/libbpf.h
    HINTS
      ${PKG_Bpf_INCLUDE_DIRS}
  )
  mark_as_advanced(Bpf_INCLUDE_DIR)

  find_library(Bpf_LIBRARY
    NAMES
      bpf
    HINTS
      ${PKG_Bpf_LIBRARY_DIRS}
  )
  mark_as_advanced(Bpf_LIBRARY)

  if(Bpf_INCLUDE_DIR AND Bpf_LIBRARY)
    set(Bpf_libbpf_FOUND TRUE)
  endif()

  list(APPEND _required Bpf_LIBRARY Bpf_INCLUDE_DIR)
  set(_version Bpf_VERSION)

  set(Bpf_LIBRARIES ${Bpf_LIBRARY})
  set(Bpf_INCLUDE_DIRS ${Bpf_INCLUDE_DIR})
endif()

if(bpftool IN_LIST Bpf_FIND_COMPONENTS)
  find_program(Bpf_BPFTOOL_EXECUTABLE
    NAMES
      bpftool
  )
  mark_as_advanced(Bpf_BPFTOOL_EXECUTABLE)

  if(Bpf_BPFTOOL_EXECUTABLE)
    set(Bpf_bpftool_FOUND TRUE)
  endif()
  list(APPEND _required Bpf_BPFTOOL_EXECUTABLE)
  set(Bpf_BPFTOOL_EXECUTABLE ${Bpf_BPFTOOL_EXECUTABLE})
endif()

include(FindPackageHandleStandardArgs)

# From CMake 3.18, HANDLE_COMPONENTS makes REQUIRED_VARS optional
if(CMAKE_VERSION VERSION_LESS "3.18")
  set(_required_vars
    REQUIRED_VARS
      ${_required}
  )
else()
  set(_required_vars)
endif()

if(DEFINED _version)
  set(_version_var
    VERSION_VAR ${_version}
  )
else()
  set(_version_var)
endif()

find_package_handle_standard_args(Bpf
  FOUND_VAR
    Bpf_FOUND
  ${_version_var}
  ${_required_vars}
  HANDLE_COMPONENTS
)

if(Bpf_FOUND AND Bpf_LIBRARY AND NOT TARGET Bpf::libbpf)
    add_library(Bpf::libbpf UNKNOWN IMPORTED)
    set_target_properties(Bpf::libbpf PROPERTIES
      IMPORTED_LOCATION "${Bpf_LIBRARY}"
      INTERFACE_COMPILE_OPTIONS "${Bpf_COMPILE_OPTIONS}"
      INTERFACE_INCLUDE_DIRECTORIES "${Bpf_INCLUDE_DIR}"
    )
endif()

if(Bpf_FOUND AND Bpf_BPFTOOL_EXECUTABLE AND NOT TARGET Bpf::bpftool)
    add_executable(Bpf::bpftool IMPORTED)
    set_target_properties(Bpf::bpftool PROPERTIES
      IMPORTED_LOCATION "${Bpf_BPFTOOL_EXECUTABLE}"
    )
endif()
