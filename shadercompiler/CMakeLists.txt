cmake_minimum_required(VERSION 3.15)
project(shadercompiler)

option(ENABLE_HLSL "Enables HLSL input support" OFF)
option(ENABLE_OPT "Enables spirv-opt capability if present" ON)

set(SPIRV_CROSS_CLI OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_TESTS OFF CACHE BOOL "" FORCE)

if(ENABLE_HLSL)
    add_definitions(-DENABLE_HLSL)
endif()

if (MSVC)
	string(REPLACE "/GR" "" flags_without_rtti "${CMAKE_CXX_FLAGS}")
	set(CMAKE_CXX_FLAGS ${flags_without_rtti})
	set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS} /wd4530")
	
    if(JASPERENGINE_SHARED)
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    else()
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
endif()

add_subdirectory(libs/glslang)
add_subdirectory(libs/spirv-cross)
if(ENABLE_OPT)
    add_definitions(-DENABLE_OPT=1)
    include_directories ("${CMAKE_CURRENT_SOURCE_DIR}/libs/glslang/External/spirv-tools/include")
endif()
add_subdirectory(src)
