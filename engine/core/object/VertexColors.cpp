// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// docs/specs/V1-vertex-colors.md R80-R83, R125, D12: Vertex Colors at runtime

#include "object/VertexColors.h"

#include "Engine.h"
#include "component/MeshComponent.h"
#include "component/VertexColorsComponent.h"
#include "picking/MeshRaycast.h"
#include "subsystem/VertexColorSceneDetail.h"
#include "vertexcolor/VertexColorOps.h"
#include "vertexcolor/VertexColorOpsJson.h"

#include <algorithm>
#include <cmath>
#include <limits>

using namespace jasperengine;

namespace {

    nlohmann::json vertexColorsKeyJson(const vertexcolor::TargetKey& key){
        if (key.submesh >= 0)
            return nlohmann::json{{"submesh", key.submesh}};
        return nlohmann::json{{"node", key.node}, {"primitive", key.primitive}};
    }

    // the uniform scale of a matrix, for a world radius in local units (R32)
    float vertexColorsScale(const Matrix4& model){
        const float sx = Vector3(model[0][0], model[0][1], model[0][2]).length();
        const float sy = Vector3(model[1][0], model[1][1], model[1][2]).length();
        const float sz = Vector3(model[2][0], model[2][1], model[2][2]).length();
        const float scale = (sx + sy + sz) / 3.0f;
        return scale > 1e-12f ? scale : 1.0f;
    }

    // the closest point of triangle abc to p, as barycentric weights
    Vector3 vertexColorsClosest(const Vector3& p, const Vector3& a, const Vector3& b, const Vector3& c){
        const Vector3 ab = b - a, ac = c - a, ap = p - a;
        const float d1 = ab.dotProduct(ap), d2 = ac.dotProduct(ap);
        if (d1 <= 0.0f && d2 <= 0.0f) return Vector3(1.0f, 0.0f, 0.0f);
        const Vector3 bp = p - b;
        const float d3 = ab.dotProduct(bp), d4 = ac.dotProduct(bp);
        if (d3 >= 0.0f && d4 <= d3) return Vector3(0.0f, 1.0f, 0.0f);
        const float vc = d1 * d4 - d3 * d2;
        if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f){
            const float v = d1 / (d1 - d3);
            return Vector3(1.0f - v, v, 0.0f);
        }
        const Vector3 cp = p - c;
        const float d5 = ab.dotProduct(cp), d6 = ac.dotProduct(cp);
        if (d6 >= 0.0f && d5 <= d6) return Vector3(0.0f, 0.0f, 1.0f);
        const float vb = d5 * d2 - d1 * d6;
        if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f){
            const float w = d2 / (d2 - d6);
            return Vector3(1.0f - w, 0.0f, w);
        }
        const float va = d3 * d6 - d5 * d4;
        if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f){
            const float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
            return Vector3(0.0f, 1.0f - w, w);
        }
        const float denom = 1.0f / (va + vb + vc);
        const float v = vb * denom, w = vc * denom;
        return Vector3(1.0f - v - w, v, w);
    }

}

VertexColors::VertexColors(Scene* scene, Entity entity): EntityHandle(scene, entity){
}

VertexColors::~VertexColors(){
}

VertexColors::VertexColors(const VertexColors& rhs) = default;
VertexColors& VertexColors::operator=(const VertexColors& rhs) = default;
VertexColors::VertexColors(VertexColors&& rhs) noexcept = default;
VertexColors& VertexColors::operator=(VertexColors&& rhs) noexcept = default;

VertexColorsComponent* VertexColors::component() const{
    return scene ? scene->findComponent<VertexColorsComponent>(entity) : nullptr;
}

bool VertexColors::isValid() const{
    return component() != nullptr;
}

bool VertexColors::isEnabled() const{
    const VertexColorsComponent* colors = component();
    return colors && colors->enabled;
}

void VertexColors::setEnabled(bool enabled){
    if (VertexColorsComponent* colors = component()){
        colors->enabled = enabled;
        colors->needUpdate = true;
    }
}

std::string VertexColors::getFormat() const{
    const VertexColorsComponent* colors = component();
    return (colors && colors->format == vertexcolor::Format::RGBA16F) ? "rgba16f" : "rgba8";
}

