# Cross-compile for 64-bit Windows with MinGW-w64 from Linux.
# The resulting executables are not run on the build host.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(MINGW_TRIPLE x86_64-w64-mingw32)
# Prefer the POSIX thread model variant of x86_64-w64-mingw32-g++: the default
# win32 variant on Ubuntu 22.04 (GCC 10) lacks std::thread and std::mutex.
find_program(MINGW_CXX NAMES ${MINGW_TRIPLE}-g++-posix ${MINGW_TRIPLE}-g++ REQUIRED)
set(CMAKE_CXX_COMPILER ${MINGW_CXX})
set(CMAKE_RC_COMPILER ${MINGW_TRIPLE}-windres)

set(CMAKE_FIND_ROOT_PATH /usr/${MINGW_TRIPLE})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Link the GCC runtime statically so the executables run without MinGW DLLs.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static -static-libgcc -static-libstdc++")
