// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The undo step of vertex color writes (docs/specs/V1-vertex-colors.md R107, D7): XOR deltas of the
// base values in 4,096-vertex chunks, zstd-packed, and the override tables before and after.

#include "VertexColorEditorInternal.h"

#include "Project.h"
#include "component/VertexColorsComponent.h"
#include "terrain/TerrainBytes.h"

#include <algorithm>
#include <cstring>
#include <set>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::vertexcolor;
namespace vcd = jasperengine::editor::vertexcolor::detail;
using vcd::VertexColorEditCmd;

namespace {

    constexpr size_t kVcValueBytes = sizeof(float) * 4;
    constexpr size_t kVcCornerBytes = sizeof(uint32_t) + kVcValueBytes;

    void vcValueBits(const Vector4& value, uint8_t* out) {
        const float components[4] = {value.x, value.y, value.z, value.w};
        std::memcpy(out, components, kVcValueBytes);
    }

    Vector4 vcBitsValue(const uint8_t* bytes) {
        float components[4];
        std::memcpy(components, bytes, kVcValueBytes);
        return Vector4(components[0], components[1], components[2], components[3]);
    }

    std::shared_ptr<const std::vector<uint8_t>> vcPackCorners(const std::vector<vc::CornerValue>& corners) {
        if (corners.empty()) return nullptr;
        std::vector<uint8_t> raw(corners.size() * kVcCornerBytes);
        for (size_t i = 0; i < corners.size(); i++) {
            std::memcpy(raw.data() + i * kVcCornerBytes, &corners[i].corner, sizeof(uint32_t));
            vcValueBits(corners[i].value, raw.data() + i * kVcCornerBytes + sizeof(uint32_t));
        }
        return std::make_shared<const std::vector<uint8_t>>(packTerrainBytes(raw));
    }

    bool vcUnpackCorners(const std::shared_ptr<const std::vector<uint8_t>>& packed, uint32_t count, std::vector<vc::CornerValue>& out) {
        out.clear();
        if (count == 0) return true;
        std::vector<uint8_t> raw((size_t)count * kVcCornerBytes);
        if (!packed || !unpackTerrainBytes(*packed, raw)) return false;
        out.resize(count);
        for (uint32_t i = 0; i < count; i++) {
            std::memcpy(&out[i].corner, raw.data() + (size_t)i * kVcCornerBytes, sizeof(uint32_t));
            out[i].value = vcBitsValue(raw.data() + (size_t)i * kVcCornerBytes + sizeof(uint32_t));
        }
        return true;
    }

    const vc::TargetData* vcFindTarget(const vc::Data* data, const vc::TargetKey& key) {
        return data ? data->find(key) : nullptr;
    }

}

VertexColorEditCmd::VertexColorEditCmd(Project* project, uint32_t sceneId, std::string name, bool alreadyApplied):
    project(project), sceneId(sceneId), name(std::move(name)), applied(alreadyApplied) {}

