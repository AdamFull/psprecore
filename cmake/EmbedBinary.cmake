# cmake -DINPUT=<file> -DOUTPUT=<header> -DNAME=<identifier> -P EmbedBinary.cmake
file(READ ${INPUT} hex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
string(REGEX REPLACE "((0x..,){16})" "\\1\n    " bytes "${bytes}")
file(WRITE ${OUTPUT} "// Generated from ${INPUT}; do not edit.\n#pragma once\n\nalignas(4) inline constexpr unsigned char ${NAME}[] = {\n    ${bytes}\n};\n")
