add_library(shadercompiler STATIC
    args.cc
    input.cc
    glslang.cc
    spirvcross.cc
)

target_link_libraries(shadercompiler PRIVATE
    glslang
    OSDependent 
    SPIRV 
    SPVRemapper 
    spirv-cross-core 
    spirv-cross-cpp 
    spirv-cross-glsl 
    spirv-cross-hlsl 
    spirv-cross-reflect
    spirv-cross-util 
    spirv-cross-msl
)

set_target_properties(
    shadercompiler
    PROPERTIES
    CXX_STANDARD 17
)

install(TARGETS shadercompiler
        CONFIGURATIONS Release MinSizeRel
        ARCHIVE DESTINATION lib
        LIBRARY DESTINATION lib
        RUNTIME DESTINATION bin)
