# Embeds jasperengine.manifest (DPI awareness, the UTF-8 code page) into a Windows
# executable. Included by the editor build, the standalone game build (separate top
# levels) and the test targets (engine/cmake/JasperTesting.cmake), so tests run in the
# code page users run in.

set(JASPERENGINE_WINDOWS_MANIFEST "${CMAKE_CURRENT_LIST_DIR}/jasperengine.manifest")

function(jasperengine_add_windows_manifest target_name)
    # the file next to this one, whichever scope included it and whichever calls
    set(JASPERENGINE_WINDOWS_MANIFEST "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/jasperengine.manifest")
    if(MSVC)
        # CMake passes a .manifest source to the linker's /MANIFESTINPUT, which
        # merges it with the one the linker generates
        target_sources(${target_name} PRIVATE ${JASPERENGINE_WINDOWS_MANIFEST})
    elseif(MINGW)
        # windres has no equivalent, so embed it as RT_MANIFEST (24) under
        # CREATEPROCESS_MANIFEST_RESOURCE_ID (1)
        set(generated_rc "${CMAKE_CURRENT_BINARY_DIR}/resources/${target_name}-manifest.rc")
        file(GENERATE OUTPUT ${generated_rc}
             CONTENT "1 24 \"${JASPERENGINE_WINDOWS_MANIFEST}\"\n")
        set_property(SOURCE ${generated_rc} PROPERTY GENERATED TRUE)
        set_property(SOURCE ${generated_rc} APPEND
                     PROPERTY OBJECT_DEPENDS ${JASPERENGINE_WINDOWS_MANIFEST})
        target_sources(${target_name} PRIVATE ${generated_rc})
    endif()
endfunction()
