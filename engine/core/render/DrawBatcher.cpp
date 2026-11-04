// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "DrawBatcher.h"

#include <algorithm>

using namespace jasperengine;

const char* jasperengine::getBatchBreakName(BatchBreak reason){
    switch (reason){
        case BatchBreak::NONE: return "none";
        case BatchBreak::TEXTURE: return "texture";
        case BatchBreak::PIPELINE: return "pipeline";
        case BatchBreak::MATERIAL_PARAMS: return "material_params";
        case BatchBreak::ALPHA_CUTOFF: return "alpha_cutoff";
        case BatchBreak::UV_SETS: return "uv_sets";
        case BatchBreak::OBJECT_LIGHTS: return "object_lights";
        case BatchBreak::COLOR_FACTOR: return "color_factor";
        case BatchBreak::UI_COLOR: return "ui_color";
        case BatchBreak::TEXT_PARAMS: return "text_params";
        case BatchBreak::DEPTH_MATERIAL: return "depth_material";
        case BatchBreak::MIRRORED: return "mirrored";
        case BatchBreak::ORIGIN_CELL: return "origin_cell";
        case BatchBreak::SCISSOR: return "scissor";
        case BatchBreak::RUN_VERTEX_LIMIT: return "run_vertex_limit";
        case BatchBreak::OVERLAP: return "overlap";
        case BatchBreak::SORT_BUCKET: return "sort_bucket";
        case BatchBreak::FOREIGN_DRAW: return "foreign_draw";
        case BatchBreak::INELIGIBLE_CUSTOM_SHADER: return "ineligible_custom_shader";
        case BatchBreak::INELIGIBLE_EXTRA_PASS: return "ineligible_extra_pass";
        case BatchBreak::INELIGIBLE_IBL: return "ineligible_ibl";
        case BatchBreak::INELIGIBLE_SKINNED: return "ineligible_skinned";
        case BatchBreak::INELIGIBLE_INSTANCED: return "ineligible_instanced";
        case BatchBreak::INELIGIBLE_LATTICE: return "ineligible_lattice";
        case BatchBreak::INELIGIBLE_VAT: return "ineligible_vat";
        case BatchBreak::INELIGIBLE_LOD: return "ineligible_lod";
        case BatchBreak::INELIGIBLE_VERTEX_COUNT: return "ineligible_vertex_count";
        case BatchBreak::INELIGIBLE_SUBMESHES: return "ineligible_submeshes";
        case BatchBreak::INELIGIBLE_GEOMETRY: return "ineligible_geometry";
        case BatchBreak::INELIGIBLE_RENDER_TARGET: return "ineligible_render_target";
        case BatchBreak::INELIGIBLE_BILLBOARD: return "ineligible_billboard";
        case BatchBreak::INELIGIBLE_TRANSFORM: return "ineligible_transform";
        case BatchBreak::INELIGIBLE_SHADER_PROPERTY: return "ineligible_shader_property";
        case BatchBreak::INELIGIBLE_MIRROR: return "ineligible_mirror";
        case BatchBreak::INELIGIBLE_FLUID: return "ineligible_fluid";
        case BatchBreak::INELIGIBLE_NOT_READY: return "ineligible_not_ready";
        case BatchBreak::DISABLED: return "disabled";
        case BatchBreak::COUNT: break;
    }
    return "unknown";
}

bool BatchKey::addSection(BatchBreak reason, const void* data, size_t length){
    if (length > 255 || (size_t)size + 2 + length > MAX_BYTES)
        return false;
    bytes[size] = (uint8_t)reason;
    bytes[size + 1] = (uint8_t)length;
    if (length > 0)
        std::memcpy(bytes + size + 2, data, length);
    size = (uint16_t)(size + 2 + length);
    return true;
}

