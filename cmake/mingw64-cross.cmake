# A MinGW-w64 cross compiler on macOS or Linux (for example Homebrew's mingw-w64) producing
# native Windows x64 binaries. The tests it builds need Windows (or Wine) to run.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(MINGW_TRIPLE "x86_64-w64-mingw32" CACHE STRING "MinGW-w64 target triple")
find_program(CMAKE_C_COMPILER "${MINGW_TRIPLE}-gcc" REQUIRED)
find_program(CMAKE_RC_COMPILER "${MINGW_TRIPLE}-windres" REQUIRED)
# Look for the target's headers and libraries only in the cross toolchain, and for build tools
# only on the host.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
