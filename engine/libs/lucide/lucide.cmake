# The built-in Lucide icon pack (phase-2 §3.11.8), included by engine/CMakeLists.txt:
# jasperengine_htmlui_lucide_embed(<target>) generates htmlui_lucide.h from lucide.pack in the
# target's build directory, ahead of engine/shaders/ (which holds an empty stand-in for builds
# without the step: the Xcode workspace, where the Apple export writes the real header).

set(HTMLUI_LUCIDE_DIR ${CMAKE_CURRENT_LIST_DIR})

function(jasperengine_htmlui_lucide_embed target)
    set(header ${CMAKE_CURRENT_BINARY_DIR}/generated/htmlui/htmlui_lucide.h)
    add_custom_command(
        OUTPUT ${header}
        COMMAND ${CMAKE_COMMAND}
                -DPACK=${HTMLUI_LUCIDE_DIR}/lucide.pack
                -DOUTPUT=${header}
                -P ${HTMLUI_LUCIDE_DIR}/embed_lucide.cmake
        DEPENDS ${HTMLUI_LUCIDE_DIR}/embed_lucide.cmake ${HTMLUI_LUCIDE_DIR}/lucide.pack
        COMMENT "Embedding the Lucide icon pack"
    )
    target_sources(${target} PRIVATE ${header})
    target_include_directories(${target} BEFORE PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated/htmlui)
endfunction()
