# cmake -DINPUT=<file> -DOUTPUT=<file.cpp> -DNAME=<symbol> -P embed_file.cmake
# Writes the bytes of INPUT as giant::shaders::NAME().
file(READ ${INPUT} hex HEX)
string(LENGTH "${hex}" hex_length)
math(EXPR size "${hex_length} / 2")
if(size EQUAL 0)
  set(body "    return {};\n")
else()
  string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
  # Break the initialiser into lines of 32 bytes for compilers and diffs.
  string(REGEX REPLACE "((0x[0-9a-f][0-9a-f],){32})" "\\1\n        " bytes "${bytes}")
  set(body "    static const std::uint8_t bytes[${size}] = {\n        ${bytes}\n    };\n    return {bytes, sizeof(bytes)};\n")
endif()
file(WRITE ${OUTPUT}.tmp
  "// Generated from ${INPUT}; do not edit.\n"
  "#include <cstdint>\n#include <span>\n\n"
  "namespace giant::shaders {\n\n"
  "std::span<const std::uint8_t> ${NAME}() {\n${body}}\n\n"
  "} // namespace giant::shaders\n")
file(COPY_FILE ${OUTPUT}.tmp ${OUTPUT} ONLY_IF_DIFFERENT)
file(REMOVE ${OUTPUT}.tmp)
