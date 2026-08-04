# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

file(MAKE_DIRECTORY
  "/workspace/onerom-1801re2/firmware/build-200/_deps/picotool-src/enc_bootloader"
  "/workspace/onerom-1801re2/firmware/build-200/_deps/picotool-build/enc_bootloader"
  "/workspace/onerom-1801re2/firmware/build-200/_deps/picotool-build/enc_bootloader"
  "/workspace/onerom-1801re2/firmware/build-200/_deps/picotool-build/enc_bootloader/tmp"
  "/workspace/onerom-1801re2/firmware/build-200/_deps/picotool-build/enc_bootloader/src/enc_bootloader-stamp"
  "/workspace/onerom-1801re2/firmware/build-200/_deps/picotool-build/enc_bootloader/src"
  "/workspace/onerom-1801re2/firmware/build-200/_deps/picotool-build/enc_bootloader/src/enc_bootloader-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/workspace/onerom-1801re2/firmware/build-200/_deps/picotool-build/enc_bootloader/src/enc_bootloader-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/workspace/onerom-1801re2/firmware/build-200/_deps/picotool-build/enc_bootloader/src/enc_bootloader-stamp${cfgdir}") # cfgdir has leading slash
endif()
