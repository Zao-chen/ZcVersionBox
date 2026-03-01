# Additional clean files
cmake_minimum_required(VERSION 3.16)

if("${CONFIG}" STREQUAL "" OR "${CONFIG}" STREQUAL "Debug")
  file(REMOVE_RECURSE
  "CMakeFiles\\ZcVersionBox_autogen.dir\\AutogenUsed.txt"
  "CMakeFiles\\ZcVersionBox_autogen.dir\\ParseCache.txt"
  "ZcVersionBox_autogen"
  )
endif()
