# Build-time SPIR-V embedding for the M4 FSR engine. Runs as a CMake script (-P) after glslc has
# produced the .spv files, so the header generation happens when the shaders exist, not at
# configure time. Pure CMake: read the file as hex, turn each byte pair into `0xNN,`, write the
# two arrays into one header the engine includes.
#
# Usage:
#   cmake -DMAGPIE_EASU_SPV=<path> -DMAGPIE_RCAS_SPV=<path> -DMAGPIE_OUT_HEADER=<path> -P embed_shaders.cmake

if(NOT MAGPIE_EASU_SPV OR NOT MAGPIE_RCAS_SPV OR NOT MAGPIE_OUT_HEADER)
    message(FATAL_ERROR "embed_shaders.cmake needs MAGPIE_EASU_SPV, MAGPIE_RCAS_SPV and MAGPIE_OUT_HEADER")
endif()

foreach(pair IN ITEMS "${MAGPIE_EASU_SPV};kEasuSpv" "${MAGPIE_RCAS_SPV};kRcasSpv")
    list(GET pair 0 spv_file)
    list(GET pair 1 c_name)
    file(READ "${spv_file}" magpie_hex HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1, " magpie_bytes "${magpie_hex}")
    string(LENGTH "${magpie_hex}" magpie_hex_len)
    math(EXPR magpie_len "${magpie_hex_len} / 2")
    string(APPEND magpie_out
        "// Generated from ${spv_file} by the native build; do not edit.\n"
        "#include <cstdint>\n"
        "namespace magpie_shaders {\n"
        "inline const uint32_t ${c_name}[] = {\n${magpie_bytes}0u\n};\n"
        "inline const uint32_t ${c_name}_word_count = ${magpie_len} / 4u;\n"
        "}  // namespace magpie_shaders\n\n")
endforeach()

file(WRITE "${MAGPIE_OUT_HEADER}" "${magpie_out}")
message(STATUS "Embedded FSR shaders into ${MAGPIE_OUT_HEADER}")
