# The effect and generator presets (docs/specs/S83-title-effects-generators.md R7, §6.8):
# editor/resources/titleart/fx-presets/<type>.yaml embedded in the editor as
# ${GENERATED_DIR}/titleart_fx_presets.h (editor/titleart/fx/FxPresets.cpp, the only includer), so
# a preset applies the same in an installed editor, a headless one and the tests.
file(GLOB TITLE_ART_FX_PRESET_FILES CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/../../resources/titleart/fx-presets/*.yaml")
list(SORT TITLE_ART_FX_PRESET_FILES)
set(titleart_fx_presets_header "// Generated from editor/resources/titleart/fx-presets by editor/titleart/fx/FxPresets.cmake. Do not edit.\n#pragma once\n\n")
set(titleart_fx_presets_entries "")
set(titleart_fx_preset_index 0)
foreach(titleart_fx_preset IN LISTS TITLE_ART_FX_PRESET_FILES)
    get_filename_component(titleart_fx_preset_name "${titleart_fx_preset}" NAME)
    file(READ "${titleart_fx_preset}" titleart_fx_preset_hex HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," titleart_fx_preset_bytes "${titleart_fx_preset_hex}")
    string(APPEND titleart_fx_presets_header "static const unsigned char titleart_fx_preset_${titleart_fx_preset_index}[] = {${titleart_fx_preset_bytes}0x00};\n")
    string(APPEND titleart_fx_presets_entries "    { \"${titleart_fx_preset_name}\", titleart_fx_preset_${titleart_fx_preset_index}, sizeof(titleart_fx_preset_${titleart_fx_preset_index}) - 1 },\n")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${titleart_fx_preset}")
    math(EXPR titleart_fx_preset_index "${titleart_fx_preset_index} + 1")
endforeach()
string(APPEND titleart_fx_presets_header "\nstruct TitleArtFxPresetFile { const char* name; const unsigned char* data; unsigned int size; };\n")
string(APPEND titleart_fx_presets_header "static const TitleArtFxPresetFile titleart_fx_preset_files[] = {\n${titleart_fx_presets_entries}    { nullptr, nullptr, 0 }\n};\n")
file(WRITE "${GENERATED_DIR}/titleart_fx_presets.h.in" "${titleart_fx_presets_header}")
configure_file("${GENERATED_DIR}/titleart_fx_presets.h.in" "${GENERATED_DIR}/titleart_fx_presets.h" COPYONLY)
