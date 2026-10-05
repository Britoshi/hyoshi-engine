# Converts a WGSL file into a C++ header holding it as a string.
# Usage: cmake -DINPUT=<file.wgsl> -DOUTPUT=<file.h> -DNAME=<ARRAY_NAME> -P EmbedWgsl.cmake

file(READ ${INPUT} source)

file(WRITE ${OUTPUT} "// Generated from ${INPUT}. Do not edit.\n#pragma once\n\n"
                     "inline constexpr char ${NAME}[] = R\"hyoshi_wgsl(${source})hyoshi_wgsl\";\n")