std::vector<std::string> VertexColors::getChannelNames() const{
    const VertexColorsComponent* colors = component();
    if (!colors)
        return {"", "", "", ""};
    return std::vector<std::string>(colors->channelNames.begin(), colors->channelNames.end());
}

std::vector<vertexcolor::SceneTarget> VertexColors::getTargets() const{
    return vertexcolor::sceneTargets(scene, entity);
}

std::string VertexColors::describeTargets() const{
    std::string list;
    for (const vertexcolor::SceneTarget& target : getTargets())
        list += (list.empty() ? "" : ", ") + target.key.describe();
    return list.empty() ? "none" : list;
}

bool VertexColors::target(const vertexcolor::TargetKey& key, vertexcolor::SceneTarget& out, std::string& error) const{
    if (!component()){
        error = "the GameObject has no Vertex Colors component (add one in the editor)";
        return false;
    }
    if (vertexcolor::detail::resolveTarget(scene, entity, key, out))
        return true;
    error = "no target " + key.describe() + " (the targets: " + describeTargets() + ")";
    return false;
}

bool VertexColors::applyOps(const nlohmann::json& list, const vertexcolor::TargetKey* only, nlohmann::json& result, std::string& error){
    VertexColorsComponent* colors = component();
    if (!colors){
        error = "the GameObject has no Vertex Colors component (add one in the editor)";
        return false;
    }
    vertexcolor::OpJsonHooks hooks;
    hooks.firstIndex = 1;
    hooks.fieldsDoc = "jasper://docs/meshes";
    vertexcolor::OpJsonParser parser(hooks);
    std::vector<vertexcolor::Op> ops;
    if (!parser.parseList(list, ops)){
        error = parser.error;
        return false;
    }
    std::vector<vertexcolor::SceneTarget> targets;
    if (only){
        vertexcolor::SceneTarget one;
        if (!target(*only, one, error))
            return false;
        targets.push_back(one);
    }else{
        targets = getTargets();
    }
    if (targets.empty()){
        error = "the mesh has no targets yet (still loading, or nothing to paint)";
        return false;
    }

    // all targets or none (R26): each runs on a copy, kept once every one is ok
    vertexcolor::detail::loadComponentData(scene, entity, *colors);
    std::vector<std::pair<vertexcolor::TargetData, vertexcolor::OpsResult>> done;
    for (const vertexcolor::SceneTarget& t : targets){
        std::shared_ptr<const vertexcolor::MeshView> view = sceneMeshView(scene, entity, t.key);
        if (!view){
            error = t.key.describe() + ": its geometry cannot be read";
            return false;
        }
        const vertexcolor::TargetData* existing = colors->loaded ? colors->loaded->find(t.key) : nullptr;
        if (existing && !existing->fingerprint.sameTopology(view->fingerprint())){
            error = t.key.describe() + " was painted on another version of the mesh: open the scene in the editor and save it to remap it";
            return false;
        }
        vertexcolor::TargetData copy = existing ? *existing : vertexcolor::TargetData();
        copy.key = t.key;
        copy.fingerprint = view->fingerprint();
        vertexcolor::ApplyOptions options;
        options.model = sceneTargetModel(scene, entity, t.key);
        options.world = true;
        options.initial = colors->initialValue;
        options.customInitial = colors->customInitial;
        options.format = colors->format;
        options.firstIndex = 1;
        vertexcolor::OpsResult r = vertexcolor::applyOps(copy, *view, ops, options);
        if (!r.ok){
            error = (targets.size() > 1 ? t.key.describe() + ": " : std::string()) + r.error;
            return false;
        }
        done.emplace_back(std::move(copy), std::move(r));
    }

    vertexcolor::Data& data = vertexcolor::detail::writableData(scene, entity, *colors);
    result = nlohmann::json::array();
    for (auto& [copy, r] : done){
        nlohmann::json entry = vertexcolor::toJson(r);
        entry["target"] = vertexColorsKeyJson(copy.key);
        result.push_back(std::move(entry));
        vertexcolor::TargetData* stored = data.find(copy.key);
        if (!stored)
            stored = &data.add(copy.key);
        *stored = std::move(copy);
    }
    vertexcolor::markVertexColorsChanged(scene, entity);
    return true;
}

