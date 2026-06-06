# Copyright 2014-2015, Max Planck Society.
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without modification,
# are permitted provided that the following conditions are met:
#
# 1. Redistributions of source code must retain the above copyright notice,
#    this list of conditions and the following disclaimer.
#
# 2. Redistributions in binary form must reproduce the above copyright notice,
#    this list of conditions and the following disclaimer in the documentation
#    and/or other materials provided with the distribution.
#
# 3. Neither the name of the copyright holder nor the names of its contributors
#    may be used to endorse or promote products derived from this software without
#    specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
# ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
# WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
# IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
# INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
# BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
# LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
# OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
# OF THE POSSIBILITY OF SUCH DAMAGE.

# File created by Raffi Enficiaud

set(thirdparty_dir ${CMAKE_SOURCE_DIR}/thirdparty)

# the location where the archives will be deflated
set(thirdparties_deflate_directory ${CMAKE_BINARY_DIR}/external_libs_deflate)
if(NOT EXISTS ${thirdparties_deflate_directory})
  file(MAKE_DIRECTORY ${thirdparties_deflate_directory})
endif()

# custom cmake packages, should have lower priority than the ones bundled with cmake
list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake_modules/ )

# these variables allow to specify to which the main project will link and
# to potentially copy some resources to the output directory of the main project.
# They are used by the CMakeLists.txt calling this file.

set(PHD_LINK_EXTERNAL)          # target to which the phd2 main library will link to
set(PHD_COPY_EXTERNAL_ALL)      # copy of a file for any configuration
set(PHD_COPY_EXTERNAL_DBG)      # copy for debug only
set(PHD_COPY_EXTERNAL_REL)      # copy for release only
set(PHD_EXTERNAL_PROJECT_DEPENDENCIES)

# Find system-installed libraries on Linux.
find_package(PkgConfig)

#############################################
#
# external rules common to all platforms
#
#############################################

##############################################
# cfitsio

find_package(CFITSIO REQUIRED)
include_directories(${CFITSIO_INCLUDE_DIR})
list(APPEND PHD_LINK_EXTERNAL ${CFITSIO_LIBRARIES})
message(STATUS "Using system's CFITSIO.")

#############################################
# libcurl
#############################################

find_package(CURL REQUIRED)
message(STATUS "using libcurl ${CURL_LIBRARIES}")
include_directories(${CURL_INCLUDE_DIRS})
list(APPEND PHD_LINK_EXTERNAL ${CURL_LIBRARIES})

#############################################
# the Eigen library, mostly header only

find_package(Eigen3 REQUIRED)
# Eigen 5.x exposes the include path via the Eigen3::Eigen target only
# (EIGEN3_INCLUDE_DIR was dropped). Older configs still set the variable;
# fall back to it so this works on Debian/Pi too.
if(TARGET Eigen3::Eigen)
  get_target_property(EIGEN_SRC Eigen3::Eigen INTERFACE_INCLUDE_DIRECTORIES)
else()
  set(EIGEN_SRC ${EIGEN3_INCLUDE_DIR})
endif()
message(STATUS "Using system's Eigen3 (${EIGEN_SRC}).")

#############################################
# Google test
# https://github.com/google/googletest/tree/main/googletest#incorporating-into-an-existing-cmake-project

if(USE_SYSTEM_GTEST)
  find_package(GTest REQUIRED)
  message(STATUS "Using system's gtest")
else()
  include(FetchContent)
    FetchContent_Declare(
      googletest
      URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.tar.gz
  )
  # For Windows: Prevent overriding the parent project's compiler/linker settings
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
endif()

#############################################
# wxWidgets
#
# The usage is a bit different on all the platforms. For having
#  version >= 3.0, a version of cmake >= 3.0 should be used on Windows
#  on Linux/macOS it works properly this way).

set(wxWidgets_PREFIX_DIRECTORY $ENV{WXWIN} CACHE PATH "wxWidgets directory")

