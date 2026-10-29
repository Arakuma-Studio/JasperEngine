// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The flat cell arrays and the odd-r hex neighbour offsets follow OpenEngine's GridMap and
// HexGridHelpers (Engine/Modules/Pathfinding/Source/GridMap.cpp, HexGridHelpers.cpp), Copyright
// (c) 2026 Gamecrafters Guild Limited and contributors, MIT License. See NOTICE.md.

#include "NavGridMap.h"
#include "NavHash.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <numeric>

using namespace jasperengine;
using namespace jasperengine::nav;

namespace {

    constexpr float SQRT3 = 1.7320508075688772f;
    constexpr float SQRT2 = 1.4142135623730951f;
    constexpr char MAGIC[4] = {'J', 'N', 'G', 'R'};

    // odd-r offsets: even rows, then odd rows (east, north-east, north-west, west, south-west,
    // south-east, with y growing "south")
    constexpr int HEX_EVEN[6][2] = {{+1, 0}, {0, -1}, {-1, -1}, {-1, 0}, {-1, +1}, {0, +1}};
    constexpr int HEX_ODD[6][2] = {{+1, 0}, {+1, -1}, {0, -1}, {-1, 0}, {0, +1}, {+1, +1}};
    constexpr int SQUARE[8][2] = {{+1, 0}, {0, +1}, {-1, 0}, {0, -1}, {+1, +1}, {-1, +1}, {-1, -1}, {+1, -1}};

    struct Cube {
        int q, r, s;
    };

    Cube toCube(GridCell cell) {
        const int q = cell.x - (cell.y - (cell.y & 1)) / 2;
        const int r = cell.y;
        return Cube{q, r, -q - r};
    }

    int findRoot(std::vector<uint32_t>& parent, uint32_t i) {
        while (parent[i] != i) {
            parent[i] = parent[parent[i]];
            i = parent[i];
        }
        return (int)i;
    }

}

NavGridMap::NavGridMap() = default;

void NavGridMap::reset(int newWidth, int newHeight, uint8_t area) {
    width = std::max(0, newWidth);
    height = std::max(0, newHeight);
    const size_t cells = (size_t)width * (size_t)height;
    areas.assign(cells, std::min<uint8_t>(area, AREA_COUNT - 1));
    overrideBits.assign((cells + 7) / 8, 0);
    heights.clear();
    components.clear();
    componentsStale = true;
    edited.clear();
    editedAll = true;
    revision++;
}

void NavGridMap::setLayout(GridLayout newLayout, int newNeighbours) {
    layout = newLayout;
    neighbourMode = newNeighbours == 4 ? 4 : 8;
    componentsStale = true;
    editedAll = true;
    revision++;
}

void NavGridMap::setCellSize(float size) {
    cellSize = size > 0.0f && std::isfinite(size) ? size : 1.0f;
    revision++;
}

void NavGridMap::setSlopeLimits(float maxSlopeDegrees, float step) {
    maxSlope = std::clamp(std::isfinite(maxSlopeDegrees) ? maxSlopeDegrees : 45.0f, 0.0f, 90.0f);
    maxStep = std::max(0.0f, std::isfinite(step) ? step : 0.5f);
    componentsStale = true;
    editedAll = true;
    revision++;
}

void NavGridMap::touched(int cell) {
    componentsStale = true;
    if (!editedAll)
        edited.push_back(cell);
    revision++;
}

bool NavGridMap::setArea(GridCell cell, uint8_t area) {
    if (!contains(cell) || area >= AREA_COUNT)
        return false;
    const int i = index(cell);
    if (areas[(size_t)i] != area) {
        areas[(size_t)i] = area;
        touched(i);
    }
    return true;
}

int NavGridMap::fillRect(GridCell a, GridCell b, uint8_t area) {
    if (area >= AREA_COUNT || width == 0 || height == 0)
        return 0;
    const int x0 = std::clamp(std::min(a.x, b.x), 0, width - 1), x1 = std::clamp(std::max(a.x, b.x), 0, width - 1);
    const int y0 = std::clamp(std::min(a.y, b.y), 0, height - 1), y1 = std::clamp(std::max(a.y, b.y), 0, height - 1);
    if (std::max(a.x, b.x) < 0 || std::min(a.x, b.x) >= width || std::max(a.y, b.y) < 0 || std::min(a.y, b.y) >= height)
        return 0;
    int changed = 0;
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            const int i = y * width + x;
            if (areas[(size_t)i] != area) {
                areas[(size_t)i] = area;
                touched(i);
                changed++;
            }
        }
    }
    return changed;
}

