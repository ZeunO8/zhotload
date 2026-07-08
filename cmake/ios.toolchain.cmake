# cmake/ios.toolchain.cmake
#
# Minimal iOS toolchain built on CMake's native iOS support (CMake >= 3.14).
# It selects the SDK (device vs. simulator), architecture, and deployment
# target, then defers everything else to CMake's built-in Apple handling.
#
# Usage (Xcode generator is recommended for Apple platforms):
#
#   # Device (arm64):
#   cmake -B build-ios -G Xcode \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/ios.toolchain.cmake
#   cmake --build build-ios --config Release
#
#   # Simulator on Apple silicon:
#   cmake -B build-sim -G Xcode \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/ios.toolchain.cmake \
#         -DZHL_IOS_PLATFORM=SIMULATORARM64
#
# Options (all optional, passed with -D):
#   ZHL_IOS_PLATFORM   OS64            device, arm64            [default]
#                      SIMULATOR64     simulator, x86_64 (Intel host)
#                      SIMULATORARM64  simulator, arm64 (Apple silicon host)
#   DEPLOYMENT_TARGET  Minimum iOS version                     [default 13.0]

set(CMAKE_SYSTEM_NAME iOS)

if(NOT DEFINED ZHL_IOS_PLATFORM)
    set(ZHL_IOS_PLATFORM "OS64")
endif()

if(NOT DEFINED DEPLOYMENT_TARGET)
    set(DEPLOYMENT_TARGET "13.0")
endif()
set(CMAKE_OSX_DEPLOYMENT_TARGET "${DEPLOYMENT_TARGET}"
    CACHE STRING "Minimum iOS deployment target")

if(ZHL_IOS_PLATFORM STREQUAL "OS64")
    set(CMAKE_OSX_SYSROOT "iphoneos" CACHE STRING "iOS SDK")
    set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "iOS build architectures")
elseif(ZHL_IOS_PLATFORM STREQUAL "SIMULATOR64")
    set(CMAKE_OSX_SYSROOT "iphonesimulator" CACHE STRING "iOS SDK")
    set(CMAKE_OSX_ARCHITECTURES "x86_64" CACHE STRING "iOS build architectures")
elseif(ZHL_IOS_PLATFORM STREQUAL "SIMULATORARM64")
    set(CMAKE_OSX_SYSROOT "iphonesimulator" CACHE STRING "iOS SDK")
    set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "iOS build architectures")
else()
    message(FATAL_ERROR
        "Unknown ZHL_IOS_PLATFORM '${ZHL_IOS_PLATFORM}'. "
        "Expected one of: OS64, SIMULATOR64, SIMULATORARM64.")
endif()

# Skip the compiler ABI check: it tries to link a runnable test binary, which
# cannot execute when cross-compiling for iOS.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# Resolve host programs from the host, but headers/libraries/packages only from
# the iOS SDK sysroot so desktop libraries never leak into the device build.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