void VertexColorEditCmd::addTarget(Entity entity, const vc::TargetData* before, const vc::TargetData* after) {
    TargetDelta delta;
    delta.entity = entity;
    delta.key = after ? after->key : (before ? before->key : vc::TargetKey());
    delta.existsBefore = before != nullptr;
    delta.existsAfter = after != nullptr;
    if (before) {
        delta.fingerprintBefore = before->fingerprint;
        delta.countBefore = (uint32_t)before->base.size();
    }
    if (after) {
        delta.fingerprintAfter = after->fingerprint;
        delta.countAfter = (uint32_t)after->base.size();
    }
    const uint32_t count = std::max(delta.countBefore, delta.countAfter);
    std::vector<uint8_t> raw;
    for (uint32_t first = 0; first < count; first += CHUNK_VERTICES) {
        const uint32_t size = std::min(CHUNK_VERTICES, count - first);
        raw.assign((size_t)size * kVcValueBytes, 0);
        bool any = false;
        uint8_t a[kVcValueBytes];
        uint8_t b[kVcValueBytes];
        for (uint32_t i = 0; i < size; i++) {
            const uint32_t v = first + i;
            std::memset(a, 0, sizeof(a));
            std::memset(b, 0, sizeof(b));
            if (before && v < before->base.size()) vcValueBits(before->base[v], a);
            if (after && v < after->base.size()) vcValueBits(after->base[v], b);
            uint8_t* out = raw.data() + (size_t)i * kVcValueBytes;
            for (size_t k = 0; k < kVcValueBytes; k++) {
                out[k] = a[k] ^ b[k];
                any = any || out[k] != 0;
            }
        }
        if (!any) continue;
        Chunk chunk;
        chunk.index = first / CHUNK_VERTICES;
        chunk.bytes = packTerrainBytes(raw);
        delta.chunks.push_back(std::move(chunk));
    }
    const std::vector<vc::CornerValue> none;
    const std::vector<vc::CornerValue>& cornersBefore = before ? before->corners : none;
    const std::vector<vc::CornerValue>& cornersAfter = after ? after->corners : none;
    const bool sameCorners = cornersBefore.size() == cornersAfter.size() &&
                             std::equal(cornersBefore.begin(), cornersBefore.end(), cornersAfter.begin(), [](const vc::CornerValue& x, const vc::CornerValue& y) {
                                 return x.corner == y.corner && x.value == y.value;
                             });
    delta.cornersBefore.count = (uint32_t)cornersBefore.size();
    delta.cornersBefore.bytes = vcPackCorners(cornersBefore);
    if (sameCorners) {
        delta.cornersAfter = delta.cornersBefore;
    } else {
        delta.cornersAfter.count = (uint32_t)cornersAfter.size();
        delta.cornersAfter.bytes = vcPackCorners(cornersAfter);
    }
    const bool unchanged = delta.chunks.empty() && sameCorners && delta.existsBefore == delta.existsAfter &&
                           delta.countBefore == delta.countAfter && delta.fingerprintBefore == delta.fingerprintAfter;
    if (!unchanged) targets.push_back(std::move(delta));
}

void VertexColorEditCmd::setFormatChange(Entity entity, vc::Format before, vc::Format after) {
    if (before != after) formats.push_back({entity, before, after});
}

std::string VertexColorEditCmd::refusalFor(bool toAfter) const {
    for (const TargetDelta& delta : targets) {
        std::string error;
        const std::shared_ptr<vc::Data> data = vcd::resolve(project, sceneId, delta.entity, &error);
        // a file that does not read stays as it is
        if (!data && !error.empty()) return error;
        // no file yet: an earlier step of the same group makes it (the first edit's component and file)
        if (!data) continue;
        // the state the step left (undo) or found (redo)
        const bool exists = toAfter ? delta.existsBefore : delta.existsAfter;
        const vc::Fingerprint& fingerprint = toAfter ? delta.fingerprintBefore : delta.fingerprintAfter;
        const uint32_t count = toAfter ? delta.countBefore : delta.countAfter;
        const vc::TargetData* target = vcFindTarget(data.get(), delta.key);
        if ((target != nullptr) != exists || (target && (target->fingerprint != fingerprint || target->base.size() != count))) {
            return "The mesh of " + delta.key.describe() + " changed since this step (its vertex colors were remapped or repainted on another mesh), "
                   "so it can't be " + std::string(toAfter ? "redone." : "undone.");
        }
    }
    return std::string();
}

std::string VertexColorEditCmd::undoRefusal() const {
    const std::string own = refusalFor(false);
    return own.empty() ? Command::undoRefusal() : own;
}

std::string VertexColorEditCmd::redoRefusal() const {
    const std::string own = refusalFor(true);
    return own.empty() ? Command::redoRefusal() : own;
}