bool VertexColors::getValues(const vertexcolor::TargetKey& key, uint32_t first, uint32_t count, std::vector<Vector4>& out, std::string& error){
    vertexcolor::SceneTarget t;
    if (!target(key, t, error))
        return false;
    if (count > MAX_VALUES){
        error = "count " + std::to_string(count) + " is over the cap of " + std::to_string(MAX_VALUES) + " values: read in pages";
        return false;
    }
    if ((uint64_t)first + count > t.vertexCount){
        error = "vertices " + std::to_string(first) + " .. " + std::to_string((uint64_t)first + count) + " are past the target's " +
                std::to_string(t.vertexCount) + " vertices (0-based)";
        return false;
    }
    VertexColorsComponent& colors = *component();
    vertexcolor::detail::loadComponentData(scene, entity, colors);
    const vertexcolor::TargetData* data = colors.loaded ? colors.loaded->find(key) : nullptr;
    out.resize(count);
    if (data && data->base.size() == t.vertexCount){
        std::copy(data->base.begin() + first, data->base.begin() + first + count, out.begin());
        return true;
    }
    std::shared_ptr<const vertexcolor::MeshView> view = sceneMeshView(scene, entity, key);
    if (!view){
        error = key.describe() + ": its geometry cannot be read";
        return false;
    }
    vertexcolor::TargetData initial;
    initializeTarget(initial, *view, colors.initialValue, colors.customInitial);
    std::copy(initial.base.begin() + first, initial.base.begin() + first + count, out.begin());
    return true;
}

bool VertexColors::setValues(const vertexcolor::TargetKey& key, uint32_t first, std::span<const Vector4> values, std::string& error){
    vertexcolor::SceneTarget t;
    if (!target(key, t, error))
        return false;
    if ((uint64_t)first + values.size() > t.vertexCount){
        error = "vertices " + std::to_string(first) + " .. " + std::to_string((uint64_t)first + values.size()) + " are past the target's " +
                std::to_string(t.vertexCount) + " vertices (0-based)";
        return false;
    }
    std::shared_ptr<const vertexcolor::MeshView> view = sceneMeshView(scene, entity, key);
    if (!view){
        error = key.describe() + ": its geometry cannot be read";
        return false;
    }
    VertexColorsComponent& colors = *component();
    vertexcolor::Data& data = vertexcolor::detail::writableData(scene, entity, colors);
    vertexcolor::TargetData* stored = data.find(key);
    if (stored && !stored->fingerprint.sameTopology(view->fingerprint())){
        error = key.describe() + " was painted on another version of the mesh: open the scene in the editor and save it to remap it";
        return false;
    }
    if (!stored){
        stored = &data.add(key);
        stored->fingerprint = view->fingerprint();
    }
    if (stored->base.size() != view->vertexCount())
        initializeTarget(*stored, *view, colors.initialValue, colors.customInitial);
    const uint32_t end = first + (uint32_t)values.size();
    for (uint32_t v = first; v < end; v++)
        stored->base[v] = quantize(values[v - first], colors.format);
    // R4: a vertex set whole has no corner of another value
    stored->corners.erase(std::remove_if(stored->corners.begin(), stored->corners.end(), [&](const vertexcolor::CornerValue& corner){
        const uint32_t v = corner.corner < view->indices.size() ? view->indices[corner.corner] : UINT32_MAX;
        return v >= first && v < end;
    }), stored->corners.end());
    vertexcolor::markVertexColorsChanged(scene, entity);
    return true;
}

bool VertexColors::getStats(const vertexcolor::TargetKey& key, nlohmann::json& out, std::string& error){
    vertexcolor::SceneTarget t;
    if (!target(key, t, error))
        return false;
    std::shared_ptr<const vertexcolor::MeshView> view = sceneMeshView(scene, entity, key);
    if (!view){
        error = key.describe() + ": its geometry cannot be read";
        return false;
    }
    VertexColorsComponent& colors = *component();
    vertexcolor::detail::loadComponentData(scene, entity, colors);
    const vertexcolor::TargetData* data = colors.loaded ? colors.loaded->find(key) : nullptr;
    vertexcolor::TargetData initial;
    if (!data || data->base.size() != view->vertexCount()){
        initializeTarget(initial, *view, colors.initialValue, colors.customInitial);
        data = &initial;
    }
    out = vertexcolor::toJson(vertexcolor::computeStats(*data, *view));
    return true;
}

