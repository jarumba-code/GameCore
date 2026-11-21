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
