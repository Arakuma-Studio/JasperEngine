# jasperengine-ecs-bench (tools/performance/ecs_bench.cpp) against the engine of the build it is
# included in: engine/CMakeLists.txt includes it with -DJASPERENGINE_ECS_BENCH=ON, and
# run-ecs-bench.py includes it into a checkout of the --baseline-ref engine, so both builds
# compile the same benchmark source.
get_filename_component(ECS_BENCH_DIR "${CMAKE_CURRENT_LIST_DIR}" ABSOLUTE)
add_executable(jasperengine-ecs-bench ${ECS_BENCH_DIR}/ecs_bench.cpp)
target_link_libraries(jasperengine-ecs-bench PRIVATE jasperengine)
if(GRAPHIC_BACKEND STREQUAL "vulkan")
    target_link_libraries(jasperengine-ecs-bench PRIVATE Vulkan::Vulkan)
else()
    target_link_libraries(jasperengine-ecs-bench PRIVATE GL)
endif()
# the engine's standard (an engine before S01 has no jasperengine_apply_cxx_standard)
if(COMMAND jasperengine_apply_cxx_standard)
    jasperengine_apply_cxx_standard(jasperengine-ecs-bench)
else()
    set_target_properties(jasperengine-ecs-bench PROPERTIES CXX_STANDARD ${JASPERENGINE_CXX_STANDARD} CXX_STANDARD_REQUIRED ON)
endif()
