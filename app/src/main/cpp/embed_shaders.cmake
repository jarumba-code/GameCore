# Build-time SPIR-V embedding for the Magpie FSR components. Runs as a CMake script (-P) after
# the shader compiler has produced the .spv files, so header generation happens when the blobs
# exist, not at configure time. Pure CMake: read the file as hex, turn each byte pair into
# `0xNN,`, write the two arrays into one header.
#
# Usage:
#   cmake -DMAGPIE_EASU_SPV=<path> -DMAGPIE_RCAS_SPV=<path> -DMAGPIE_OUT_HEADER=<path>
#         [-DMAGPIE_NAMESPACE=magpie_shaders] [-DMAGPIE_NAME_A=kEasuSpv] [-DMAGPIE_NAME_B=kRcasSpv]
#         -P embed_shaders.cmake

if(NOT MAGPIE_EASU_SPV OR NOT MAGPIE_RCAS_SPV OR NOT MAGPIE_OUT_HEADER)
    message(FATAL_ERROR "embed_shaders.cmake needs MAGPIE_EASU_SPV, MAGPIE_RCAS_SPV and MAGPIE_OUT_HEADER")
endif()
if(NOT MAGPIE_NAMESPACE)
    set(MAGPIE_NAMESPACE "magpie_shaders")
endif()
if(NOT MAGPIE_NAME_A)
    set(MAGPIE_NAME_A "kEasuSpv")
endif()
if(NOT MAGPIE_NAME_B)
    set(MAGPIE_NAME_B "kRcasSpv")
endif()

foreach(pair IN ITEMS "${MAGPIE_EASU_SPV};${MAGPIE_NAME_A}" "${MAGPIE_RCAS_SPV};${MAGPIE_NAME_B}")
    list(GET pair 0 spv_file)
    list(GET pair 1 c_name)
    file(READ "${spv_file}" magpie_hex HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1, " magpie_bytes "${magpie_hex}")
    string(LENGTH "${magpie_hex}" magpie_hex_len)
    math(EXPR magpie_len "${magpie_hex_len} / 2")
    string(APPEND magpie_out
        "// Generated from ${spv_file} by the native build; do not edit.\n"
        "#include <cstdint>\n"
        "namespace ${MAGPIE_NAMESPACE} {\n"
        "inline const uint32_t ${c_name}[] = {\n${magpie_bytes}0u\n};\n"
        "inline const uint32_t ${c_name}_word_count = ${magpie_len} / 4u;\n"
        "}  // namespace ${MAGPIE_NAMESPACE}\n\n")
endforeach()

file(WRITE "${MAGPIE_OUT_HEADER}" "${magpie_out}")
message(STATUS "Embedded shaders (${MAGPIE_NAMESPACE}) into ${MAGPIE_OUT_HEADER}")
