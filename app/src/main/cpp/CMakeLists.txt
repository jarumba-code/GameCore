cmake_minimum_required(VERSION 3.22.1)
project(android_magpie_native LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(MAGPIE_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../../)
set(SDK_ROOT ${MAGPIE_ROOT}/native-sdk/proof)

add_library(magpie_core SHARED ${SDK_ROOT}/src/core.cpp)
target_include_directories(magpie_core PUBLIC ${SDK_ROOT}/include)
target_compile_options(magpie_core PRIVATE -Wall -Wextra -Wpedantic)

add_library(magpie_fsr SHARED
    ${SDK_ROOT}/src/fsr.cpp
    ${MAGPIE_ROOT}/graphics/fsr1/src/image.cpp
    ${MAGPIE_ROOT}/graphics/fsr1/src/easu.cpp
    ${MAGPIE_ROOT}/graphics/fsr1/src/rcas.cpp
)
target_include_directories(magpie_fsr
    PUBLIC ${SDK_ROOT}/include
    PRIVATE ${MAGPIE_ROOT}/graphics/fsr1/include
)
target_link_libraries(magpie_fsr PRIVATE magpie_core)
target_compile_options(magpie_fsr PRIVATE -Wall -Wextra -Wpedantic)

# Use project-prefixed CMake cache variables and verify that Vulkan resolves to
# a real NDK stub-library path before adding it to the linker command.
find_library(MAGPIE_VULKAN_LIBRARY NAMES vulkan REQUIRED)
if(NOT IS_ABSOLUTE "${MAGPIE_VULKAN_LIBRARY}" OR NOT EXISTS "${MAGPIE_VULKAN_LIBRARY}")
    message(FATAL_ERROR "Android Vulkan link library was not resolved: '${MAGPIE_VULKAN_LIBRARY}'")
endif()

add_library(magpie_vulkan SHARED ${SDK_ROOT}/src/vulkan_probe.cpp)
target_include_directories(magpie_vulkan PUBLIC ${SDK_ROOT}/include)
target_link_libraries(magpie_vulkan PRIVATE "${MAGPIE_VULKAN_LIBRARY}")
target_compile_options(magpie_vulkan PRIVATE -Wall -Wextra -Wpedantic)

# ---------------------------------------------------------------------------
# M4: the two AMD FSR 1.0.2 compute shaders, compiled at build time with the
# NDK's own glslc and embedded as C arrays, so the engine never compiles GLSL at
# runtime and the pinned vendored sources stay the single origin of the shipped
# SPIR-V. The compile inputs match the M2 harness exactly (same -I, same files).
#
# glslc discovery deliberately searches every root a build can plausibly have:
# the ANDROID_NDK_HOME / ANDROID_NDK_ROOT environment variables the wrapper
# scripts set, the toolchain file's own ANDROID_NDK / CMAKE_ANDROID_NDK values,
# the ANDROID_HOME ndk directory, and the GitHub-runner SDK layout. A build that
# cannot find glslc fails with the list of what was searched, not a bare error.
set(MAGPIE_GLSLC_CANDIDATES "")
foreach(magpie_ndk_root IN ITEMS
        "$ENV{ANDROID_NDK_HOME}"
        "$ENV{ANDROID_NDK_ROOT}"
        "${ANDROID_NDK}"
        "${CMAKE_ANDROID_NDK}")
    if(magpie_ndk_root)
        list(APPEND MAGPIE_GLSLC_CANDIDATES
            "${magpie_ndk_root}/toolchains/llvm/prebuilt/*/bin/glslc"
            "${magpie_ndk_root}/toolchains/llvm/prebuilt/*/bin/glslc.exe"
            "${magpie_ndk_root}/shader-tools/*/glslc"
            "${magpie_ndk_root}/shader-tools/*/glslc.exe")
    endif()
endforeach()
if(DEFINED ENV{ANDROID_HOME})
    list(APPEND MAGPIE_GLSLC_CANDIDATES
        "$ENV{ANDROID_HOME}/ndk/*/toolchains/llvm/prebuilt/*/bin/glslc"
        "$ENV{ANDROID_HOME}/ndk/*/toolchains/llvm/prebuilt/*/bin/glslc.exe"
        "$ENV{ANDROID_HOME}/ndk/*/shader-tools/*/glslc"
        "$ENV{ANDROID_HOME}/ndk/*/shader-tools/*/glslc.exe")
endif()
# The GitHub-hosted runner's SDK location, where the workflow installs the NDK.
list(APPEND MAGPIE_GLSLC_CANDIDATES
    "/usr/local/lib/android/sdk/ndk/*/toolchains/llvm/prebuilt/*/bin/glslc"
    "/usr/local/lib/android/sdk/ndk/*/shader-tools/*/glslc")
file(GLOB MAGPIE_GLSLC_CANDIDATES ${MAGPIE_GLSLC_CANDIDATES})
find_program(MAGPIE_GLSLC_EXECUTABLE NAMES glslc HINTS ${MAGPIE_GLSLC_CANDIDATES})
# The NDK stopped shipping glslc (r25+), so the host's glslangValidator is the fallback: it
# compiles the same GLSL to the same Vulkan 1.0 SPIR-V, with the include directive declared in
# the shader itself. One of the two must exist for the build to proceed.
if(MAGPIE_GLSLC_EXECUTABLE)
    set(MAGPIE_SHADER_COMPILER "${MAGPIE_GLSLC_EXECUTABLE}")
    set(MAGPIE_SHADER_COMPILER_ARGS "")
    message(STATUS "FSR shaders will be compiled with glslc: ${MAGPIE_GLSLC_EXECUTABLE}")
else()
    find_program(MAGPIE_GLSLANG_EXECUTABLE NAMES glslangValidator glslang REQUIRED)
    set(MAGPIE_SHADER_COMPILER "${MAGPIE_GLSLANG_EXECUTABLE}")
    set(MAGPIE_SHADER_COMPILER_ARGS "-V" "--target-env" "vulkan1.0")
    message(STATUS "FSR shaders will be compiled with glslangValidator: ${MAGPIE_GLSLANG_EXECUTABLE}")
endif()
set(MAGPIE_SHADER_DIR ${CMAKE_CURRENT_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${MAGPIE_SHADER_DIR})
set(MAGPIE_SHADER_ROOT ${MAGPIE_ROOT}/graphics/fsr1/vulkan)
set(MAGPIE_AMD_ROOT ${MAGPIE_ROOT}/third_party/amd-fsr1)
foreach(shader IN ITEMS easu rcas)
    add_custom_command(
        OUTPUT ${MAGPIE_SHADER_DIR}/${shader}.spv
        COMMAND ${MAGPIE_SHADER_COMPILER} ${MAGPIE_SHADER_COMPILER_ARGS}
                -I${MAGPIE_AMD_ROOT}
                ${MAGPIE_SHADER_ROOT}/${shader}.comp -o ${MAGPIE_SHADER_DIR}/${shader}.spv
        DEPENDS ${MAGPIE_SHADER_ROOT}/${shader}.comp
                ${MAGPIE_AMD_ROOT}/ffx_a.h
                ${MAGPIE_AMD_ROOT}/ffx_fsr1.h
        VERBATIM)
    list(APPEND MAGPIE_SHADER_SPV ${MAGPIE_SHADER_DIR}/${shader}.spv)
endforeach()

# Embed each SPIR-V blob as a C byte array at build time (never at configure time — the .spv
# files only exist after glslc runs). Pure CMake: read the file as hex, turn every byte pair into
# `0xNN,`, write the header. No generator tooling beyond CMake itself, so the app build stays
# self-contained.
add_custom_command(
    OUTPUT ${MAGPIE_SHADER_DIR}/magpie_shaders.h
    COMMAND ${CMAKE_COMMAND}
        -DMAGPIE_EASU_SPV=${MAGPIE_SHADER_DIR}/easu.spv
        -DMAGPIE_RCAS_SPV=${MAGPIE_SHADER_DIR}/rcas.spv
        -DMAGPIE_OUT_HEADER=${MAGPIE_SHADER_DIR}/magpie_shaders.h
        -P ${CMAKE_CURRENT_SOURCE_DIR}/embed_shaders.cmake
    DEPENDS ${MAGPIE_SHADER_SPV} ${CMAKE_CURRENT_SOURCE_DIR}/embed_shaders.cmake
    VERBATIM)

add_library(magpie_vulkan_engine SHARED ${SDK_ROOT}/src/vulkan_engine.cpp)
add_custom_target(magpie_shaders_header
    DEPENDS ${MAGPIE_SHADER_DIR}/magpie_shaders.h ${MAGPIE_SHADER_SPV})
add_dependencies(magpie_vulkan_engine magpie_shaders_header)
target_include_directories(magpie_vulkan_engine
    PUBLIC ${SDK_ROOT}/include
    PRIVATE ${MAGPIE_SHADER_DIR})
target_link_libraries(magpie_vulkan_engine PRIVATE "${MAGPIE_VULKAN_LIBRARY}")
target_compile_options(magpie_vulkan_engine PRIVATE -Wall -Wextra -Wpedantic)

find_library(MAGPIE_EGL_LIBRARY NAMES EGL REQUIRED)
find_library(MAGPIE_GLES_LIBRARY NAMES GLESv3 REQUIRED)
add_library(magpie_gles SHARED ${SDK_ROOT}/src/gles_probe.cpp)
target_include_directories(magpie_gles PUBLIC ${SDK_ROOT}/include)
target_link_libraries(magpie_gles PRIVATE
    "${MAGPIE_EGL_LIBRARY}"
    "${MAGPIE_GLES_LIBRARY}"
)
target_compile_options(magpie_gles PRIVATE -Wall -Wextra -Wpedantic)

find_library(MAGPIE_ANDROID_LIBRARY NAMES android REQUIRED)
find_library(MAGPIE_JNIGRAPHICS_LIBRARY NAMES jnigraphics REQUIRED)
add_library(magpie_android_jni SHARED native_fsr.cpp)
target_include_directories(magpie_android_jni PRIVATE
    ${SDK_ROOT}/include
    ${MAGPIE_SHADER_DIR})
add_dependencies(magpie_android_jni magpie_shaders_header)
target_link_libraries(magpie_android_jni PRIVATE
    magpie_core
    magpie_fsr
    magpie_vulkan
    magpie_vulkan_engine
    magpie_gles
    "${MAGPIE_ANDROID_LIBRARY}"
    "${MAGPIE_JNIGRAPHICS_LIBRARY}"
)
target_compile_options(magpie_android_jni PRIVATE -Wall -Wextra -Wpedantic)
