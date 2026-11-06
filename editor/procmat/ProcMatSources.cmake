# The Procedural Material (docs/specs/S90-procedural-material.md): the node library's descriptors embedded
# in the editor (editor/procmat/ProceduralMaterialRegistration.cpp, the only includer), the generator,
# the planner and the evaluator on the editor's context. SpirvInterpreter.cpp joins when the CPU tier does:
# until then only the tests and tools/procmat/sweep link it.

set(PROCMAT_SOURCES
    ${EDITOR_DIR}/procmat/NodeLibrary.cpp
    ${EDITOR_DIR}/procmat/ProcMatExpander.cpp
    ${EDITOR_DIR}/procmat/ProceduralMaterialKind.cpp
    ${EDITOR_DIR}/procmat/ProceduralMaterialRegistration.cpp
    ${EDITOR_DIR}/procmat/PassPlanner.cpp
    ${EDITOR_DIR}/procmat/ProceduralSession.cpp
    ${EDITOR_DIR}/procmat/ProceduralGpuEvaluator.cpp
)
list(APPEND JASPERENGINE_EDITOR_SOURCES ${PROCMAT_SOURCES})
jasperengine_skip_unity_build(
    REASON "the Procedural Material: file-local helpers named as in other editor sources, and the embedded library"
    SOURCES ${PROCMAT_SOURCES})

set(PROCMAT_NODES_DIR "${CMAKE_CURRENT_SOURCE_DIR}/${EDITOR_DIR}/procmat/nodes")
file(GLOB PROCMAT_NODE_FILES CONFIGURE_DEPENDS "${PROCMAT_NODES_DIR}/*.json")
list(SORT PROCMAT_NODE_FILES)
# the importer's record of what it left out: nothing reads it at run time
list(FILTER PROCMAT_NODE_FILES EXCLUDE REGEX "/_manifest\\.json$")
set(procmat_nodes_header "// Generated from editor/procmat/nodes by editor/procmat/ProcMatSources.cmake. Do not edit.\n#pragma once\n\n")
set(procmat_nodes_entries "")
set(procmat_node_index 0)
foreach(procmat_node_file IN LISTS PROCMAT_NODE_FILES)
    get_filename_component(procmat_node_name "${procmat_node_file}" NAME)
    file(READ "${procmat_node_file}" procmat_node_hex HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," procmat_node_bytes "${procmat_node_hex}")
    string(APPEND procmat_nodes_header "static const unsigned char procmat_node_${procmat_node_index}[] = {${procmat_node_bytes}0x00};\n")
    string(APPEND procmat_nodes_entries "    { \"${procmat_node_name}\", procmat_node_${procmat_node_index}, sizeof(procmat_node_${procmat_node_index}) - 1 },\n")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${procmat_node_file}")
    math(EXPR procmat_node_index "${procmat_node_index} + 1")
endforeach()
string(APPEND procmat_nodes_header "\nstruct ProcMatEmbeddedNode { const char* name; const unsigned char* data; unsigned int size; };\n")
string(APPEND procmat_nodes_header "static const ProcMatEmbeddedNode procmat_embedded_nodes[] = {\n${procmat_nodes_entries}    { nullptr, nullptr, 0 }\n};\n")
file(WRITE "${GENERATED_DIR}/procmat_nodes.h.in" "${procmat_nodes_header}")
configure_file("${GENERATED_DIR}/procmat_nodes.h.in" "${GENERATED_DIR}/procmat_nodes.h" COPYONLY)
