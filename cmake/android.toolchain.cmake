# cmake/android.toolchain.cmake
#
# Thin convenience wrapper over the NDK's own android.toolchain.cmake. It locates
# the NDK from the usual environment variables (or the standard Android Studio
# SDK layout), fills in sensible defaults for ABI and API level, then defers
# everything else to the NDK toolchain. The goal is a one-line configure for a
# zhl Android build:
#
#   cmake -B build-android \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/android.toolchain.cmake
#
# Options (all optional, passed with -D or the environment):
#   ANDROID_NDK       Path to the NDK. Auto-detected from ANDROID_NDK_HOME /
#                     ANDROID_NDK_ROOT / NDK_ROOT, or the newest NDK under
#                     $ANDROID_HOME|$ANDROID_SDK_ROOT/ndk (Android Studio's
#                     default install location).
#   ANDROID_ABI       arm64-v8a [default] | armeabi-v7a | x86_64 | x86
#   ANDROID_PLATFORM  Minimum API level, e.g. android-24                [default]
#
# The NDK toolchain leaves CMAKE_SYSTEM_NAME = "Android" and defines the CMake
# `ANDROID` variable, which zhl's CMakeLists keys off to select the JNI HTTP
# backend and to exclude the host-only server/test targets.

# ── Locate the NDK ────────────────────────────────────────────────────────────
if(NOT ANDROID_NDK)
    foreach(_env ANDROID_NDK_HOME ANDROID_NDK_ROOT ANDROID_NDK NDK_ROOT NDK_HOME)
        if(DEFINED ENV{${_env}} AND IS_DIRECTORY "$ENV{${_env}}")
            set(ANDROID_NDK "$ENV{${_env}}")
            break()
        endif()
    endforeach()
endif()

# Fall back to the newest NDK under the Android Studio SDK location.
if(NOT ANDROID_NDK)
    set(_sdk_candidates "")
    foreach(_env ANDROID_HOME ANDROID_SDK_ROOT)
        if(DEFINED ENV{${_env}} AND IS_DIRECTORY "$ENV{${_env}}")
            list(APPEND _sdk_candidates "$ENV{${_env}}")
        endif()
    endforeach()
    if(DEFINED ENV{HOME})
        list(APPEND _sdk_candidates "$ENV{HOME}/Library/Android/sdk")   # macOS
        list(APPEND _sdk_candidates "$ENV{HOME}/Android/Sdk")            # Linux
    endif()
    foreach(_sdk ${_sdk_candidates})
        if(IS_DIRECTORY "${_sdk}/ndk")
            file(GLOB _ndk_versions LIST_DIRECTORIES true "${_sdk}/ndk/*")
            list(FILTER _ndk_versions INCLUDE REGEX "/[0-9]+\\.[0-9]+")
            if(_ndk_versions)
                # Natural sort so 26.x precedes 100.x correctly; take the newest.
                list(SORT _ndk_versions COMPARE NATURAL)
                list(GET _ndk_versions -1 ANDROID_NDK)
                break()
            endif()
        endif()
        # Some setups keep a single unversioned NDK under sdk/ndk-bundle.
        if(NOT ANDROID_NDK AND IS_DIRECTORY "${_sdk}/ndk-bundle")
            set(ANDROID_NDK "${_sdk}/ndk-bundle")
            break()
        endif()
    endforeach()
endif()

if(NOT ANDROID_NDK OR NOT IS_DIRECTORY "${ANDROID_NDK}")
    message(FATAL_ERROR
        "Android NDK not found. Install it via Android Studio's SDK Manager "
        "(SDK Tools -> NDK), then either set -DANDROID_NDK=/path/to/ndk or export "
        "ANDROID_NDK_HOME. Looked at ANDROID_NDK_HOME/ANDROID_NDK_ROOT/NDK_ROOT "
        "and the newest NDK under \$ANDROID_HOME/ndk.")
endif()

set(_ndk_toolchain "${ANDROID_NDK}/build/cmake/android.toolchain.cmake")
if(NOT EXISTS "${_ndk_toolchain}")
    message(FATAL_ERROR
        "'${ANDROID_NDK}' does not look like a valid NDK: "
        "${_ndk_toolchain} is missing.")
endif()

# ── Defaults ──────────────────────────────────────────────────────────────────
if(NOT DEFINED ANDROID_ABI)
    set(ANDROID_ABI "arm64-v8a")
endif()
if(NOT DEFINED ANDROID_PLATFORM AND NOT DEFINED ANDROID_NATIVE_API_LEVEL)
    # API 24 (Android 7.0) is a safe floor: dlopen of app-private, absolute-path
    # .so files works cleanly, and it covers the vast majority of devices.
    set(ANDROID_PLATFORM "android-24")
endif()

# ── Hand off to the real NDK toolchain ────────────────────────────────────────
include("${_ndk_toolchain}")
