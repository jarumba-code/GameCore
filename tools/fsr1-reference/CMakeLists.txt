cmake_minimum_required(VERSION 3.16)
project(magpie_fsr1_reference LANGUAGES CXX)

add_library(magpie_fsr1 STATIC
    ../../graphics/fsr1/src/image.cpp
    ../../graphics/fsr1/src/easu.cpp
    ../../graphics/fsr1/src/rcas.cpp)
target_include_directories(magpie_fsr1 PUBLIC ../../graphics/fsr1/include)
target_compile_features(magpie_fsr1 PUBLIC cxx_std_17)
target_compile_options(magpie_fsr1 PRIVATE -Wall -Wextra -Wpedantic -Wconversion)

add_executable(fsr1_reference main.cpp)
target_link_libraries(fsr1_reference PRIVATE magpie_fsr1)
target_compile_features(fsr1_reference PRIVATE cxx_std_17)
target_compile_options(fsr1_reference PRIVATE -Wall -Wextra -Wpedantic)
enable_testing()
add_test(NAME fsr1_reference_validation COMMAND fsr1_reference --iterations 12)
target_compile_definitions(fsr1_reference PRIVATE MAGPIE_TESTDATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/testdata")

option(MAGPIE_BUILD_VULKAN_DEVICE_TEST "Build the native Vulkan shader harness" OFF)
if(MAGPIE_BUILD_VULKAN_DEVICE_TEST)
    find_path(VULKAN_INCLUDE_DIR vulkan/vulkan.h REQUIRED)
    if(ANDROID)
        find_library(VULKAN_LIBRARY vulkan REQUIRED)
    else()
        find_library(VULKAN_LIBRARY vulkan PATHS /system/lib64 REQUIRED)
    endif()
    find_program(GLSLC_EXECUTABLE glslc REQUIRED)
    find_program(SPIRV_VAL_EXECUTABLE spirv-val REQUIRED)

    set(MAGPIE_SHADER_DIR ${CMAKE_CURRENT_BINARY_DIR}/shaders)
    file(MAKE_DIRECTORY ${MAGPIE_SHADER_DIR})
    foreach(shader easu rcas)
        add_custom_command(
            OUTPUT ${MAGPIE_SHADER_DIR}/${shader}.spv
            COMMAND ${GLSLC_EXECUTABLE} -I${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/amd-fsr1
                    ${CMAKE_CURRENT_SOURCE_DIR}/../../graphics/fsr1/vulkan/${shader}.comp
                    -o ${MAGPIE_SHADER_DIR}/${shader}.spv
            COMMAND ${SPIRV_VAL_EXECUTABLE} ${MAGPIE_SHADER_DIR}/${shader}.spv
            DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/../../graphics/fsr1/vulkan/${shader}.comp
                    ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/amd-fsr1/ffx_a.h
                    ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/amd-fsr1/ffx_fsr1.h
            VERBATIM)
        list(APPEND MAGPIE_SHADER_BINARIES ${MAGPIE_SHADER_DIR}/${shader}.spv)
    endforeach()
    add_custom_target(magpie_fsr1_shaders DEPENDS ${MAGPIE_SHADER_BINARIES})

    add_executable(fsr1_vulkan_reference vulkan_runner.cpp)
    add_dependencies(fsr1_vulkan_reference magpie_fsr1_shaders)
    target_include_directories(fsr1_vulkan_reference PRIVATE
        ${VULKAN_INCLUDE_DIR} ../../graphics/fsr1/include)
    target_link_libraries(fsr1_vulkan_reference PRIVATE magpie_fsr1 ${VULKAN_LIBRARY})
    target_compile_features(fsr1_vulkan_reference PRIVATE cxx_std_17)
    target_compile_options(fsr1_vulkan_reference PRIVATE -Wall -Wextra -Wpedantic
        -Wno-missing-field-initializers)

    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(MAGPIE_GPU_TEST_DIR ${CMAKE_CURRENT_BINARY_DIR}/gpu-testdata)
    file(MAKE_DIRECTORY ${MAGPIE_GPU_TEST_DIR})
    foreach(case IN ITEMS diagonal checker shape)
        add_test(NAME magpie_vulkan_${case}
            COMMAND fsr1_vulkan_reference
                ${CMAKE_CURRENT_SOURCE_DIR}/testdata/${case}_input_160x90.ppm
                ${MAGPIE_GPU_TEST_DIR}/${case}_amd_fsr1_gpu.ppm
                ${MAGPIE_SHADER_DIR}/easu.spv ${MAGPIE_SHADER_DIR}/rcas.spv)
    endforeach()
    add_test(NAME magpie_vulkan_quality_floors
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/validate_gpu_outputs.py
            ${CMAKE_CURRENT_SOURCE_DIR} ${MAGPIE_GPU_TEST_DIR})
    set_tests_properties(magpie_vulkan_quality_floors PROPERTIES
        DEPENDS "magpie_vulkan_diagonal;magpie_vulkan_checker;magpie_vulkan_shape")

    foreach(shader easu rcas)
        add_custom_command(
            OUTPUT ${MAGPIE_SHADER_DIR}/${shader}_gles.spv
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/prepare_gles_headers.py
                    ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/amd-fsr1
                    ${MAGPIE_SHADER_DIR}/gles_headers
            COMMAND ${GLSLC_EXECUTABLE} -fshader-stage=compute --target-env=opengl
                    -I${MAGPIE_SHADER_DIR}/gles_headers
                    ${CMAKE_CURRENT_SOURCE_DIR}/../../graphics/fsr1/gles/${shader}.comp
                    -o ${MAGPIE_SHADER_DIR}/${shader}_gles.spv
            COMMAND ${SPIRV_VAL_EXECUTABLE} --target-env opengl4.5
                    ${MAGPIE_SHADER_DIR}/${shader}_gles.spv
            DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/prepare_gles_headers.py
                    ${CMAKE_CURRENT_SOURCE_DIR}/../../graphics/fsr1/gles/${shader}.comp
                    ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/amd-fsr1/ffx_a.h
                    ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/amd-fsr1/ffx_fsr1.h
            VERBATIM)
        list(APPEND MAGPIE_GLES_SHADER_BINARIES ${MAGPIE_SHADER_DIR}/${shader}_gles.spv)
    endforeach()
    add_custom_target(magpie_gles_shader_compile DEPENDS ${MAGPIE_GLES_SHADER_BINARIES})

    add_executable(magpie_gles_capability_probe gles_capability_probe.cpp)
    add_dependencies(magpie_gles_capability_probe magpie_gles_shader_compile)
    target_include_directories(magpie_gles_capability_probe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../)
    if(ANDROID)
        find_library(ANDROID_EGL_LIBRARY EGL REQUIRED)
        find_library(ANDROID_GLES_LIBRARY GLESv2 REQUIRED)
    else()
        find_library(ANDROID_EGL_LIBRARY EGL PATHS /system/lib64 REQUIRED)
        find_library(ANDROID_GLES_LIBRARY GLESv2 PATHS /system/lib64 REQUIRED)
    endif()
    target_link_libraries(magpie_gles_capability_probe PRIVATE
        ${ANDROID_EGL_LIBRARY} ${ANDROID_GLES_LIBRARY})
    add_test(NAME magpie_gles_runtime_probe COMMAND magpie_gles_capability_probe)
endif()
