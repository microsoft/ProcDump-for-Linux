# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

#[=======================================================================[.rst:
FindLibelf
----------

Find libelf headers and library.

Imported Targets
^^^^^^^^^^^^^^^^

``Libelf::Libelf``
  The libelf library, if found.

Result Variables
^^^^^^^^^^^^^^^^

This will define the following variables in your project:

``Libelf_FOUND``
  true if (the requested version of) Libelf is available.
``Libelf_VERSION``
  the version of Libelf.
``Libelf_LIBRARIES``
  the libraries to link against to use Libelf.
``Libelf_INCLUDE_DIRS``
  where to find the Libelf headers.
``Libelf_COMPILE_OPTIONS``
  this should be passed to target_compile_options(), if the
  target is not used for linking

#]=======================================================================]


# Use pkg-config to get the directories and then use these values
# in the FIND_PATH() and FIND_LIBRARY() calls
find_package(PkgConfig QUIET)
pkg_check_modules(PKG_Libelf QUIET libelf)

set(Libelf_COMPILE_OPTIONS ${PKG_Libelf_CFLAGS_OTHER})
set(Libelf_VERSION ${PKG_Libelf_VERSION})

find_path(Libelf_INCLUDE_DIR
  NAMES
    libelf.h
  HINTS
    ${PKG_Libelf_INCLUDE_DIRS}
)
find_library(Libelf_LIBRARY
  NAMES
    elf
  HINTS
    ${PKG_Libelf_LIBRARY_DIRS}
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Libelf
  FOUND_VAR
    Libelf_FOUND
  REQUIRED_VARS
    Libelf_LIBRARY
    Libelf_INCLUDE_DIR
  VERSION_VAR
    Libelf_VERSION
)

if(Libelf_FOUND AND NOT TARGET Libelf::Libelf)
  add_library(Libelf::Libelf UNKNOWN IMPORTED)
  set_target_properties(Libelf::Libelf PROPERTIES
    IMPORTED_LOCATION "${Libelf_LIBRARY}"
    INTERFACE_COMPILE_OPTIONS "${Libelf_COMPILE_OPTIONS}"
    INTERFACE_INCLUDE_DIRECTORIES "${Libelf_INCLUDE_DIR}"
  )
endif()

mark_as_advanced(Libelf_LIBRARY Libelf_INCLUDE_DIR)

if(Libelf_FOUND)
  set(Libelf_LIBRARIES ${Libelf_LIBRARY})
  set(Libelf_INCLUDE_DIRS ${Libelf_INCLUDE_DIR})
endif()
