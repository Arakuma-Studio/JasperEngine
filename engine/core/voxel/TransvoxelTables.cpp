// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TransvoxelTables.h"

namespace jasperengine::voxel::tables {

    namespace lengyel {
        // The tables are data: their names, and `transitionCornerData` going unread, stay as upstream's.
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
#endif
#include "../../libs/transvoxel/Transvoxel.cpp"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
    }

    CellGeometry regularCell(int caseCode) {
        const lengyel::RegularCellData& data = lengyel::regularCellData[lengyel::regularCellClass[caseCode]];
        CellGeometry geometry;
        geometry.vertexCount = static_cast<int>(data.GetVertexCount());
        geometry.triangleCount = static_cast<int>(data.GetTriangleCount());
        geometry.vertexIndex = data.vertexIndex;
        return geometry;
    }

    const unsigned short* regularVertices(int caseCode) {
        return lengyel::regularVertexData[caseCode];
    }

    CellGeometry transitionCell(int caseCode, bool& reversed) {
        const unsigned char cellClass = lengyel::transitionCellClass[caseCode];
        reversed = (cellClass & 0x80) != 0;
        const lengyel::TransitionCellData& data = lengyel::transitionCellData[cellClass & 0x7F];
        CellGeometry geometry;
        geometry.vertexCount = static_cast<int>(data.GetVertexCount());
        geometry.triangleCount = static_cast<int>(data.GetTriangleCount());
        geometry.vertexIndex = data.vertexIndex;
        return geometry;
    }

    const unsigned short* transitionVertices(int caseCode) {
        return lengyel::transitionVertexData[caseCode];
    }

}