BatchBreak BatchKey::compare(const BatchKey& other) const{
    if (pass != other.pass || pipType != other.pipType || pipeline != other.pipeline || noCull != other.noCull)
        return BatchBreak::PIPELINE;
    if (mirrored != other.mirrored)
        return BatchBreak::MIRRORED;
    if (cell[0] != other.cell[0] || cell[1] != other.cell[1] || cell[2] != other.cell[2])
        return BatchBreak::ORIGIN_CELL;
    if (scissor != other.scissor)
        return BatchBreak::SCISSOR;
    if (size == other.size && std::memcmp(bytes, other.bytes, size) == 0)
        return BatchBreak::NONE;
    // the first section that differs names the reason (equal variants have the same sections)
    size_t a = 0;
    size_t b = 0;
    while (a + 2 <= size && b + 2 <= other.size){
        const size_t lengthA = bytes[a + 1];
        const size_t lengthB = other.bytes[b + 1];
        if (bytes[a] != other.bytes[b] || lengthA != lengthB)
            return (BatchBreak)bytes[a];
        if (std::memcmp(bytes + a + 2, other.bytes + b + 2, lengthA) != 0)
            return (BatchBreak)bytes[a];
        a += 2 + lengthA;
        b += 2 + lengthB;
    }
    return BatchBreak::MATERIAL_PARAMS;
}

void BatchRun::reset(){
    head = BatchHead();
    sortKey = 0;
    stride = 0;
    vertexCount = 0;
    items = 0;
    lastEntity = NULL_ENTITY;
    openedBy = BatchBreak::NONE;
    allQuads = true;
    vertices.clear();
    indices.clear();
    rects.clear();
}

unsigned char* BatchRun::appendVertices(uint32_t count){
    const size_t start = vertices.size();
    vertices.resize(start + (size_t)count * stride);
    vertexCount += count;
    return vertices.data() + start;
}

void BatchRun::appendIndices(const uint16_t* source, uint32_t count, uint32_t firstVertex){
    static const uint16_t quad[6] = {0, 1, 2, 0, 2, 3};
    // (appendVertices ran first: the object's vertices are firstVertex..vertexCount)
    if (allQuads && !(count == 6 && std::memcmp(source, quad, sizeof(quad)) == 0 && firstVertex % 4 == 0 && vertexCount - firstVertex == 4))
        allQuads = false;
    const size_t start = indices.size();
    indices.resize(start + count);
    for (uint32_t i = 0; i < count; i++){
        indices[start + i] = (uint16_t)(source[i] + firstVertex);
    }
}

DrawBatcher::DrawBatcher(){
}

void DrawBatcher::setCallbacks(DrawRun drawRun, RequestFlush requestFlush, void* callbackOwner){
    drawRunCallback = drawRun;
    requestFlushCallback = requestFlush;
    owner = callbackOwner;
}

void DrawBatcher::beginPass(BatchMode passMode, size_t passMaxOpenRuns){
    flush();
    mode = passMode;
    maxOpenRuns = (passMode == BatchMode::STRICT) ? 1 : std::max<size_t>(passMaxOpenRuns, 1);
    foreignSinceLastRun = false;
    lastEntity = NULL_ENTITY;
}

void DrawBatcher::logBreak(BatchBreak reason, BatchPass pass, Entity entity, Entity previous){
    stats.breaks[(size_t)reason]++;
    if (breakLog.size() < MAX_BREAK_LOG)
        breakLog.push_back({reason, pass, entity, previous});
}

bool DrawBatcher::overlapsAfter(size_t runIndex, const BatchItem& item, size_t& tested) const{
    const BatchRun& run = *runs[runIndex];
    for (const std::array<float, 4>& rect : run.rects){
        if (++tested > MAX_OVERLAP_RECTS)
            return true;
        if (item.minX <= rect[2] && item.maxX >= rect[0] && item.minY <= rect[3] && item.maxY >= rect[1])
            return true;
    }
    return false;
}