bool NavGridMap::getOverride(GridCell cell) const {
    if (!contains(cell))
        return false;
    const int i = index(cell);
    return (overrideBits[(size_t)i >> 3] >> (i & 7)) & 1;
}

void NavGridMap::setOverride(GridCell cell, bool value) {
    if (!contains(cell))
        return;
    const int i = index(cell);
    if (value)
        overrideBits[(size_t)i >> 3] |= (uint8_t)(1u << (i & 7));
    else
        overrideBits[(size_t)i >> 3] &= (uint8_t)~(1u << (i & 7));
    revision++;
}

void NavGridMap::clearOverrides() {
    std::fill(overrideBits.begin(), overrideBits.end(), 0);
    revision++;
}

int NavGridMap::countOverrides() const {
    int count = 0;
    for (uint8_t bits : overrideBits)
        count += std::popcount((unsigned int)bits);
    return count;
}

void NavGridMap::setHasHeights(bool value) {
    if (value && heights.empty())
        heights.assign(areas.size(), 0.0f);
    else if (!value)
        heights.clear();
    componentsStale = true;
    editedAll = true;
    revision++;
}

void NavGridMap::setHeight(GridCell cell, float value) {
    if (!contains(cell))
        return;
    if (heights.empty())
        heights.assign(areas.size(), 0.0f);
    const int i = index(cell);
    if (heights[(size_t)i] != value) {
        heights[(size_t)i] = value;
        touched(i);
    }
}

Vector2 NavGridMap::cellCenter(GridCell cell) const {
    if (layout == GridLayout::Hex) {
        // pointy-top: width = cellSize (the distance between neighbouring centres), size (centre to
        // corner) = cellSize / sqrt(3), rows 1.5 * size apart, odd rows half a cell right
        const float size = cellSize / SQRT3;
        return Vector2(cellSize * ((float)cell.x + 0.5f + 0.5f * (float)(cell.y & 1)), size + 1.5f * size * (float)cell.y);
    }
    return Vector2(((float)cell.x + 0.5f) * cellSize, ((float)cell.y + 0.5f) * cellSize);
}

GridCell NavGridMap::cellAt(const Vector2& point) const {
    if (layout == GridLayout::Hex) {
        const float size = cellSize / SQRT3;
        // relative to the centre of (0, 0), then axial (pointy-top), then cube rounding
        const float px = point.x - cellSize * 0.5f;
        const float py = point.y - size;
        const double q = ((double)SQRT3 / 3.0 * (double)px - 1.0 / 3.0 * (double)py) / (double)size;
        const double r = (2.0 / 3.0 * (double)py) / (double)size;
        const double s = -q - r;
        double rq = std::round(q), rr = std::round(r), rs = std::round(s);
        const double dq = std::fabs(rq - q), dr = std::fabs(rr - r), ds = std::fabs(rs - s);
        if (dq > dr && dq > ds)
            rq = -rr - rs;
        else if (dr > ds)
            rr = -rq - rs;
        const int iq = (int)rq, ir = (int)rr;
        return GridCell{iq + (ir - (ir & 1)) / 2, ir};
    }
    return GridCell{(int)std::floor(point.x / cellSize), (int)std::floor(point.y / cellSize)};
}

Vector2 NavGridMap::getSize() const {
    if (layout == GridLayout::Hex) {
        const float size = cellSize / SQRT3;
        return Vector2(cellSize * ((float)width + (height > 1 ? 0.5f : 0.0f)), height > 0 ? size * (2.0f + 1.5f * (float)(height - 1)) : 0.0f);
    }
    return Vector2((float)width * cellSize, (float)height * cellSize);
}

int NavGridMap::cellCorners(GridCell cell, Vector2* out) const {
    const Vector2 c = cellCenter(cell);
    if (layout == GridLayout::Hex) {
        const float size = cellSize / SQRT3;
        for (int i = 0; i < 6; i++) {
            // pointy-top: corners at 30 + 60 * i degrees
            const float angle = (60.0f * (float)i - 30.0f) * 3.14159265358979323846f / 180.0f;
            out[i] = Vector2(c.x + size * std::cos(angle), c.y + size * std::sin(angle));
        }
        return 6;
    }
    const float h = cellSize * 0.5f;
    out[0] = Vector2(c.x - h, c.y - h);
    out[1] = Vector2(c.x + h, c.y - h);
    out[2] = Vector2(c.x + h, c.y + h);
    out[3] = Vector2(c.x - h, c.y + h);
    return 4;
}

