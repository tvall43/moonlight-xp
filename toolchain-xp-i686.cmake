set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR i686)

set(TOOLCHAIN_PREFIX /workspaces/moonlight-xp/tools/bin/i686-w64-mingw32-)
set(CMAKE_C_COMPILER ${TOOLCHAIN_PREFIX}gcc)
set(CMAKE_CXX_COMPILER ${TOOLCHAIN_PREFIX}g++)
set(CMAKE_RC_COMPILER ${TOOLCHAIN_PREFIX}windres)
set(CMAKE_AR /workspaces/moonlight-xp/tools/bin/llvm-ar)
set(CMAKE_RANLIB /workspaces/moonlight-xp/tools/bin/llvm-ranlib)

# Target Windows XP (NT 5.1)
set(XP_FLAGS "-D_WIN32_WINNT=0x0501 -DWINVER=0x0501 -march=pentium4 -msse2 -mfpmath=sse")
set(XP_LINK_FLAGS "-Wl,--subsystem,windows:5.1 -Wl,--major-os-version,5 -Wl,--minor-os-version,1 -Wl,--major-subsystem-version,5 -Wl,--minor-subsystem-version,1")

set(CMAKE_C_FLAGS_INIT "${XP_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT "${XP_FLAGS}")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${XP_LINK_FLAGS}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${XP_LINK_FLAGS}")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
