# jasperengine-core-tests: unit tests of the engine core (engine/tests/core/<area>/*Test.cpp,
# mirroring engine/core/<area>), built by the standalone engine build with
# -DJASPERENGINE_TESTS=ON (tools/build.sh unit-tests). The engine is initialised without a GPU.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "jasperengine-core-tests is built on Linux only for now")
endif()

# googletest from the repository's yaml-cpp test tree (not shipped with the engine)
if(NOT TARGET gtest)
    set(JASPER_GTEST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../libs/yaml-cpp/test/gtest-1.11.0")
    set(CMAKE_POLICY_VERSION_MINIMUM 3.10)
    set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    add_subdirectory(${JASPER_GTEST_DIR} ${CMAKE_BINARY_DIR}/gtest EXCLUDE_FROM_ALL)
    target_compile_options(gtest PRIVATE -w)
endif()

file(GLOB CORE_TEST_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/*/*.cpp
)

add_executable(jasperengine-core-tests
    ${CORE_TEST_SOURCES}
    # the engine's System for tests (log capture, paths in the build directory)
    ${CMAKE_CURRENT_SOURCE_DIR}/../htmlui/harness/TestSystem.cpp
)

target_include_directories(jasperengine-core-tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../htmlui/harness
)

target_link_libraries(jasperengine-core-tests PRIVATE jasperengine gtest)
if(GRAPHIC_BACKEND STREQUAL "vulkan")
    target_link_libraries(jasperengine-core-tests PRIVATE Vulkan::Vulkan)
else()
    target_link_libraries(jasperengine-core-tests PRIVATE GL)
endif()

target_compile_definitions(jasperengine-core-tests PRIVATE
    JASPER_HTMLUI_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    JASPER_HTMLUI_TEST_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}"
    JASPER_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    JASPER_TEST_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}"
)

set_target_properties(jasperengine-core-tests PROPERTIES
    CXX_STANDARD ${JASPERENGINE_CXX_STANDARD}
    CXX_STANDARD_REQUIRED ON
)