int NavGridMap::neighbours(GridCell cell, GridCell* out, float* stepLengths) const {
    int count = 0;
    if (layout == GridLayout::Hex) {
        const int (*offsets)[2] = (cell.y & 1) ? HEX_ODD : HEX_EVEN;
        for (int i = 0; i < 6; i++) {
            const GridCell next{cell.x + offsets[i][0], cell.y + offsets[i][1]};
            if (!contains(next))
                continue;
            out[count] = next;
            stepLengths[count] = 1.0f;
            count++;
        }
        return count;
    }
    const int mode = neighbourMode == 4 ? 4 : 8;
    for (int i = 0; i < mode; i++) {
        const GridCell next{cell.x + SQUARE[i][0], cell.y + SQUARE[i][1]};
        if (!contains(next))
            continue;
        out[count] = next;
        stepLengths[count] = i < 4 ? 1.0f : SQRT2;
        count++;
    }
    return count;
}

bool NavGridMap::stepAllowed(int from, int to, float stepLength) const {
    if (heights.empty())
        return true;
    const float rise = std::fabs(heights[(size_t)to] - heights[(size_t)from]);
    if (rise > maxStep) {
        // a slope within the limit may rise more than a step over a cell
        const float run = stepLength * cellSize;
        const float slope = std::atan2(rise, run) * 180.0f / 3.14159265358979323846f;
        return slope <= maxSlope;
    }
    return true;
}

int NavGridMap::hexDistance(GridCell a, GridCell b) const {
    const Cube ca = toCube(a), cb = toCube(b);
    return (std::abs(ca.q - cb.q) + std::abs(ca.r - cb.r) + std::abs(ca.s - cb.s)) / 2;
}

float NavGridMap::heuristic(GridCell a, GridCell b) const {
    if (layout == GridLayout::Hex)
        return (float)hexDistance(a, b);
    const int dx = std::abs(a.x - b.x), dy = std::abs(a.y - b.y);
    if (neighbourMode == 4)
        return (float)(dx + dy);
    // octile
    return (float)(dx + dy) + (SQRT2 - 2.0f) * (float)std::min(dx, dy);
}

void NavGridMap::ensureComponents() const {
    if (!componentsStale && components.size() == areas.size())
        return;
    const size_t cells = areas.size();
    std::vector<uint32_t> parent(cells);
    std::iota(parent.begin(), parent.end(), 0u);
    auto unite = [&](uint32_t a, uint32_t b) {
        const int ra = findRoot(parent, a), rb = findRoot(parent, b);
        if (ra != rb)
            parent[(size_t)std::max(ra, rb)] = (uint32_t)std::min(ra, rb);
    };
    GridCell next[MAX_NEIGHBOURS];
    float steps[MAX_NEIGHBOURS];
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            const int i = y * width + x;
            if (!isWalkable(i))
                continue;
            const int count = neighbours(GridCell{x, y}, next, steps);
            for (int n = 0; n < count; n++) {
                const int j = index(next[n]);
                if (j <= i || !isWalkable(j))
                    continue;
                // a diagonal never cuts a corner here: it joins cells the orthogonal steps join
                if (layout == GridLayout::Square && steps[n] > 1.0f)
                    continue;
                if (stepAllowed(i, j, steps[n]) && stepAllowed(j, i, steps[n]))
                    unite((uint32_t)i, (uint32_t)j);
            }
        }
    }
    components.assign(cells, UINT32_MAX);
    for (size_t i = 0; i < cells; i++) {
        if (isWalkable((int)i))
            components[i] = (uint32_t)findRoot(parent, (uint32_t)i);
    }
    componentsStale = false;
}

uint32_t NavGridMap::getComponent(GridCell cell) const {
    if (!contains(cell))
        return UINT32_MAX;
    ensureComponents();
    return components[(size_t)index(cell)];
}

bool NavGridMap::isReachable(GridCell a, GridCell b) const {
    if (!contains(a) || !contains(b))
        return false;
    ensureComponents();
    const uint32_t ca = components[(size_t)index(a)], cb = components[(size_t)index(b)];
    return ca != UINT32_MAX && ca == cb;
}

std::vector<int> NavGridMap::takeEditedCells() {
    std::vector<int> result;
    if (editedAll) {
        result.resize(areas.size());
        std::iota(result.begin(), result.end(), 0);
    } else {
        std::sort(edited.begin(), edited.end());
        edited.erase(std::unique(edited.begin(), edited.end()), edited.end());
        result.swap(edited);
    }
    edited.clear();
    editedAll = false;
    return result;
}

size_t NavGridMap::memoryBytes() const {
    return areas.capacity() + overrideBits.capacity() + heights.capacity() * sizeof(float) + components.capacity() * sizeof(uint32_t);
}

