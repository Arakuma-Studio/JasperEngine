add_library(

    sokol

    STATIC

    sokol.cpp

)

set(SOKOL_LINK_LIBRARIES)

if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        list(APPEND SOKOL_LINK_LIBRARIES
            user32 shell32 dxgi ole32 gdi32
        )
        if(GRAPHIC_BACKEND STREQUAL "d3d11")
            list(APPEND SOKOL_LINK_LIBRARIES
                d3d11
            )
        endif()
    endif()
    if(GRAPHIC_BACKEND STREQUAL "vulkan")
        if(TARGET Vulkan::Vulkan)
            list(APPEND SOKOL_LINK_LIBRARIES
                Vulkan::Vulkan
            )
        else()
            list(APPEND SOKOL_LINK_LIBRARIES
                vulkan-1
            )
        endif()
    elseif(GRAPHIC_BACKEND STREQUAL "glcore")
        # wglGetProcAddress (sokol_async_impl.h) and the GL 1.1 calls of whatever links sokol
        # (FrameReadback): a shared jasperengine links them itself
        list(APPEND SOKOL_LINK_LIBRARIES
            opengl32
        )
    endif()
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND SOKOL_LINK_LIBRARIES
        GL dl m pthread X11 Xi Xcursor
    )

    if(GRAPHIC_BACKEND STREQUAL "vulkan")
        if(TARGET Vulkan::Vulkan)
            list(APPEND SOKOL_LINK_LIBRARIES
                Vulkan::Vulkan
            )
        else()
            list(APPEND SOKOL_LINK_LIBRARIES
                vulkan
            )
        endif()
    endif()
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Darwin" OR CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set_source_files_properties(sokol.cpp PROPERTIES COMPILE_FLAGS "-x objective-c++")

    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fobjc-arc")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fobjc-arc")

    set_target_properties(
        sokol
        PROPERTIES
        XCODE_ATTRIBUTE_CLANG_ENABLE_OBJC_ARC "YES"
    )

    include_directories (/usr/local/include)
    link_directories(/usr/local/lib)

    if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        list(APPEND SOKOL_LINK_LIBRARIES "-framework Cocoa")
        if(GRAPHIC_BACKEND STREQUAL "glcore")
            list(APPEND SOKOL_LINK_LIBRARIES "-framework OpenGL")
        elseif(GRAPHIC_BACKEND STREQUAL "metal")
            list(APPEND SOKOL_LINK_LIBRARIES "-framework Metal -framework MetalKit -framework Quartz")
        elseif(GRAPHIC_BACKEND STREQUAL "vulkan")
            if(TARGET Vulkan::Vulkan)
                list(APPEND SOKOL_LINK_LIBRARIES
                    Vulkan::Vulkan
                )
            else()
                list(APPEND SOKOL_LINK_LIBRARIES
                    vulkan
                )
            endif()
        endif()
    elseif(CMAKE_SYSTEM_NAME STREQUAL "iOS")
        list(APPEND SOKOL_LINK_LIBRARIES "-framework UIKit")
        if(GRAPHIC_BACKEND STREQUAL "metal")
            list(APPEND SOKOL_LINK_LIBRARIES "-framework Metal -framework MetalKit")
        endif()
    endif()
endif()

find_package(Threads REQUIRED)
target_link_libraries(sokol PRIVATE Threads::Threads)

if(EMSCRIPTEN AND EMSCRIPTEN_THREAD_SUPPORT)
	target_compile_options(sokol PRIVATE -pthread)
endif()

target_link_libraries(
    sokol

    PUBLIC

    ${SOKOL_LINK_LIBRARIES}
)