BatchRun& DrawBatcher::place(const BatchKey& key, const BatchHead& head, const BatchItem& item, unsigned int stride){
    auto join = [&](BatchRun& run) -> BatchRun& {
        run.items++;
        run.lastEntity = item.entity;
        if (mode == BatchMode::REORDER)
            run.rects.push_back({item.minX, item.minY, item.maxX, item.maxY});
        lastEntity = item.entity;
        foreignSinceLastRun = false;
        return run;
    };
    auto fits = [&](const BatchRun& run){
        return run.vertexCount + item.vertexCount <= MAX_RUN_VERTICES;
    };

    BatchBreak reason = BatchBreak::NONE;
    if (openRuns > 0){
        BatchRun& newest = *runs[openRuns - 1];
        reason = newest.key.compare(key);
        if (reason == BatchBreak::NONE && !fits(newest))
            reason = BatchBreak::RUN_VERTEX_LIMIT;
        if (reason == BatchBreak::NONE){
            if (newest.sortKey != item.sortKey)
                newest.sortKey = UINT64_MAX; // a run spanning buckets: nothing may jump over it
            return join(newest);
        }

        if (mode == BatchMode::GROUP){
            for (size_t r = 0; r + 1 < openRuns; r++){
                BatchRun& run = *runs[r];
                if (run.key.compare(key) == BatchBreak::NONE && fits(run))
                    return join(run);
            }
        }else if (mode == BatchMode::REORDER){
            // back over the runs after the candidate: each must hold only this object's bucket and
            // nothing its bounds touch
            size_t tested = 0;
            for (size_t r = openRuns - 1; r-- > 0;){
                const BatchRun& after = *runs[r + 1];
                if (after.sortKey != item.sortKey)
                    break;
                if (overlapsAfter(r + 1, item, tested))
                    break;
                BatchRun& run = *runs[r];
                if (run.key.compare(key) == BatchBreak::NONE && fits(run) && run.sortKey == item.sortKey){
                    stats.reordered++;
                    return join(run);
                }
            }
        }
    }

    BatchBreak openedBy = BatchBreak::NONE;
    if (openRuns > 0)
        openedBy = reason;
    else if (foreignSinceLastRun && lastEntity != NULL_ENTITY)
        openedBy = BatchBreak::FOREIGN_DRAW;
    if (openedBy != BatchBreak::NONE)
        logBreak(openedBy, key.pass, item.entity, lastEntity);

    if (openRuns >= maxOpenRuns){
        internalFlush = true;
        if (requestFlushCallback)
            requestFlushCallback();
        // (no flush installed: drawn here)
        if (openRuns > 0)
            flush();
        internalFlush = false;
    }
    return openRun(key, head, item, stride, openedBy);
}

BatchRun& DrawBatcher::openRun(const BatchKey& key, const BatchHead& head, const BatchItem& item, unsigned int stride, BatchBreak openedBy){
    if (openRuns == runs.size())
        runs.push_back(std::make_unique<BatchRun>());
    BatchRun& run = *runs[openRuns++];
    run.reset();
    run.key = key;
    run.head = head;
    run.stride = stride;
    run.sortKey = item.sortKey;
    run.items = 1;
    run.lastEntity = item.entity;
    run.openedBy = openedBy;
    if (mode == BatchMode::REORDER)
        run.rects.push_back({item.minX, item.minY, item.maxX, item.maxY});
    lastEntity = item.entity;
    foreignSinceLastRun = false;
    return run;
}

void DrawBatcher::flush(){
    const size_t count = openRuns;
    // something else draws next, unless this batcher made room for its own next run
    if (count > 0 && !internalFlush)
        foreignSinceLastRun = true;
    // (a draw callback that flushes again finds nothing open)
    openRuns = 0;
    for (size_t r = 0; r < count; r++){
        BatchRun& run = *runs[r];
        if (run.items == 0)
            continue;
        if (run.items > 1){
            stats.batches++;
            stats.batchedObjects += run.items;
        }else{
            stats.singleDraws++;
        }
        if (drawRunCallback)
            drawRunCallback(owner, run);
        run.reset();
    }
}

void DrawBatcher::noteAlone(BatchPass pass, Entity entity, BatchBreak reason){
    stats.aloneDraws++;
    if (reason != BatchBreak::NONE)
        logBreak(reason, pass, entity, NULL_ENTITY);
}

void DrawBatcher::clearFrame(){
    const bool enabled = stats.enabled;
    stats = BatchStats();
    stats.enabled = enabled;
    breakLog.clear();
}
