# Sources of the HTML/CSS compatibility layer (engine/core/htmlui/compat, phase 2), included
# by engine/CMakeLists.txt. Globbed per directory so the phase-2 packages add files without
# editing CMake (the Xcode project lists them explicitly).

set(HTMLUI_COMPAT_DIR ${CMAKE_CURRENT_LIST_DIR})

file(GLOB HTMLUI_COMPAT_SRCS CONFIGURE_DEPENDS
    ${HTMLUI_COMPAT_DIR}/*.cpp
    ${HTMLUI_COMPAT_DIR}/css/*.cpp
    ${HTMLUI_COMPAT_DIR}/css/props/*.cpp
    ${HTMLUI_COMPAT_DIR}/html/*.cpp
    ${HTMLUI_COMPAT_DIR}/html/transforms/*.cpp
    ${HTMLUI_COMPAT_DIR}/runtime/*.cpp
    ${HTMLUI_COMPAT_DIR}/tables/*.cpp
    ${HTMLUI_COMPAT_DIR}/ua/*.cpp
)
list(SORT HTMLUI_COMPAT_SRCS)

# The user-agent stylesheet and its SVGs (ua/), embedded as htmlui_ua.h. Ahead of shaders/,
# which holds a stand-in for builds that cannot generate it (the Xcode workspace; the Apple
# export writes the real one there).
function(jasperengine_htmlui_compat_embed target)
    set(header ${CMAKE_CURRENT_BINARY_DIR}/generated/htmlui/htmlui_ua.h)
    file(GLOB ua_files CONFIGURE_DEPENDS ${HTMLUI_COMPAT_DIR}/ua/*.css ${HTMLUI_COMPAT_DIR}/ua/*.svg)
    add_custom_command(
        OUTPUT ${header}
        COMMAND ${CMAKE_COMMAND}
                -DUA_DIR=${HTMLUI_COMPAT_DIR}/ua
                -DOUTPUT=${header}
                -P ${HTMLUI_COMPAT_DIR}/ua/embed_ua.cmake
        DEPENDS ${HTMLUI_COMPAT_DIR}/ua/embed_ua.cmake ${ua_files}
        COMMENT "Embedding the HTML UI user-agent files"
    )
    target_sources(${target} PRIVATE ${header})
    target_include_directories(${target} BEFORE PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated/htmlui)
endfunction()
