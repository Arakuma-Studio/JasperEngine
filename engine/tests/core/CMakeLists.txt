# jasperengine-core-tests: unit tests of engine core against the static engine, built by the
# standalone engine build with -DJASPERENGINE_TESTS=ON (tools/build.sh unit-tests). Tests are
# <Subject>Test.cpp files under engine/tests/core/<area>/, mirroring engine/core/<area>/.
if(NOT TARGET gtest)
    # googletest from the repository's yaml-cpp test tree (as engine/tests/htmlui)
    set(JASPER_GTEST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../libs/yaml-cpp/test/gtest-1.11.0")
    set(CMAKE_POLICY_VERSION_MINIMUM 3.10)
    set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    add_subdirectory(${JASPER_GTEST_DIR} ${CMAKE_BINARY_DIR}/gtest EXCLUDE_FROM_ALL)
    target_compile_options(gtest PRIVATE ${EXTERNAL_WARNING_FLAG})
    target_compile_options(gtest_main PRIVATE ${EXTERNAL_WARNING_FLAG})
    jasperengine_apply_cxx_standard(gtest gtest_main)
endif()

file(GLOB_RECURSE CORE_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*Test.cpp)

add_executable(jasperengine-core-tests ${CORE_TEST_SOURCES})
target_include_directories(jasperengine-core-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(jasperengine-core-tests PRIVATE jasperengine gtest gtest_main)
target_compile_definitions(jasperengine-core-tests PRIVATE
    JASPER_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    JASPER_TEST_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}"
)
jasperengine_apply_cxx_standard(jasperengine-core-tests)
