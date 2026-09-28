# Converts a SPIR-V binary into a C++ header holding a uint32_t array.
# Usage: cmake -DINPUT=<file.spv> -DOUTPUT=<file.h> -DNAME=<ARRAY_NAME> -P EmbedSpirv.cmake

file(READ ${INPUT} bytes HEX)

# SPIR-V is a stream of little-endian 32-bit words.
string(REGEX REPLACE "([0-9a-f][0-9a-f])([0-9a-f][0-9a-f])([0-9a-f][0-9a-f])([0-9a-f][0-9a-f])" "0x\\4\\3\\2\\1u,\n"
       words "${bytes}")

file(WRITE ${OUTPUT} "// Generated from ${INPUT}. Do not edit.\n#pragma once\n\n#include <cstdint>\n\n"
                     "inline constexpr uint32_t ${NAME}[] = {\n${words}};\n")
