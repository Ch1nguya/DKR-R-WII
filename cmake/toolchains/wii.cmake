# cmake/toolchains/wii.cmake
# Cross-compilation toolchain pour Nintendo Wii (PowerPC Broadway)

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR powerpc)

# devkitPPC toolchain
set(DEVKITPRO $ENV{DEVKITPRO})
if(NOT DEVKITPRO)
    set(DEVKITPRO /opt/devkitpro)
endif()
set(DEVKITPPC ${DEVKITPRO}/devkitPPC)

set(CMAKE_C_COMPILER   ${DEVKITPPC}/bin/powerpc-eabi-gcc)
set(CMAKE_CXX_COMPILER ${DEVKITPPC}/bin/powerpc-eabi-g++)
set(CMAKE_AR           ${DEVKITPPC}/bin/powerpc-eabi-ar)
set(CMAKE_RANLIB       ${DEVKITPPC}/bin/powerpc-eabi-ranlib)

# Flags de base Wii
set(MACHDEP "-DGEKKO -mrvl -mcpu=750 -meabi -mhard-float")
set(CMAKE_C_FLAGS_INIT   "${MACHDEP} -O2 -Wall")
set(CMAKE_CXX_FLAGS_INIT "${MACHDEP} -O2 -Wall -fno-rtti")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${MACHDEP} -specs=${DEVKITPRO}/libogc/share/rvl.specs -L${DEVKITPRO}/libogc/lib/wii")

# libogc
set(LIBOGC_DIR ${DEVKITPRO}/libogc)
include_directories(${LIBOGC_DIR}/include)
link_directories(${LIBOGC_DIR}/lib/wii)

set(CMAKE_FIND_ROOT_PATH ${DEVKITPPC} ${LIBOGC_DIR})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
