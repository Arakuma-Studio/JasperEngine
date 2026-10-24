# jasperengine-texsynth-tests: the texture synthesis core of editor/texsynth on generated images
# (determinism across thread counts, the modes, companions, the recipe chunk, the k-NN). Included
# by the root CMakeLists.txt with -DJASPERENGINE_TEXSYNTH_TESTS=ON (include(), not
# add_subdirectory(): the editor's relative paths keep resolving against the root). The core needs
# neither the engine nor a window: its sources, stb and googletest only. Run by
# tools/build.sh texsynth-tests, or:
#   cmake -S . -B build/linux/editor -DJASPERENGINE_TEXSYNTH_TESTS=ON
#   cmake --build build/linux/editor --target jasperengine-texsynth-tests && build/linux/editor/jasperengine-texsynth-tests

# googletest from the repository's yaml-cpp test tree (as engine/tests/htmlui and the HTML UI editor tests)
if(NOT TARGET gtest)
    set(CMAKE_POLICY_VERSION_MINIMUM 3.10)
    set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    add_subdirectory(${CMAKE_SOURCE_DIR}/libs/yaml-cpp/test/gtest-1.11.0 ${CMAKE_BINARY_DIR}/gtest-editor EXCLUDE_FROM_ALL)
    target_compile_options(gtest PRIVATE -w)
    target_compile_options(gtest_main PRIVATE -w)
endif()

find_package(Threads REQUIRED)

add_executable(jasperengine-texsynth-tests
    ${JASPERENGINE_TEXSYNTH_SOURCES}
    ${EDITOR_DIR}/util/PngWriter.cpp
    ${CMAKE_CURRENT_LIST_DIR}/TexSynthTest.cpp
)
target_include_directories(jasperengine-texsynth-tests PRIVATE ${CMAKE_SOURCE_DIR}/${EDITOR_DIR} ${GENERATED_DIR})
target_link_libraries(jasperengine-texsynth-tests PRIVATE gtest_main gtest stb Threads::Threads)
set_target_properties(jasperengine-texsynth-tests PROPERTIES
    CXX_STANDARD ${JASPERENGINE_CXX_STANDARD}
    CXX_STANDARD_REQUIRED ON
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)
# the synthesis sources compile warning-free with the usual warnings on
if(NOT MSVC)
    target_compile_options(jasperengine-texsynth-tests PRIVATE -Wall -Wextra -Wshadow)
endif()
