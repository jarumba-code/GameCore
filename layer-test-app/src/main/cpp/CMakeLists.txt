cmake_minimum_required(VERSION 3.22.1)
project(magpie_layer_test LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(MAGPIE_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../../)
set(MAGPIE_AMD_ROOT ${MAGPIE_ROOT}/third_party/amd-fsr1)

# GLSL front end: prefer glslc when an NDK ships one (r24-), otherwise glslangValidator
# (installed by the CI workflow; same Vulkan 1.0 SPIR-V). One shared resolver for all shaders.
function(magpie_find_shader_compiler out_exe out_args)
    file(GLOB glslc_candidates
        "$ENV{ANDROID_NDK_HOME}/toolchains/llvm/prebuilt/*/bin/glslc"
        "$ENV{ANDROID_NDK_HOME}/toolchains/llvm/prebuilt/*/bin/glslc.exe"
        "$ENV{ANDROID_NDK_HOME}/shader-tools/*/glslc"
        "$ENV{ANDROID_NDK_HOME}/shader-tools/*/glslc.exe")
    find_program(magpie_glslc NAMES glslc HINTS ${glslc_candidates})
    if(magpie_glslc)
        set(${out_exe} "${magpie_glslc}" PARENT_SCOPE)
        set(${out_args} "" PARENT_SCOPE)
        return()
    endif()
    find_program(magpie_glslang NAMES glslangValidator glslang)
    if(magpie_glslang)
        set(${out_exe} "${magpie_glslang}" PARENT_SCOPE)
        set(${out_args} "-V" "--target-env" "vulkan1.0" PARENT_SCOPE)
        return()
    endif()
    # No shader compiler found
    set(${out_exe} "" PARENT_SCOPE)
    set(${out_args} "" PARENT_SCOPE)
endfunction()

magpie_find_shader_compiler(MAGPIE_SHADER_EXE MAGPIE_SHADER_ARGS)
if(NOT MAGPIE_SHADER_EXE)
    message(FATAL_ERROR "No GLSL shader compiler (glslc or glslangValidator) found. "
        "Install glslang-tools package or ensure NDK shader-tools are available.")
endif()
message(STATUS "layer-test shaders compiled with: ${MAGPIE_SHADER_EXE}")

set(MAGPIE_SHADER_DIR ${CMAKE_CURRENT_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${MAGPIE_SHADER_DIR})

# Embed a compiled SPIR-V blob as a C array (same discipline as the main app's embed step).
# Note: This function is defined but not currently used; headers are generated inline below.
function(magpie_embed_spv spv_file c_name out_header)
    file(READ "${spv_file}" magpie_hex HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1, " magpie_bytes "${magpie_hex}")
    string(LENGTH "${magpie_hex}" magpie_hex_len)
    math(EXPR magpie_len "${magpie_hex_len} / 2")
    file(WRITE "${out_header}"
"// Generated from ${spv_file}; do not edit.\n#include <cstdint>\nnamespace ${c_name} {\ninline const uint32_t blob[] = {\n${magpie_bytes}0u\n};\ninline const uint32_t blob_word_count = ${magpie_len} / 4u;\n}  // namespace ${c_name}\n")
endfunction()

# Track test shaders (vert + frag) separately from FSR shaders
set(MAGPIE_TEST_SPV "")
# Test app graphics shaders (vert + frag), compiled and embedded as magpie_test::kVertSpv/kFragSpv.
foreach(entry IN ITEMS "test.vert:test_vert" "test.frag:test_frag")
    string(REPLACE ":" ";" entry_parts "${entry}")
    list(LENGTH entry_parts entry_part_count)
    if(NOT entry_part_count EQUAL 2)
        message(FATAL_ERROR "Invalid test shader entry '${entry}'; expected source:basename")
    endif()
    list(GET entry_parts 0 src)
    list(GET entry_parts 1 base)
    add_custom_command(
        OUTPUT ${MAGPIE_SHADER_DIR}/${base}.spv
        COMMAND ${MAGPIE_SHADER_EXE} ${MAGPIE_SHADER_ARGS}
                ${CMAKE_CURRENT_SOURCE_DIR}/${src} -o ${MAGPIE_SHADER_DIR}/${base}.spv
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${src}
        VERBATIM)
    list(APPEND MAGPIE_TEST_SPV ${MAGPIE_SHADER_DIR}/${base}.spv)
endforeach()

# Track FSR shaders (easu + rcas) separately
set(MAGPIE_FSR_SPV "")
# The layer's FSR compute shaders, from the same pinned AMD sources as the main app embeds.
foreach(shader IN ITEMS easu rcas)
    add_custom_command(
        OUTPUT ${MAGPIE_SHADER_DIR}/${shader}.spv
        COMMAND ${MAGPIE_SHADER_EXE} ${MAGPIE_SHADER_ARGS} -I${MAGPIE_AMD_ROOT}
                ${MAGPIE_ROOT}/graphics/fsr1/vulkan/${shader}.comp
                -o ${MAGPIE_SHADER_DIR}/${shader}.spv
        DEPENDS ${MAGPIE_ROOT}/graphics/fsr1/vulkan/${shader}.comp
                ${MAGPIE_AMD_ROOT}/ffx_a.h
                ${MAGPIE_AMD_ROOT}/ffx_fsr1.h
        VERBATIM)
    list(APPEND MAGPIE_FSR_SPV ${MAGPIE_SHADER_DIR}/${shader}.spv)
endforeach()

# test_shaders.h depends ONLY on test_vert.spv and test_frag.spv
add_custom_command(
    OUTPUT ${MAGPIE_SHADER_DIR}/test_shaders.h
    COMMAND ${CMAKE_COMMAND}
        -DMAGPIE_EASU_SPV=${MAGPIE_SHADER_DIR}/test_vert.spv
        -DMAGPIE_RCAS_SPV=${MAGPIE_SHADER_DIR}/test_frag.spv
        -DMAGPIE_OUT_HEADER=${MAGPIE_SHADER_DIR}/test_shaders.h
        -DMAGPIE_NAMESPACE=magpie_test
        -DMAGPIE_NAME_A=kVertSpv
        -DMAGPIE_NAME_B=kFragSpv
        -P ${MAGPIE_ROOT}/app/src/main/cpp/embed_shaders.cmake
    DEPENDS ${MAGPIE_SHADER_DIR}/test_vert.spv ${MAGPIE_SHADER_DIR}/test_frag.spv
            ${MAGPIE_ROOT}/app/src/main/cpp/embed_shaders.cmake
    VERBATIM)

# magpie_shaders.h depends ONLY on easu.spv and rcas.spv
add_custom_command(
    OUTPUT ${MAGPIE_SHADER_DIR}/magpie_shaders.h
    COMMAND ${CMAKE_COMMAND}
        -DMAGPIE_EASU_SPV=${MAGPIE_SHADER_DIR}/easu.spv
        -DMAGPIE_RCAS_SPV=${MAGPIE_SHADER_DIR}/rcas.spv
        -DMAGPIE_OUT_HEADER=${MAGPIE_SHADER_DIR}/magpie_shaders.h
        -P ${MAGPIE_ROOT}/app/src/main/cpp/embed_shaders.cmake
    DEPENDS ${MAGPIE_SHADER_DIR}/easu.spv ${MAGPIE_SHADER_DIR}/rcas.spv
            ${MAGPIE_ROOT}/app/src/main/cpp/embed_shaders.cmake
    VERBATIM)

add_custom_target(magpie_test_shaders
    DEPENDS ${MAGPIE_SHADER_DIR}/test_shaders.h ${MAGPIE_SHADER_DIR}/magpie_shaders.h
            ${MAGPIE_TEST_SPV} ${MAGPIE_FSR_SPV})

find_library(MAGPIE_VULKAN_LIBRARY NAMES vulkan REQUIRED)
find_library(MAGPIE_LOG_LIBRARY NAMES log REQUIRED)
if(NOT IS_ABSOLUTE "${MAGPIE_VULKAN_LIBRARY}" OR NOT EXISTS "${MAGPIE_VULKAN_LIBRARY}")
    message(FATAL_ERROR "Android Vulkan link library was not resolved: '${MAGPIE_VULKAN_LIBRARY}'")
endif()

add_library(VkLayer_MagpieFSR SHARED
    ${MAGPIE_ROOT}/vulkan-layer/src/magpie_layer.cpp)
add_dependencies(VkLayer_MagpieFSR magpie_test_shaders)
target_include_directories(VkLayer_MagpieFSR PRIVATE
    ${MAGPIE_SHADER_DIR}
    ${MAGPIE_ROOT}/third_party/Vulkan-Headers/include)
target_link_libraries(VkLayer_MagpieFSR PRIVATE
    "${MAGPIE_VULKAN_LIBRARY}"
    "${MAGPIE_LOG_LIBRARY}")
target_compile_options(VkLayer_MagpieFSR PRIVATE -Wall -Wextra)

# The controlled test application's renderer.
find_library(MAGPIE_ANDROID_LIBRARY NAMES android REQUIRED)
add_library(magpie_test_renderer SHARED MagpieTestRenderer.cpp)
add_dependencies(magpie_test_renderer magpie_test_shaders)
target_include_directories(magpie_test_renderer PRIVATE ${MAGPIE_SHADER_DIR})
target_link_libraries(magpie_test_renderer PRIVATE
    "${MAGPIE_VULKAN_LIBRARY}"
    "${MAGPIE_ANDROID_LIBRARY}"
    "${MAGPIE_LOG_LIBRARY}")
target_compile_options(magpie_test_renderer PRIVATE -Wall -Wextra)

