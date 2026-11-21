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
target_include_directories(magpie_android_jni PRIVATE ${SDK_ROOT}/include)
target_link_libraries(magpie_android_jni PRIVATE
    magpie_core
    magpie_fsr
    magpie_vulkan
    magpie_gles
    "${MAGPIE_ANDROID_LIBRARY}"
    "${MAGPIE_JNIGRAPHICS_LIBRARY}"
)
target_compile_options(magpie_android_jni PRIVATE -Wall -Wextra -Wpedantic)
