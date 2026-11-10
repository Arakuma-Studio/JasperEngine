// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "tilemap/TileCells.h"

#include "util/Base64.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>

using namespace jasperengine;

namespace {

    constexpr uint8_t CELLS_HEADER_SIZE_MASK = 0x0F;
    constexpr uint8_t CELLS_HEADER_TERRAIN = 0x10;
    constexpr uint8_t CELLS_HEADER_RESERVED = 0xE0;

    // one clock for every store: a revision names one write, whichever copy of a store made it
    std::atomic<uint64_t> cellsRevisionClock{0};

    uint64_t cellsNextRevision(){
        return cellsRevisionClock.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    int cellsShiftOf(int size){
        int shift = 0;
        while ((1 << shift) < size)
            shift++;
        return shift;
    }

    size_t cellsLocalIndex(CellCoord cell, int size){
        const int mask = size - 1;
        return (size_t)(cell.x & mask) + (size_t)(cell.y & mask) * (size_t)size;
    }

    void cellsPutVarint(std::string& out, uint32_t value){
        while (value >= 0x80){
            out.push_back((char)(uint8_t)(value | 0x80));
            value >>= 7;
        }
        out.push_back((char)(uint8_t)value);
    }

    bool cellsGetVarint(std::string_view data, size_t& pos, uint32_t& value){
        value = 0;
        for (int i = 0; i < 5; i++){
            if (pos >= data.size())
                return false;
            const uint8_t byte = (uint8_t)data[pos++];
            if (i == 4 && (byte & 0xF0) != 0)
                return false;
            value |= (uint32_t)(byte & 0x7F) << (7 * i);
            if ((byte & 0x80) == 0)
                return true;
        }
        return false;
    }

    template<class T>
    void cellsPutRuns(std::string& out, const std::vector<T>& values){
        size_t i = 0;
        while (i < values.size()){
            size_t end = i + 1;
            while (end < values.size() && values[end] == values[i])
                end++;
            cellsPutVarint(out, (uint32_t)(end - i));
            uint32_t value = values[i];
            for (size_t b = 0; b < sizeof(T); b++){
                out.push_back((char)(uint8_t)value);
                value >>= 8;
            }
            i = end;
        }
    }

    template<class T>
    bool cellsGetRuns(std::string_view data, size_t& pos, std::vector<T>& values, const char* what, std::string& error){
        size_t filled = 0;
        while (filled < values.size()){
            uint32_t count = 0;
            const size_t runStart = pos;
            if (!cellsGetVarint(data, pos, count)){
                error = std::string(what) + ": bad run length at byte " + std::to_string(runStart);
                return false;
            }
            if (count == 0){
                error = std::string(what) + ": empty run at byte " + std::to_string(runStart);
                return false;
            }
            if (count > values.size() - filled){
                error = std::string(what) + ": a run of " + std::to_string(count) + " cells at byte " + std::to_string(runStart) +
                        " overflows the chunk (" + std::to_string(values.size() - filled) + " left)";
                return false;
            }
            if (data.size() - pos < sizeof(T)){
                error = std::string(what) + ": the value of the run at byte " + std::to_string(runStart) + " is cut off";
                return false;
            }
            uint32_t value = 0;
            for (size_t b = 0; b < sizeof(T); b++)
                value |= (uint32_t)(uint8_t)data[pos + b] << (8 * b);
            pos += sizeof(T);
            std::fill_n(values.begin() + (std::ptrdiff_t)filled, count, (T)value);
            filled += count;
        }
        return true;
    }

    constexpr std::array<int8_t, 256> makeCellsBase64Table(){
        std::array<int8_t, 256> table{};
        for (int8_t& entry : table)
            entry = -1;
        constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; i++)
            table[(uint8_t)alphabet[i]] = (int8_t)i;
        return table;
    }
    constexpr std::array<int8_t, 256> CELLS_BASE64 = makeCellsBase64Table();

