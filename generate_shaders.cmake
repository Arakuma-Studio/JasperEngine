# Convert the list string back to a CMake list
string(REPLACE " " ";" SHADER_FILES "${SHADER_FILES}")

get_filename_component(OUTPUT_DIR "${SHADERS_HEADER}" DIRECTORY)
file(MAKE_DIRECTORY "${OUTPUT_DIR}")

file(WRITE "${SHADERS_HEADER}" "#pragma once\n\n#include <string>\n#include <unordered_map>\n\nnamespace jasperengine::editor {\n\nconst std::unordered_map<std::string, std::string> shaderMap = {\n")

foreach(shader_file IN LISTS SHADER_FILES)
    file(RELATIVE_PATH shader_rel_path "${SHADERS_DIR}" "${shader_file}")
    file(TO_CMAKE_PATH "${shader_rel_path}" shader_rel_path)

    file(READ "${shader_file}" shader_content)

    # Escape special characters for C++ string literal
    string(REPLACE "\\" "\\\\" shader_content "${shader_content}")
    string(REPLACE "\"" "\\\"" shader_content "${shader_content}")
    string(REPLACE "\n" "\\n\"\n\"" shader_content "${shader_content}")

    file(APPEND "${SHADERS_HEADER}" "    {\"${shader_rel_path}\", \"${shader_content}\"},\n")
endforeach()

file(APPEND "${SHADERS_HEADER}" "};\n\n}\n")
