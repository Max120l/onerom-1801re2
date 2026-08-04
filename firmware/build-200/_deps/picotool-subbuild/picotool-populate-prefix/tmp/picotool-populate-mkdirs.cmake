# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

file(MAKE_DIRECTORY
  "/workspace/onerom-1801re2/firmware/build-200/_deps/picotool-src"
  "/workspace/onerom-1801re2/firmware/build-200/_deps/picotool-build"
  "/workspace/onerom-1801re2/firmware/build-200/_deps/picotool-subbuild/picotool-populate-prefix"
  "/workspace/onerom-1801re2/firmware/build-200/_deps/picotool-subbuild/picotool-populate-prefix/tmp"
  "/workspace/onerom-1801re2/firmware/build-200/_deps/picotool-subbuild/picotool-populate-prefix/src/picotool-populate-stamp"
  "/workspace/onerom-1801re2/firmware/build-200/_deps/picotool-subbuild/picotool-populate-prefix/src"
  "/workspace/onerom-1801re2/firmware/build-200/_deps/picotool-subbuild/picotool-populate-prefix/src/picotool-populate-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/workspace/onerom-1801re2/firmware/build-200/_deps/picotool-subbuild/picotool-populate-prefix/src/picotool-populate-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/workspace/onerom-1801re2/firmware/build-200/_deps/picotool-subbuild/picotool-populate-prefix/src/picotool-populate-stamp${cfgdir}") # cfgdir has leading slash
endif()
