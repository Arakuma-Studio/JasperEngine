# engine/core/voxel (T6): the pure half of the Voxel Volume module, with no ECS, render or Jolt include
# (V1). The kernels build with no contraction and no errno, so a block's bytes are the same on every
# target (R28), and outside unity batches, since those flags would reach their neighbours.
file(GLOB JASPERENGINE_VOXEL_SRCS CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_LIST_DIR}/*.cpp)

set(JASPERENGINE_VOXEL_FLAGS "")
if(MSVC)
    # /fp:precise (the default) does not contract
    list(APPEND JASPERENGINE_VOXEL_FLAGS /fp:precise)
else()
    # -fno-math-errno: sqrt compiles to the instruction alone; it changes no result
    list(APPEND JASPERENGINE_VOXEL_FLAGS -ffp-contract=off -fno-math-errno)
endif()
if(JASPERENGINE_VOXEL_SRCS)
    set_source_files_properties(${JASPERENGINE_VOXEL_SRCS} PROPERTIES COMPILE_OPTIONS "${JASPERENGINE_VOXEL_FLAGS}")
endif()
