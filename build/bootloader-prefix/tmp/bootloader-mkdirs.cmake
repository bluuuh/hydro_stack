# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

file(MAKE_DIRECTORY
  "/root/esp/esp-idf/components/bootloader/subproject"
  "/root/projects/esp/mqtt_oneshot_read/build/bootloader"
  "/root/projects/esp/mqtt_oneshot_read/build/bootloader-prefix"
  "/root/projects/esp/mqtt_oneshot_read/build/bootloader-prefix/tmp"
  "/root/projects/esp/mqtt_oneshot_read/build/bootloader-prefix/src/bootloader-stamp"
  "/root/projects/esp/mqtt_oneshot_read/build/bootloader-prefix/src"
  "/root/projects/esp/mqtt_oneshot_read/build/bootloader-prefix/src/bootloader-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/root/projects/esp/mqtt_oneshot_read/build/bootloader-prefix/src/bootloader-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/root/projects/esp/mqtt_oneshot_read/build/bootloader-prefix/src/bootloader-stamp${cfgdir}") # cfgdir has leading slash
endif()