std::vector<uint8_t> NavGridMap::write() const {
    std::vector<uint8_t> out;
    auto u8 = [&](uint8_t v) { out.push_back(v); };
    auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; i++) out.push_back((uint8_t)(v >> (8 * i))); };
    auto f32 = [&](float v) {
        uint32_t bits;
        std::memcpy(&bits, &v, 4);
        u32(bits);
    };
    out.insert(out.end(), MAGIC, MAGIC + 4);
    u32(FILE_VERSION);
    u8((uint8_t)layout);
    u8((uint8_t)neighbourMode);
    u8(heights.empty() ? 0 : 1);
    u8(0);
    u32((uint32_t)width);
    u32((uint32_t)height);
    f32(cellSize);
    f32(maxSlope);
    f32(maxStep);
    out.insert(out.end(), areas.begin(), areas.end());
    out.insert(out.end(), overrideBits.begin(), overrideBits.end());
    for (float h : heights)
        f32(h);
    const uint64_t checksum = hashBytes(out.data(), out.size());
    for (int i = 0; i < 8; i++)
        out.push_back((uint8_t)(checksum >> (8 * i)));
    return out;
}

bool NavGridMap::read(const uint8_t* bytes, size_t size, std::string& error) {
    if constexpr (std::endian::native != std::endian::little) {
        error = "navgrid data is little-endian and this platform is big-endian";
        return false;
    }
    constexpr size_t HEADER = 4 + 4 + 4 + 4 + 4 + 12;
    if (size < HEADER + 8 || std::memcmp(bytes, MAGIC, 4) != 0) {
        error = "not a navgrid file (no JNGR magic)";
        return false;
    }
    auto u32At = [&](size_t offset) {
        uint32_t v = 0;
        for (int i = 0; i < 4; i++) v |= (uint32_t)bytes[offset + (size_t)i] << (8 * i);
        return v;
    };
    auto f32At = [&](size_t offset) {
        const uint32_t bits = u32At(offset);
        float v;
        std::memcpy(&v, &bits, 4);
        return v;
    };
    const uint32_t version = u32At(4);
    if (version != FILE_VERSION) {
        error = version > FILE_VERSION ? "navgrid format version " + std::to_string(version) + " is newer than this engine reads (" +
                                             std::to_string(FILE_VERSION) + ")"
                                       : "unknown navgrid format version " + std::to_string(version);
        return false;
    }
    uint64_t stored = 0;
    for (int i = 0; i < 8; i++)
        stored |= (uint64_t)bytes[size - 8 + (size_t)i] << (8 * i);
    if (stored != hashBytes(bytes, size - 8)) {
        error = "the navgrid file is damaged (its checksum does not match)";
        return false;
    }
    const uint8_t fileLayout = bytes[8];
    const uint8_t fileNeighbours = bytes[9];
    const bool fileHeights = bytes[10] != 0;
    const uint32_t fileWidth = u32At(12);
    const uint32_t fileHeight = u32At(16);
    const float fileCellSize = f32At(20);
    const float fileSlope = f32At(24);
    const float fileStep = f32At(28);
    if (fileLayout > 1 || (fileNeighbours != 4 && fileNeighbours != 8) || fileWidth > 16384 || fileHeight > 16384 ||
        !(fileCellSize > 0.0f) || !std::isfinite(fileCellSize)) {
        error = "the navgrid header is invalid";
        return false;
    }
    const size_t cells = (size_t)fileWidth * (size_t)fileHeight;
    const size_t expected = HEADER + cells + (cells + 7) / 8 + (fileHeights ? cells * 4 : 0) + 8;
    if (size != expected) {
        error = "the navgrid file is truncated or has trailing bytes";
        return false;
    }
    const uint8_t* cellData = bytes + HEADER;
    for (size_t i = 0; i < cells; i++) {
        if (cellData[i] >= AREA_COUNT) {
            error = "the navgrid file has a cell with an unknown area";
            return false;
        }
    }
    width = (int)fileWidth;
    height = (int)fileHeight;
    layout = (GridLayout)fileLayout;
    neighbourMode = fileNeighbours;
    cellSize = fileCellSize;
    maxSlope = fileSlope;
    maxStep = fileStep;
    areas.assign(cellData, cellData + cells);
    overrideBits.assign(cellData + cells, cellData + cells + (cells + 7) / 8);
    heights.clear();
    if (fileHeights) {
        heights.resize(cells);
        const size_t base = HEADER + cells + (cells + 7) / 8;
        for (size_t i = 0; i < cells; i++)
            heights[i] = f32At(base + i * 4);
    }
    components.clear();
    componentsStale = true;
    edited.clear();
    editedAll = true;
    revision++;
    return true;
}
