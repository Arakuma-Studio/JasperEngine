# HarfBuzz 14.3.0's subsetter - https://github.com/harfbuzz/harfbuzz/ (COPYING, Old MIT)
#
# The editor subsets the fonts of Realtime and Glyph Atlas titles at export
# (editor/titleart/runtime/TitleFontSubsetter, docs/specs/S81-title-art-runtime.md R50, R54).
# Upstream's files added to the shaping sources one directory up: the hb-subset*.cc,
# hb-depend.cc and graph/ (the repacker) sources and the harfbuzz-subset.cc amalgamation, unchanged.
#
# The amalgamation compiles a complete HarfBuzz of its own (shaping included): the shaping build
# lives hidden inside libjasperengine, so the editor cannot link against it. Editor only
# (JASPERENGINE_TEXTURE_ENCODERS tree): games, the web and Android never compile it.

add_library(

    harfbuzz-subset

    STATIC

    harfbuzz-subset.cc

)

# this directory first: hb-repacker.hh one directory up includes "graph/graph.hh", and graph/'s
# "../hb-*.hh" resolve from here to the shaping sources
target_include_directories(harfbuzz-subset PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(harfbuzz-subset SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")

# Not HB_NO_MT: an export may subset fonts from job threads. No HB_EXPERIMENTAL_API either: the
# Reserved Font Name rename rewrites the name table after subsetting.
target_compile_definitions(
    harfbuzz-subset

    PRIVATE

    HB_NO_BUFFER_SERIALIZE
    HB_NO_BUFFER_VERIFY
)

# Internal like the shaping build: every hb_* symbol of this copy stays in the editor binary and
# never interposes on the system HarfBuzz that GTK loads through the file dialog
if(NOT MSVC)
    target_compile_options(harfbuzz-subset PRIVATE -fvisibility=hidden -fvisibility-inlines-hidden)
endif()
# (upstream's flags: hb-subset.cc passes the COFF section limit)
if(MSVC)
    target_compile_options(harfbuzz-subset PRIVATE /bigobj)
elseif(MINGW)
    target_compile_options(harfbuzz-subset PRIVATE -Wa,-mbig-obj)
endif()
