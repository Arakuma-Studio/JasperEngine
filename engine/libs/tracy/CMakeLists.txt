# The Tracy client (https://github.com/wolfpld/tracy, v0.11.1, BSD-3-Clause, LICENSE): only
# with JASPERENGINE_TRACY (desktop and Android), where the profiler's zones, frame marks and
# plots go to a Tracy server too (docs/profiling.md). Vendored unchanged: public/ of the release.

add_library(TracyClient STATIC TracyClient.cpp)
target_include_directories(TracyClient SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(TracyClient PUBLIC TRACY_ENABLE)
# a vendored library: its warnings are not ours
if(MSVC)
    target_compile_options(TracyClient PRIVATE /w)
else()
    target_compile_options(TracyClient PRIVATE -w)
endif()
find_package(Threads REQUIRED)
target_link_libraries(TracyClient PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
if(WIN32)
    target_link_libraries(TracyClient PUBLIC ws2_32 dbghelp)
elseif(ANDROID)
    target_link_libraries(TracyClient PUBLIC log)
endif()
