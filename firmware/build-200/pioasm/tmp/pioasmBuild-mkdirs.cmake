# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

file(MAKE_DIRECTORY
  "/workspace/pico-sdk/tools/pioasm"
  "/workspace/onerom-1801re2/firmware/build-200/pioasm"
  "/workspace/onerom-1801re2/firmware/build-200/pioasm-install"
  "/workspace/onerom-1801re2/firmware/build-200/pioasm/tmp"
  "/workspace/onerom-1801re2/firmware/build-200/pioasm/src/pioasmBuild-stamp"
  "/workspace/onerom-1801re2/firmware/build-200/pioasm/src"
  "/workspace/onerom-1801re2/firmware/build-200/pioasm/src/pioasmBuild-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/workspace/onerom-1801re2/firmware/build-200/pioasm/src/pioasmBuild-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/workspace/onerom-1801re2/firmware/build-200/pioasm/src/pioasmBuild-stamp${cfgdir}") # cfgdir has leading slash
endif()