if(wxWidgets_PREFIX_DIRECTORY)
  set(wxWidgets_CONFIG_OPTIONS --prefix=${wxWidgets_PREFIX_DIRECTORY})

  find_program(wxWidgets_CONFIG_EXECUTABLE NAMES "wx-config" PATHS ${wxWidgets_PREFIX_DIRECTORY}/bin NO_DEFAULT_PATH)
  if(NOT wxWidgets_CONFIG_EXECUTABLE)
    message(FATAL_ERROR "Cannot find wxWidgets_CONFIG_EXECUTABLE from the given directory ${wxWidgets_PREFIX_DIRECTORY}")
  endif()
endif()

find_package(wxWidgets 3.2 REQUIRED COMPONENTS aui core base adv html net)
if(NOT wxWidgets_FOUND)
  message(FATAL_ERROR "wxWidgets >= 3.2 cannot be found. Please use wx-config prefix")
endif()

list(APPEND PHD_LINK_EXTERNAL ${wxWidgets_LIBRARIES})



# Camera SDK libraries removed - Alpaca only build


#############################################
#
# Unix/Linux specific dependencies
#
#############################################
if(UNIX AND NOT APPLE)

  # math library is needed, and should be one of the last things to link to here
  find_library(mathlib NAMES m)
  list(APPEND PHD_LINK_EXTERNAL ${mathlib})

endif()

#############################################
#
# gettext and msgmerge tools for documentation/internationalization
#
#############################################

# zip file support integrated in cmake 3.2+
if(WIN32 AND ("${CMAKE_VERSION}" VERSION_GREATER "3.2")
         AND ("${GETTEXT_ROOT}" STREQUAL ""))

  # GETTEXT_ROOT not given from the command line: deflating our own

  set(GETTEXTTOOLS gettext-0.14.4)
  set(GETTEXT_ROOT ${thirdparties_deflate_directory}/${GETTEXTTOOLS})

  # deflate
  if(NOT EXISTS ${GETTEXT_ROOT})

    message(STATUS "Deflating gettexttools from thirdparties to ${GETTEXT_ROOT}")
    # create directory
    if(NOT EXISTS ${GETTEXT_ROOT})
      file(MAKE_DIRECTORY ${GETTEXT_ROOT})
    endif()

    # untar the dependency
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E tar xzf ${thirdparty_dir}/${GETTEXTTOOLS}-bin.zip
      WORKING_DIRECTORY ${GETTEXT_ROOT})
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E tar xzf ${thirdparty_dir}/${GETTEXTTOOLS}-dep.zip
      WORKING_DIRECTORY ${GETTEXT_ROOT})
  endif()

endif()

set(GETTEXT_FINDPROGRAM_OPTIONS)
if(NOT ("${GETTEXT_ROOT}" STREQUAL ""))
  set(GETTEXT_FINDPROGRAM_OPTIONS
      PATHS ${GETTEXT_ROOT}
               PATH_SUFFIXES bin
               DOC "gettext program deflated from the thirdparties"
               NO_DEFAULT_PATH)
endif()

find_program(XGETTEXT
             NAMES xgettext
             ${GETTEXT_FINDPROGRAM_OPTIONS})

find_program(MSGFMT
              NAMES msgfmt
             ${GETTEXT_FINDPROGRAM_OPTIONS})

find_program(MSGMERGE
              NAMES msgmerge
             ${GETTEXT_FINDPROGRAM_OPTIONS})

if(NOT XGETTEXT)
  message(STATUS "'xgettext' program not found")
else()
  message(STATUS "'xgettext' program found at '${XGETTEXT}'")
endif()

if(NOT MSGFMT)
  message(STATUS "'msgfmt' program not found")
else()
  message(STATUS "'msgfmt' program found at '${MSGFMT}'")
endif()

if(NOT MSGMERGE)
  message(STATUS "'msgmerge' program not found")
else()
  message(STATUS "'msgmerge' program found at '${MSGMERGE}'")
endif()