    // Base64::decode stops at the first byte outside the alphabet and returns what it has, which
    // would turn a damaged string into a shorter valid one
    bool cellsDecodeBase64(std::string_view text, std::string& out, std::string& error){
        if (text.empty() || text.size() % 4 != 0){
            error = "base64: the length " + std::to_string(text.size()) + " is not a positive multiple of 4";
            return false;
        }
        size_t padding = 0;
        if (text[text.size() - 1] == '=')
            padding++;
        if (text[text.size() - 2] == '=')
            padding++;
        const size_t digits = text.size() - padding;
        out.clear();
        out.reserve(text.size() / 4 * 3);
        uint32_t accumulator = 0;
        int bits = 0;
        for (size_t i = 0; i < digits; i++){
            const int8_t digit = CELLS_BASE64[(uint8_t)text[i]];
            if (digit < 0){
                error = "base64: an invalid character at offset " + std::to_string(i);
                return false;
            }
            accumulator = (accumulator << 6) | (uint32_t)digit;
            bits += 6;
            if (bits >= 8){
                bits -= 8;
                out.push_back((char)(uint8_t)(accumulator >> bits));
                accumulator &= (1u << bits) - 1u;
            }
        }
        return true;
    }

}

TileCells::TileCells(int size){
    if (!isValidChunkSize(size))
        size = DEFAULT_CHUNK_SIZE;
    chunkSize = size;
    chunkShift = cellsShiftOf(size);
}

bool TileCells::isValidChunkSize(int size){
    return size == 8 || size == 16 || size == 32 || size == 64;
}

void TileCells::setChunkSize(int size){
    if (!isValidChunkSize(size) || size == chunkSize)
        return;
    const std::vector<ChunkCoord> reported = sortedChunks();
    const size_t oldChunks = reported.size();
    const int newShift = cellsShiftOf(size);
    const size_t newCount = (size_t)size * (size_t)size;
    std::unordered_map<ChunkCoord, TileChunk, ChunkCoordHash> rebucketed;
    revision = cellsNextRevision();
    for (size_t c = 0; c < oldChunks; c++){
        const ChunkCoord coord = reported[c];
        const TileChunk& old = chunkMap.at(coord);
        const CellCoord origin = chunkOrigin(coord);
        for (int i = 0; i < chunkSize * chunkSize; i++){
            const uint32_t value = old.cells[(size_t)i];
            const uint8_t terrain = old.terrain.empty() ? (uint8_t)0 : old.terrain[(size_t)i];
            if (value == 0 && terrain == 0)
                continue;
            const CellCoord cell{origin.x + i % chunkSize, origin.y + i / chunkSize};
            const ChunkCoord target{cell.x >> newShift, cell.y >> newShift};
            TileChunk& chunk = rebucketed[target];
            if (chunk.cells.empty()){
                chunk.cells.assign(newCount, 0);
                chunk.revision = revision;
            }
            const size_t index = cellsLocalIndex(cell, size);
            chunk.cells[index] = value;
            if (value != 0)
                chunk.nonEmpty++;
            if (terrain != 0){
                if (chunk.terrain.empty())
                    chunk.terrain.assign(newCount, 0);
                chunk.terrain[index] = terrain;
                chunk.terrainCount++;
            }
        }
    }
    chunkMap = std::move(rebucketed);
    chunkSize = size;
    chunkShift = newShift;
}

ChunkCoord TileCells::chunkOf(CellCoord cell, int size){
    const int shift = cellsShiftOf(size);
    return ChunkCoord{cell.x >> shift, cell.y >> shift};
}

CellCoord TileCells::chunkOrigin(ChunkCoord chunk) const{
    return CellCoord{(int32_t)((uint32_t)chunk.x << chunkShift), (int32_t)((uint32_t)chunk.y << chunkShift)};
}

