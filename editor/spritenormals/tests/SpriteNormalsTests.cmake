# jasperengine-spritenormals-tests: the sprite normal generator core of editor/spritenormals
# (upstream parity on golden fixtures, the advanced options, determinism across thread counts,
# GIF/APNG decoding, the recipe chunk, the lit preview). Included by the root CMakeLists.txt with
# -DJASPERENGINE_SPRITENORMALS_TESTS=ON (include(), not add_subdirectory(): the editor's relative
# paths keep resolving against the root). The core needs neither the engine nor a window: its
# sources, the texsynth image code, stb and googletest only. Run by tools/build.sh
# spritenormals-tests, or:
#   cmake -S . -B build/linux/editor -DJASPERENGINE_SPRITENORMALS_TESTS=ON
#   cmake --build build/linux/editor --target jasperengine-spritenormals-tests && build/linux/editor/jasperengine-spritenormals-tests
# Tests are tests/*Test.cpp; fixtures are read from tests/fixtures (SPRITENORMALS_FIXTURES).
# The golden outputs come only from the upstream binary, through tests/fixtures/make_fixtures.py
# (tests/fixtures/README.md); no test writes them.

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

file(GLOB SPRITENORMALS_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_LIST_DIR}/*Test.cpp)
add_executable(jasperengine-spritenormals-tests
    ${JASPERENGINE_SPRITENORMALS_SOURCES}
    ${JASPERENGINE_TEXSYNTH_SOURCES}
    ${EDITOR_DIR}/util/PngWriter.cpp
    ${SPRITENORMALS_TEST_SOURCES}
)
target_include_directories(jasperengine-spritenormals-tests PRIVATE ${CMAKE_SOURCE_DIR}/${EDITOR_DIR} ${GENERATED_DIR})
target_link_libraries(jasperengine-spritenormals-tests PRIVATE gtest_main gtest stb Threads::Threads)
target_compile_definitions(jasperengine-spritenormals-tests PRIVATE SPRITENORMALS_FIXTURES="${CMAKE_CURRENT_LIST_DIR}/fixtures")
set_target_properties(jasperengine-spritenormals-tests PROPERTIES
    CXX_STANDARD ${JASPERENGINE_CXX_STANDARD}
    CXX_STANDARD_REQUIRED ON
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)
# the generator sources compile warning-free with the usual warnings on
if(NOT MSVC)
    target_compile_options(jasperengine-spritenormals-tests PRIVATE -Wall -Wextra -Wshadow)
endif()