void VertexColorEditCmd::apply(bool toAfter) {
    std::set<Entity> changed;
    for (const FormatChange& change : formats) {
        if (const std::shared_ptr<vc::Data> data = vcd::resolve(project, sceneId, change.entity)) {
            data->format = toAfter ? change.after : change.before;
            changed.insert(change.entity);
        }
    }
    std::vector<uint8_t> raw;
    for (const TargetDelta& delta : targets) {
        const std::shared_ptr<vc::Data> data = vcd::resolve(project, sceneId, delta.entity);
        if (!data) continue;
        vc::TargetData* target = data->find(delta.key);
        if (!target) target = &data->add(delta.key);
        const uint32_t count = std::max(delta.countBefore, delta.countAfter);
        target->base.resize(count, Vector4(0.0f, 0.0f, 0.0f, 0.0f));
        for (const Chunk& chunk : delta.chunks) {
            const uint32_t first = chunk.index * CHUNK_VERTICES;
            const uint32_t size = std::min(CHUNK_VERTICES, count - first);
            raw.resize((size_t)size * kVcValueBytes);
            if (!unpackTerrainBytes(chunk.bytes, raw)) continue;
            uint8_t bits[kVcValueBytes];
            for (uint32_t i = 0; i < size; i++) {
                vcValueBits(target->base[first + i], bits);
                for (size_t k = 0; k < kVcValueBytes; k++) bits[k] ^= raw[(size_t)i * kVcValueBytes + k];
                target->base[first + i] = vcBitsValue(bits);
            }
        }
        target->base.resize(toAfter ? delta.countAfter : delta.countBefore);
        const Corners& corners = toAfter ? delta.cornersAfter : delta.cornersBefore;
        if (!vcUnpackCorners(corners.bytes, corners.count, target->corners)) target->corners.clear();
        target->fingerprint = toAfter ? delta.fingerprintAfter : delta.fingerprintBefore;
        if (!(toAfter ? delta.existsAfter : delta.existsBefore)) {
            const vc::TargetKey key = delta.key;
            data->targets.erase(std::remove_if(data->targets.begin(), data->targets.end(), [&key](const vc::TargetData& t) { return t.key == key; }),
                                data->targets.end());
        }
        changed.insert(delta.entity);
    }
    for (Entity entity : changed) vcd::touched(project, sceneId, entity);
}

bool VertexColorEditCmd::execute() {
    SceneProject* sceneProject = vcd::sceneProjectOf(project, sceneId);
    if (!sceneProject) return false;
    wasModified = sceneProject->isModified;
    if (applied) {
        // the live values hold it already (a stroke showed itself as it went): only redos apply
        applied = false;
    } else {
        apply(true);
    }
    // the colors are the .vcolors asset's, saved with the scene (R8)
    sceneProject->isModified = true;
    return true;
}

void VertexColorEditCmd::undo() {
    SceneProject* sceneProject = vcd::sceneProjectOf(project, sceneId);
    if (!sceneProject) return;
    apply(false);
    sceneProject->isModified = wasModified;
}

size_t VertexColorEditCmd::memoryBytes() const {
    size_t bytes = sizeof(*this) + name.capacity() + targets.capacity() * sizeof(TargetDelta) + formats.capacity() * sizeof(FormatChange);
    for (const TargetDelta& delta : targets) {
        bytes += delta.chunks.capacity() * sizeof(Chunk);
        if (delta.cornersBefore.bytes) bytes += delta.cornersBefore.bytes->capacity();
        if (delta.cornersAfter.bytes && delta.cornersAfter.bytes != delta.cornersBefore.bytes) bytes += delta.cornersAfter.bytes->capacity();
        for (const Chunk& chunk : delta.chunks) bytes += chunk.bytes.capacity();
    }
    return bytes;
}

size_t VertexColorEditCmd::chunkCount() const {
    size_t count = 0;
    for (const TargetDelta& delta : targets) count += delta.chunks.size();
    return count;
}