CellRect TileCells::chunkRect(ChunkCoord chunk) const{
    const CellCoord origin = chunkOrigin(chunk);
    CellRect rect;
    rect.minX = origin.x;
    rect.minY = origin.y;
    rect.maxX = origin.x + (chunkSize - 1);
    rect.maxY = origin.y + (chunkSize - 1);
    return rect;
}

TileChunk* TileCells::editChunk(ChunkCoord coord, bool create){
    auto it = chunkMap.find(coord);
    if (it != chunkMap.end())
        return &it->second;
    if (!create)
        return nullptr;
    TileChunk& chunk = chunkMap[coord];
    chunk.cells.assign((size_t)chunkSize * (size_t)chunkSize, 0);
    return &chunk;
}

void TileCells::touched(TileChunk* chunk){
    revision = cellsNextRevision();
    chunk->revision = revision;
}

void TileCells::dropIfEmpty(ChunkCoord coord){
    auto it = chunkMap.find(coord);
    if (it != chunkMap.end() && it->second.nonEmpty == 0 && it->second.terrainCount == 0)
        chunkMap.erase(it);
}

uint32_t TileCells::get(CellCoord cell) const{
    auto it = chunkMap.find(ChunkCoord{cell.x >> chunkShift, cell.y >> chunkShift});
    return it == chunkMap.end() ? 0u : it->second.cells[cellsLocalIndex(cell, chunkSize)];
}

uint32_t TileCells::set(CellCoord cell, uint32_t value){
    if (cellValueEmpty(value))
        value = 0;
    const ChunkCoord coord{cell.x >> chunkShift, cell.y >> chunkShift};
    TileChunk* chunk = editChunk(coord, value != 0);
    if (!chunk)
        return 0;
    const size_t index = cellsLocalIndex(cell, chunkSize);
    const uint32_t old = chunk->cells[index];
    if (old == value)
        return old;
    chunk->cells[index] = value;
    if (old == 0)
        chunk->nonEmpty++;
    else if (value == 0)
        chunk->nonEmpty--;
    touched(chunk);
    dropIfEmpty(coord);
    return old;
}

int32_t TileCells::getTile(CellCoord cell, uint32_t* flags) const{
    const uint32_t value = get(cell);
    if (flags)
        *flags = cellValueFlags(value);
    return cellValueTile(value);
}

void TileCells::setTile(CellCoord cell, int32_t tile, uint32_t flags){
    set(cell, makeCellValue(tile, flags));
}

uint8_t TileCells::getTerrainValue(CellCoord cell) const{
    auto it = chunkMap.find(ChunkCoord{cell.x >> chunkShift, cell.y >> chunkShift});
    if (it == chunkMap.end() || it->second.terrain.empty())
        return TERRAIN_NONE_VALUE;
    return it->second.terrain[cellsLocalIndex(cell, chunkSize)];
}

uint8_t TileCells::setTerrainValue(CellCoord cell, uint8_t value){
    const ChunkCoord coord{cell.x >> chunkShift, cell.y >> chunkShift};
    TileChunk* chunk = editChunk(coord, value != TERRAIN_NONE_VALUE);
    if (!chunk)
        return TERRAIN_NONE_VALUE;
    const size_t index = cellsLocalIndex(cell, chunkSize);
    if (chunk->terrain.empty()){
        if (value == TERRAIN_NONE_VALUE)
            return TERRAIN_NONE_VALUE;
        chunk->terrain.assign((size_t)chunkSize * (size_t)chunkSize, 0);
    }
    const uint8_t old = chunk->terrain[index];
    if (old == value)
        return old;
    chunk->terrain[index] = value;
    if (old == TERRAIN_NONE_VALUE)
        chunk->terrainCount++;
    else if (value == TERRAIN_NONE_VALUE)
        chunk->terrainCount--;
    // an all-none plane would make equal maps encode differently (R6: no plane without terrain)
    if (chunk->terrainCount == 0)
        std::vector<uint8_t>().swap(chunk->terrain);
    touched(chunk);
    dropIfEmpty(coord);
    return old;
}

