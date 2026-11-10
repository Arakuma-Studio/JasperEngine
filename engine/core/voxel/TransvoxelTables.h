// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef TRANSVOXEL_TABLES_H
#define TRANSVOXEL_TABLES_H

// Eric Lengyel's Transvoxel tables (D14), through accessors, so the verbatim file stays in one
// translation unit: its `long` fields and global names go nowhere else.

#include <cstdint>

namespace jasperengine::voxel::tables {

    struct CellGeometry {
        int vertexCount = 0;
        int triangleCount = 0;
        const unsigned char* vertexIndex = nullptr;
    };

    // Regular cell: the 8-bit case index, one bit per corner (x = 1, y = 2, z = 4)
    CellGeometry regularCell(int caseCode);
    // Low byte: the edge's two corners, one per nibble
    const unsigned short* regularVertices(int caseCode);

    // Transition cell: the 9-bit case index. reversed: the class's high bit, which flips the winding.
    CellGeometry transitionCell(int caseCode, bool& reversed);
    const unsigned short* transitionVertices(int caseCode);

}

#endif
