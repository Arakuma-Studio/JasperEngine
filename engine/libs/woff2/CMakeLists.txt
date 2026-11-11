# Google's WOFF2 decoder (MIT), pinned in README.jasper.md. No encoder or file I/O.
add_library(woff2 STATIC
    src/table_tags.cc
    src/variable_length.cc
    src/woff2_common.cc
    src/woff2_dec.cc
    src/woff2_out.cc
)
target_include_directories(woff2 SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(woff2 PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(woff2 PRIVATE brotlidec)
set_target_properties(woff2 PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(NOT MSVC)
    target_compile_options(woff2 PRIVATE -fvisibility=hidden -fvisibility-inlines-hidden)
endif()
if(EMSCRIPTEN AND EMSCRIPTEN_THREAD_SUPPORT)
    target_compile_options(woff2 PRIVATE -pthread)
endif()