namespace {

    // the value drawn at a triangle point: corner values (authored, or initial), the runtime
    // layer over each corner's vertex, blended by the barycentric weights
    Vector4 vertexColorsValueAt(const vertexcolor::TargetData* data, const std::vector<Vector4>& initial, const vertexcolor::RuntimeLayer* layer,
                                const vertexcolor::MeshView& view, uint32_t triangle, const Vector3& bary){
        Vector4 out(0.0f, 0.0f, 0.0f, 0.0f);
        const float w[3] = {bary.x, bary.y, bary.z};
        for (int k = 0; k < 3; k++){
            const uint32_t corner = triangle * 3 + k;
            const uint32_t v = view.indices[corner];
            Vector4 value = data ? data->cornerValue(corner, view.indices) : initial[v];
            if (layer)
                value = layer->over(v, value);
            out = out + value * w[k];
        }
        return out;
    }

}

bool VertexColors::sampleAt(const Vector3& worldPoint, Vector4& out, std::string& error){
    VertexColorsComponent* colors = component();
    if (!colors){
        error = "the GameObject has no Vertex Colors component (add one in the editor)";
        return false;
    }
    vertexcolor::detail::loadComponentData(scene, entity, *colors);
    float best = std::numeric_limits<float>::infinity();
    bool found = false;
    for (const vertexcolor::SceneTarget& t : getTargets()){
        std::shared_ptr<const vertexcolor::MeshView> view = sceneMeshView(scene, entity, t.key);
        if (!view || view->triangleCount() == 0)
            continue;
        const Matrix4 model = sceneTargetModel(scene, entity, t.key);
        const Vector3 local = model.inverse() * worldPoint;
        uint32_t bestTriangle = 0;
        Vector3 bestBary;
        float targetBest = std::numeric_limits<float>::infinity();
        for (uint32_t tri = 0; tri < view->triangleCount(); tri++){
            const Vector3& a = view->positions[view->indices[tri * 3]];
            const Vector3& b = view->positions[view->indices[tri * 3 + 1]];
            const Vector3& c = view->positions[view->indices[tri * 3 + 2]];
            const Vector3 bary = vertexColorsClosest(local, a, b, c);
            const Vector3 point = a * bary.x + b * bary.y + c * bary.z;
            const float d = (model * point - worldPoint).squaredLength();
            if (d < targetBest){
                targetBest = d;
                bestTriangle = tri;
                bestBary = bary;
            }
        }
        if (targetBest >= best)
            continue;
        best = targetBest;
        found = true;
        const vertexcolor::TargetData* data = colors->loaded ? colors->loaded->find(t.key) : nullptr;
        if (data && data->base.size() != view->vertexCount())
            data = nullptr;
        vertexcolor::TargetData initial;
        if (!data)
            initializeTarget(initial, *view, colors->initialValue, colors->customInitial);
        auto layer = colors->runtimeLayers.find(t.key);
        out = vertexColorsValueAt(data, initial.base, layer != colors->runtimeLayers.end() ? layer->second.get() : nullptr,
                                  *view, bestTriangle, bestBary);
    }
    if (!found)
        error = "the mesh has no triangles to sample (still loading?)";
    return found;
}