int TileCells::getTerrain(CellCoord cell) const{
    const uint8_t value = getTerrainValue(cell);
    return value == TERRAIN_NONE_VALUE ? NO_TERRAIN : (int)value - 1;
}

void TileCells::setTerrain(CellCoord cell, int terrain){
    if (terrain >= MAX_TILESET_TERRAINS)
        return;
    setTerrainValue(cell, terrain < 0 ? TERRAIN_NONE_VALUE : (uint8_t)(terrain + 1));
}

bool TileCells::hasTerrain() const{
    for (const auto& [coord, chunk] : chunkMap){
        if (!chunk.terrain.empty())
            return true;
    }
    return false;
}

void TileCells::clear(){
    if (chunkMap.empty())
        return;
    revision = cellsNextRevision();
    chunkMap.clear();
}

CellRect TileCells::usedRect() const{
    CellRect rect;
    for (const auto& [coord, chunk] : chunkMap){
        if (chunk.nonEmpty == 0)
            continue;
        const CellCoord origin = chunkOrigin(coord);
        for (int i = 0; i < chunkSize * chunkSize; i++){
            if (chunk.cells[(size_t)i] != 0)
                rect.merge(CellCoord{origin.x + i % chunkSize, origin.y + i / chunkSize});
        }
    }
    return rect;
}

CellRect TileCells::usedRectWithTerrain() const{
    CellRect rect;
    for (const auto& [coord, chunk] : chunkMap){
        const CellCoord origin = chunkOrigin(coord);
        for (int i = 0; i < chunkSize * chunkSize; i++){
            if (chunk.cells[(size_t)i] != 0 || (!chunk.terrain.empty() && chunk.terrain[(size_t)i] != 0))
                rect.merge(CellCoord{origin.x + i % chunkSize, origin.y + i / chunkSize});
        }
    }
    return rect;
}

size_t TileCells::cellCount() const{
    size_t count = 0;
    for (const auto& [coord, chunk] : chunkMap)
        count += chunk.nonEmpty;
    return count;
}

size_t TileCells::memoryBytes() const{
    size_t bytes = sizeof(TileCells);
    for (const auto& [coord, chunk] : chunkMap)
        bytes += sizeof(TileChunk) + sizeof(ChunkCoord) + sizeof(void*) * 2 + chunk.cells.capacity() * sizeof(uint32_t) + chunk.terrain.capacity();
    return bytes;
}

const TileChunk* TileCells::chunk(ChunkCoord coord) const{
    auto it = chunkMap.find(coord);
    return it == chunkMap.end() ? nullptr : &it->second;
}

std::vector<ChunkCoord> TileCells::sortedChunks() const{
    std::vector<ChunkCoord> coords;
    coords.reserve(chunkMap.size());
    for (const auto& [coord, chunk] : chunkMap)
        coords.push_back(coord);
    std::sort(coords.begin(), coords.end());
    return coords;
}

std::string TileCells::encodeChunkBytes(ChunkCoord coord) const{
    const TileChunk* found = chunk(coord);
    const bool terrain = found && !found->terrain.empty();
    std::string out;
    out.push_back((char)(uint8_t)((uint8_t)chunkShift | (terrain ? CELLS_HEADER_TERRAIN : 0)));
    if (found){
        cellsPutRuns(out, found->cells);
        if (terrain)
            cellsPutRuns(out, found->terrain);
    }else{
        // a chunk with nothing in it is still a valid document: all empty, decoded to no chunk
        cellsPutRuns(out, std::vector<uint32_t>((size_t)chunkSize * (size_t)chunkSize, 0u));
    }
    return out;
}

std::string TileCells::encodeChunk(ChunkCoord coord) const{
    const std::string bytes = encodeChunkBytes(coord);
    return Base64::encode((const unsigned char*)bytes.data(), bytes.size());
}

