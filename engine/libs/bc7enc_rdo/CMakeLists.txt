# bc7enc_rdo - https://github.com/richgel999/bc7enc_rdo (master b9438627eef7), MIT or Unlicense
# (LICENSE). BC1/BC3/BC4/BC5 (rgbcx), BC7 (bc7enc) and the RDO entropy-reduction transform (ert)
# of the editor's texture cook (S14, README.jasper.md). Editor only, scalar (no ISPC).

add_library(

    bc7enc_rdo

    STATIC

    bc7enc.cpp
    bc7decomp.cpp
    rgbcx.cpp
    ert.cpp

)

target_include_directories(bc7enc_rdo SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# the same bytes whatever the compiler would fuse into multiply-adds on the target
target_compile_options(bc7enc_rdo PRIVATE
    $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>>:-ffp-contract=off -fno-strict-aliasing>
    $<$<CXX_COMPILER_ID:MSVC>:/fp:precise>)
set_target_properties(bc7enc_rdo PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    POSITION_INDEPENDENT_CODE ON
    FOLDER "libs")