bool VertexColors::hitPoint(const MeshRaycastHit& hit, Vector3& world, vertexcolor::SceneTarget& target, std::string& error){
    if (!component()){
        error = "the GameObject has no Vertex Colors component (add one in the editor)";
        return false;
    }
    if (!hit.hit){
        error = "the hit is empty (the ray hit nothing)";
        return false;
    }
    for (const vertexcolor::SceneTarget& t : getTargets()){
        if (t.meshEntity != hit.entity || t.submesh != hit.submesh)
            continue;
        std::shared_ptr<const vertexcolor::MeshView> view = sceneMeshView(scene, entity, t.key);
        if (!view || hit.triangle >= view->triangleCount()){
            error = t.key.describe() + ": the hit's triangle is not on the mesh as loaded";
            return false;
        }
        // D12: the posed hit back to bind pose through its triangle
        const Vector3& a = view->positions[view->indices[hit.triangle * 3]];
        const Vector3& b = view->positions[view->indices[hit.triangle * 3 + 1]];
        const Vector3& c = view->positions[view->indices[hit.triangle * 3 + 2]];
        world = sceneTargetModel(scene, entity, t.key) * (a * hit.barycentric.x + b * hit.barycentric.y + c * hit.barycentric.z);
        target = t;
        return true;
    }
    error = "the hit is not on this GameObject's meshes (MeshRaycast hit '" + scene->getEntityName(hit.entity) + "')";
    return false;
}

bool VertexColors::sampleAt(const MeshRaycastHit& hit, Vector4& out, std::string& error){
    Vector3 world;
    vertexcolor::SceneTarget t;
    if (!hitPoint(hit, world, t, error))
        return false;
    VertexColorsComponent& colors = *component();
    vertexcolor::detail::loadComponentData(scene, entity, colors);
    std::shared_ptr<const vertexcolor::MeshView> view = vertexcolor::sceneMeshView(scene, entity, t.key);
    const vertexcolor::TargetData* data = colors.loaded ? colors.loaded->find(t.key) : nullptr;
    if (data && data->base.size() != view->vertexCount())
        data = nullptr;
    vertexcolor::TargetData initial;
    if (!data)
        initializeTarget(initial, *view, colors.initialValue, colors.customInitial);
    auto layer = colors.runtimeLayers.find(t.key);
    out = vertexColorsValueAt(data, initial.base, layer != colors.runtimeLayers.end() ? layer->second.get() : nullptr,
                              *view, hit.triangle, hit.barycentric);
    return true;
}

uint32_t VertexColors::paint(const Vector3& worldPoint, const vertexcolor::RuntimePaint& stroke, std::string& error){
    VertexColorsComponent* colors = component();
    if (!colors){
        error = "the GameObject has no Vertex Colors component (add one in the editor)";
        return 0;
    }
    if (!(stroke.radius > 0.0f) || !std::isfinite(stroke.radius)){
        error = "radius must be a number above 0";
        return 0;
    }
    const double now = Engine::getGameTime();
    uint32_t touched = 0;
    for (const vertexcolor::SceneTarget& t : getTargets()){
        std::shared_ptr<const vertexcolor::MeshView> view = sceneMeshView(scene, entity, t.key);
        if (!view)
            continue;
        const Matrix4 model = sceneTargetModel(scene, entity, t.key);
        const Vector3 local = model.inverse() * worldPoint;
        vertexcolor::RuntimePaint scaled = stroke;
        scaled.radius = stroke.radius / vertexColorsScale(model);
        // bounds first: a stamp away from a target costs no vertex loop
        AABB reach = view->bounds;
        if (reach.isNull())
            continue;
        const Vector3 lo = reach.getMinimum() - Vector3(scaled.radius), hi = reach.getMaximum() + Vector3(scaled.radius);
        if (local.x < lo.x || local.y < lo.y || local.z < lo.z || local.x > hi.x || local.y > hi.y || local.z > hi.z)
            continue;
        std::shared_ptr<vertexcolor::RuntimeLayer>& layer = colors->runtimeLayers[t.key];
        if (!layer)
            layer = std::make_shared<vertexcolor::RuntimeLayer>();
        touched += layer->paint(*view, local, scaled, now);
    }
    return touched;
}

uint32_t VertexColors::paint(const MeshRaycastHit& hit, const vertexcolor::RuntimePaint& stroke, std::string& error){
    Vector3 world;
    vertexcolor::SceneTarget t;
    if (!hitPoint(hit, world, t, error))
        return 0;
    return paint(world, stroke, error);
}

void VertexColors::clearRuntime(){
    VertexColorsComponent* colors = component();
    if (!colors || colors->runtimeLayers.empty())
        return;
    colors->runtimeLayers.clear();
    vertexcolor::markVertexColorsChanged(scene, entity);
}