bool TileCells::decodeChunk(ChunkCoord coord, std::string_view base64, std::string& error){
    std::string bytes;
    if (!cellsDecodeBase64(base64, bytes, error))
        return false;
    return decodeChunkBytes(coord, bytes, error);
}

bool TileCells::decodeChunkBytes(ChunkCoord coord, std::string_view bytes, std::string& error){
    if (bytes.empty()){
        error = "no header byte";
        return false;
    }
    const uint8_t header = (uint8_t)bytes[0];
    if ((header & CELLS_HEADER_RESERVED) != 0){
        error = "reserved header bits are set (a newer format?)";
        return false;
    }
    const int shift = header & CELLS_HEADER_SIZE_MASK;
    const int size = shift < 8 ? 1 << shift : 0;
    if (!isValidChunkSize(size)){
        error = "the header's chunk size log2 " + std::to_string(shift) + " is not 8, 16, 32 or 64 cells";
        return false;
    }
    if (size != chunkSize && !chunkMap.empty()){
        error = "the data's chunk size " + std::to_string(size) + " differs from the layer's " + std::to_string(chunkSize);
        return false;
    }
    const size_t count = (size_t)size * (size_t)size;
    std::vector<uint32_t> cells(count, 0u);
    std::vector<uint8_t> terrain;
    size_t pos = 1;
    if (!cellsGetRuns(bytes, pos, cells, "cells", error))
        return false;
    if (header & CELLS_HEADER_TERRAIN){
        terrain.assign(count, 0);
        if (!cellsGetRuns(bytes, pos, terrain, "terrain", error))
            return false;
    }
    if (pos != bytes.size()){
        error = std::to_string(bytes.size() - pos) + " bytes left after the last run";
        return false;
    }

    uint32_t nonEmpty = 0;
    for (uint32_t& value : cells){
        if (cellValueEmpty(value))
            value = 0;
        else
            nonEmpty++;
    }
    uint32_t terrainCount = 0;
    for (uint8_t value : terrain){
        if (value != TERRAIN_NONE_VALUE)
            terrainCount++;
    }
    if (terrainCount == 0)
        terrain.clear();

    if (size != chunkSize){
        chunkSize = size;
        chunkShift = shift;
    }
    auto it = chunkMap.find(coord);
    if (nonEmpty == 0 && terrainCount == 0){
        if (it != chunkMap.end()){
            touched(&it->second);
            chunkMap.erase(it);
        }
        return true;
    }
    if (it != chunkMap.end() && it->second.cells == cells && it->second.terrain == terrain)
        return true;
    TileChunk* chunk = editChunk(coord, true);
    chunk->cells = std::move(cells);
    chunk->terrain = std::move(terrain);
    chunk->nonEmpty = nonEmpty;
    chunk->terrainCount = terrainCount;
    touched(chunk);
    return true;
}

bool TileCells::operator==(const TileCells& other) const{
    if (chunkSize != other.chunkSize || chunkMap.size() != other.chunkMap.size())
        return false;
    for (const auto& [coord, chunk] : chunkMap){
        auto it = other.chunkMap.find(coord);
        if (it == other.chunkMap.end() || chunk.cells != it->second.cells || chunk.terrain != it->second.terrain)
            return false;
    }
    return true;
}

const TileCells& TileCellsRef::read() const{
    static const TileCells empty;
    return store ? *store : empty;
}

TileCells& TileCellsRef::edit(){
    if (!store)
        store = std::make_shared<TileCells>();
    else if (store.use_count() > 1)
        store = std::make_shared<TileCells>(*store);
    return *store;
}

bool TileCellsRef::operator==(const TileCellsRef& other) const{
    const bool setA = store != nullptr, setB = other.store != nullptr;
    if (setA != setB)
        return (setA ? store : other.store)->empty();
    return read() == other.read();
}
