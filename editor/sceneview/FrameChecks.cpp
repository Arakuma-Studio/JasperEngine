// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "FrameChecks.h"

#include "Project.h"
#include "Scene.h"
#include "asset/MissingAssetTracker.h"
#include "component/CameraComponent.h"
#include "component/LightComponent.h"
#include "component/MeshComponent.h"
#include "component/Transform.h"
#include "math/Quaternion.h"
#include "math/Vector4.h"
#include "picking/MeshBvh.h"
#include "picking/MeshPose.h"
#include "picking/PickCache.h"
#include "picking/SceneBvh.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <unordered_set>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::framechecks;

namespace {

    using Clock = std::chrono::steady_clock;

    // a pair's faces read from each mesh, at most (a dense pair is decided on these)
    constexpr size_t MAX_PAIR_TRIANGLES = 4096;
    // the share of a normal's largest component another axis may come within and still be
    // searched: normals COPLANAR_COS apart differ by less than this per component
    constexpr float AXIS_MARGIN = 0.02f;
    // a mesh's triangle BVH still queued is built in the check up to this size
    constexpr uint32_t INLINE_BUILD_TRIANGLES = 4096;
    // how far z-fighting faces are pushed apart by the fix (metres)
    constexpr float SEPARATION = 0.005f;

    // the order findings of one severity are listed in: what hides the most first
    const char* const KIND_ORDER[] = {"empty_frame", "exposure_dark", "exposure_clipped", "subject_out_of_frame",
                                      "texture_missing", "subject_too_small", "z_fighting"};

    int kindRank(const std::string& kind){
        for (size_t i = 0; i < sizeof(KIND_ORDER) / sizeof(KIND_ORDER[0]); i++){
            if (kind == KIND_ORDER[i]) return (int)i;
        }
        return (int)(sizeof(KIND_ORDER) / sizeof(KIND_ORDER[0]));
    }

    std::string num(float value){
        if (!std::isfinite(value)) value = 0.0f;
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%.3f", value);
        std::string text = buffer;
        while (!text.empty() && text.back() == '0') text.pop_back();
        if (!text.empty() && text.back() == '.') text.pop_back();
        if (text == "-0") text = "0";
        return text;
    }

    std::string vec(const Vector3& v){
        return "[" + num(v.x) + ", " + num(v.y) + ", " + num(v.z) + "]";
    }

    std::string percent(float share){
        char buffer[16];
        std::snprintf(buffer, sizeof(buffer), share < 0.1f ? "%.1f %%" : "%.0f %%", share * 100.0f);
        return buffer;
    }

    std::string label(Scene* scene, Entity entity){
        std::string name = scene->getEntityName(entity);
        if (name.empty()) name = "GameObject";
        return name + " (#" + std::to_string(entity) + ")";
    }

    std::string entityList(const std::vector<Entity>& entities){
        if (entities.size() == 1) return "entity: " + std::to_string(entities[0]);
        std::string text = "entities: [";
        for (size_t i = 0; i < entities.size(); i++){
            text += (i ? ", " : "") + std::to_string(entities[i]);
        }
        return text + "]";
    }

    AABB grown(const AABB& box, float by){
        return AABB(box.getMinimum() - Vector3(by, by, by), box.getMaximum() + Vector3(by, by, by));
    }

    bool usable(const AABB& box){
        return !box.isNull() && !box.isInfinite();
    }

    bool facePlane(const Triangle& t, Vector3& normal){
        normal = (t.b - t.a).crossProduct(t.c - t.a);
        const float length = normal.length();
        if (!(length > 1e-12f) || !std::isfinite(length)){
            return false;
        }
        normal = normal / length;
        return true;
    }

    // the projections of p and q on each edge normal of p: separated when one overlaps by no more
    // than minDepth
    bool separatedBy(const float p[3][2], const float q[3][2], float minDepth){
        for (int e = 0; e < 3; e++){
            const float ex = p[(e + 1) % 3][0] - p[e][0];
            const float ey = p[(e + 1) % 3][1] - p[e][1];
            const float length = std::sqrt(ex * ex + ey * ey);
            if (!(length > 1e-12f)){
                continue;
            }
            const float ax = -ey / length, ay = ex / length;
            float pMin = 3.4e38f, pMax = -3.4e38f, qMin = 3.4e38f, qMax = -3.4e38f;
            for (int i = 0; i < 3; i++){
                const float pv = p[i][0] * ax + p[i][1] * ay;
                const float qv = q[i][0] * ax + q[i][1] * ay;
                pMin = std::min(pMin, pv); pMax = std::max(pMax, pv);
                qMin = std::min(qMin, qv); qMax = std::max(qMax, qv);
            }
            if (std::min(pMax, qMax) - std::max(pMin, qMin) <= minDepth){
                return true;
            }
        }
        return false;
    }

    // the rectangle of NDC points a box projects to, its corners behind the eye clipped at it
    bool projectedRect(const Matrix4& viewProjection, const AABB& box, float& minX, float& minY, float& maxX, float& maxY, bool& inFront){
        constexpr float NEAR_W = 1e-4f;
        const Vector3* corners = box.getCorners();
        Vector4 clip[8];
        inFront = true;
        for (int i = 0; i < 8; i++){
            clip[i] = viewProjection * Vector4(corners[i], 1.0f);
            if (!(clip[i].w > NEAR_W)) inFront = false;
        }
        minX = minY = 3.4e38f;
        maxX = maxY = -3.4e38f;
        bool any = false;
        auto add = [&](const Vector4& c){
            const float x = c.x / c.w, y = c.y / c.w;
            if (!std::isfinite(x) || !std::isfinite(y)) return;
            minX = std::min(minX, x); maxX = std::max(maxX, x);
            minY = std::min(minY, y); maxY = std::max(maxY, y);
            any = true;
        };
        for (int i = 0; i < 8; i++){
            if (clip[i].w > NEAR_W) add(clip[i]);
        }
        if (!inFront){
            // every segment between a corner in front and one behind crosses the clip plane inside
            // the box: those crossings bound what is left of it
            for (int i = 0; i < 8; i++){
                for (int j = i + 1; j < 8; j++){
                    const bool fi = clip[i].w > NEAR_W, fj = clip[j].w > NEAR_W;
                    if (fi == fj) continue;
                    const float t = (NEAR_W - clip[i].w) / (clip[j].w - clip[i].w);
                    add(Vector4(clip[i].x + (clip[j].x - clip[i].x) * t, clip[i].y + (clip[j].y - clip[i].y) * t,
                                clip[i].z + (clip[j].z - clip[i].z) * t, NEAR_W));
                }
            }
        }
        return any;
    }

    struct LightPick {
        Entity entity = NULL_ENTITY;
        const LightComponent* light = nullptr;
    };

    // the light that puts the most on `target` (a directional light its intensity, the others
    // falling off with distance and stopping at their range)
    LightPick brightestLight(Scene* scene, const AABB& target){
        LightPick best;
        float bestScore = 0.0f;
        auto lights = scene->getComponentArray<LightComponent>();
        for (size_t i = 0; i < lights->size(); i++){
            const Entity entity = lights->getEntity(i);
            const LightComponent& light = lights->getComponentFromIndex(i);
            if (!light.enabled || !scene->isEntityActiveInHierarchy(entity)) continue;
            const Transform* transform = scene->findComponent<Transform>(entity);
            if (!transform || !transform->visible) continue;
            const float strength = light.resolvedIntensity * std::max(light.resolvedColor.x, std::max(light.resolvedColor.y, light.resolvedColor.z));
            float score = strength;
            if (light.type != LightType::DIRECTIONAL){
                const float distance = usable(target) ? target.distance(transform->worldPosition) : 0.0f;
                if (light.range > 0.0f && distance > light.range) continue;
                score = strength / std::max(distance * distance, 0.25f);
            }
            if (score > bestScore){
                bestScore = score;
                best.entity = entity;
                best.light = &light;
            }
        }
        return best;
    }

    const char* lightTypeName(LightType type){
        switch (type){
            case LightType::DIRECTIONAL: return "Directional";
            case LightType::POINT: return "Point";
            case LightType::SPOT: return "Spot";
            default: return "Light";
        }
    }

    // One run's state, shared by the checks
    struct Run {
        const Request& request;
        Project* project;
        SceneProject* sceneProject;
        Scene* scene;
        const CameraComponent& camera;
        Matrix4 viewProjection;
        Plane planes[6];
        PickFilter filter;
        std::vector<Entity> visible;
        std::unordered_set<Entity> visibleSet;
        // every drawn renderer, and the visible ones
        AABB drawnBounds;
        AABB visibleBounds;
        bool empty = false;
        std::vector<Entity> selection;
        std::vector<Finding> findings;

        Run(const Request& request, const CameraComponent& camera): request(request), project(request.project),
            sceneProject(request.sceneProject), scene(request.sceneProject->scene), camera(camera){}

        void add(const char* kind, std::string message, std::string fix, std::vector<Entity> entities = {}, Severity severity = Severity::WARNING){
            Finding finding;
            finding.kind = kind;
            finding.severity = severity;
            finding.message = std::move(message);
            finding.fix = std::move(fix);
            finding.entities = std::move(entities);
            findings.push_back(std::move(finding));
        }

        // The call that brings `subject` into the view: frame_entity in the Scene view; in the Game
        // view the camera moved (back, when closer is false, only as far as it must) and aimed at it
        std::string frameFix(const std::vector<Entity>& entities, const AABB& subject, bool closer) const{
            Scene* cameraScene = request.cameraScene;
            const Transform* transform = cameraScene->findComponent<Transform>(request.camera);
            const bool is3D = sceneProject->sceneType == SceneType::SCENE_3D;
            // frame_entity frames at least a metre across: a smaller subject is brought closer by hand
            if (request.sceneView && (!closer || !is3D || !transform || !usable(subject))){
                if (!entities.empty()) return "frame_entity {" + entityList(entities) + "}";
                return "set_scene_camera {frame_all: true}";
            }
            if (!transform || !usable(subject)){
                return "move the main camera (#" + std::to_string(request.camera) + ") with set_transform to look at it";
            }
            const Vector3 eye = transform->worldPosition;
            const Vector3 center = subject.getCenter();
            const float radius = std::max(subject.getHalfSize().length(), 0.01f);
            Vector3 forward = camera.worldDirection;
            if (forward.squaredLength() < 1e-12f) forward = Vector3(0.0f, 0.0f, -1.0f);
            forward.normalize();
            Vector3 newEye = eye;
            Vector3 target = center;
            if (camera.type == CameraType::CAMERA_PERSPECTIVE){
                const float halfY = std::max(camera.yfov * 0.5f, 0.01f);
                const float halfX = std::atan(std::tan(halfY) * std::max(camera.aspect, 0.0001f));
                // fitted, and never inside the near plane
                const float fit = std::max(radius * 1.1f / std::sin(std::min(halfY, halfX)), radius + 2.0f * std::max(camera.nearClip, 0.0f));
                Vector3 toCenter = center - eye;
                const float distance = toCenter.length();
                const Vector3 direction = distance > 1e-4f ? toCenter / distance : forward;
                newEye = center - direction * (closer ? fit : std::max(distance, fit));
                if (request.sceneView){
                    return "set_scene_camera {position: " + vec(newEye) + ", target: " + vec(center) + "}";
                }
            }else if (request.sceneView){
                return "set_scene_camera {target: " + vec(center) + ", ortho_size: " + num(radius * 1.25f) + "}";
            }else{
                // an orthographic camera slides across its view, keeping its depth
                const Vector3 toCenter = center - eye;
                newEye = eye + (toCenter - forward * toCenter.dotProduct(forward));
                target = camera.worldTarget + (newEye - eye);
            }
            const std::string id = std::to_string(request.camera);
            std::string fix = "set_transform {entity: " + id + ", position: " + vec(newEye) + ", space: \"world\"}";
            if (camera.useTarget){
                // the target is the camera's parent's space
                Vector3 local = target;
                if (transform->parent != NULL_ENTITY){
                    if (const Transform* parent = cameraScene->findComponent<Transform>(transform->parent)){
                        local = parent->modelMatrix.inverse() * target;
                    }
                }
                fix += " then set_properties {entity: " + id + ", values: {camera: {target: " + vec(local) + "}}}";
            }else{
                // a camera looks down its -Z
                Quaternion rotation = Quaternion::lookRotation(-(target - newEye), Vector3(0.0f, 1.0f, 0.0f));
                fix.insert(fix.size() - 1, ", rotation: " + vec(rotation.normalize().getEulerAngles(RotationOrder::ZYX)));
            }
            if (closer && camera.type != CameraType::CAMERA_PERSPECTIVE){
                fix += "; zoom in with the Camera's left/right/bottom/top";
            }
            return fix;
        }

        bool underSelection(Entity entity) const{
            for (int depth = 0; depth < 256 && entity != NULL_ENTITY; depth++){
                if (std::find(selection.begin(), selection.end(), entity) != selection.end()) return true;
                const Transform* transform = scene->findComponent<Transform>(entity);
                entity = transform ? transform->parent : NULL_ENTITY;
            }
            return false;
        }
    };

    // ---- the checks

    void checkExposure(Run& run){
        const Luminance& luminance = run.request.luminance;
        if (!luminance.valid){
            return;
        }
        if (luminance.clippedShare >= CLIPPED_SHARE){
            std::string message = percent(luminance.clippedShare) + " of the image is clipped white (luminance at or above " +
                                  num(CLIPPED_LUMINANCE) + ")";
            std::string fix;
            std::vector<Entity> entities;
            const LightPick light = run.empty ? LightPick() : brightestLight(run.scene, run.visibleBounds);
            if (light.light){
                message += "; the brightest light on it is " + label(run.scene, light.entity) + ", " + lightTypeName(light.light->type) +
                           " at intensity " + num(light.light->intensity) + ".";
                fix = "set_properties {entity: " + std::to_string(light.entity) + ", values: {light: {intensity: " +
                      num(light.light->intensity * 0.5f) + "}}} halves it";
                entities.push_back(light.entity);
            }else{
                message += "; no light explains it (the background, the sky, emissive materials or post-processing exposure).";
                fix = "get_scene_settings shows the background, sky and post-processing: lower the brightest";
            }
            run.add("exposure_clipped", std::move(message), std::move(fix), std::move(entities));
        }
        if (luminance.mean < DARK_MEAN && luminance.darkShare >= DARK_SHARE){
            // a dark clear colour with nothing drawn is a dark frame on purpose (empty_frame says why)
            const Vector4 clear = run.scene->getBackgroundColor();
            const float clearLuminance = 0.2126f * clear.x + 0.7152f * clear.y + 0.0722f * clear.z;
            if (run.empty && clearLuminance < DARK_MEAN){
                return;
            }
            std::string message = "The image is nearly black: mean luminance " + num(luminance.mean) + ", " +
                                  percent(luminance.darkShare) + " of the pixels under " + num(DARK_LUMINANCE) + ".";
            std::string fix;
            std::vector<Entity> entities;
            const LightPick light = brightestLight(run.scene, run.visibleBounds);
            if (run.sceneProject->sceneType != SceneType::SCENE_3D){
                fix = "set_scene_settings {settings: {ambient_light_2d_intensity: 1}}, or add a Light 2D";
            }else if (light.light){
                message += " The brightest light is " + label(run.scene, light.entity) + " at intensity " + num(light.light->intensity) + ".";
                fix = "set_properties {entity: " + std::to_string(light.entity) + ", values: {light: {intensity: " +
                      num(std::max(light.light->intensity, 0.25f) * 4.0f) + "}}}";
                entities.push_back(light.entity);
            }else{
                message += " No light reaches it.";
                fix = "create_entity {type: \"directional_light\"}";
            }
            run.add("exposure_dark", std::move(message), std::move(fix), std::move(entities));
        }
    }

    void checkEmpty(Run& run){
        if (!run.empty){
            return;
        }
        AABB subject;
        std::vector<Entity> entities;
        if (!run.selection.empty()){
            PickFilter selected = run.filter;
            selected.accept = [&run](Entity entity){ return run.underSelection(entity); };
            subject = run.scene->getPickBvh().getBounds(selected);
            if (usable(subject)) entities = run.selection;
        }
        if (!usable(subject)) subject = run.drawnBounds;
        if (!usable(subject)){
            run.add("empty_frame", "Nothing in the scene draws: it has no visible renderer.",
                    "create_entity {type: \"cube\"} (or a model) gives it something to show");
            return;
        }
        run.add("empty_frame", "Nothing is in view: no renderer's bounds meet the camera's frustum.",
                run.frameFix(entities, subject, false), entities);
    }

    void checkSubject(Run& run){
        if (run.empty){
            return;
        }
        if (!run.selection.empty()){
            PickFilter selected = run.filter;
            selected.accept = [&run](Entity entity){ return run.underSelection(entity); };
            const AABB bounds = run.scene->getPickBvh().getBounds(selected);
            if (!usable(bounds)){
                return;  // a selection that draws nothing (a light, an empty) has no frame to fill
            }
            const Projection projection = projectBounds(run.viewProjection, bounds, run.planes);
            const std::string what = run.selection.size() == 1 ? label(run.scene, run.selection[0])
                                                               : "The " + std::to_string(run.selection.size()) + " selected GameObjects";
            if (projection.outsideShare > OUT_OF_FRAME_SHARE){
                run.add("subject_out_of_frame", what + " (the selection) is " + percent(projection.outsideShare) + " outside the frame.",
                        run.frameFix(run.selection, bounds, false), run.selection);
            }else if (projection.coverage < TOO_SMALL_SHARE){
                run.add("subject_too_small", what + " (the selection) covers " + percent(projection.coverage) + " of the image.",
                        run.frameFix(run.selection, bounds, true), run.selection);
            }
            return;
        }

        // no selection: the visible renderers that could fit in the frame (a ground, a terrain or a
        // room around the eye never does, and is left out)
        float minX = 3.4e38f, minY = 3.4e38f, maxX = -3.4e38f, maxY = -3.4e38f;
        bool fitting = false;
        AABB fittingBounds;
        std::vector<std::pair<float, Entity>> cut;
        for (const SceneBvh::Leaf& leaf : run.scene->getPickBvh().getLeaves()){
            if (!run.visibleSet.count(leaf.entity) || !usable(leaf.bounds)) continue;
            const Projection projection = projectBounds(run.viewProjection, leaf.bounds, run.planes);
            if (!projection.inFront || projection.maxX - projection.minX > 2.0f || projection.maxY - projection.minY > 2.0f) continue;
            minX = std::min(minX, projection.minX); maxX = std::max(maxX, projection.maxX);
            minY = std::min(minY, projection.minY); maxY = std::max(maxY, projection.maxY);
            fitting = true;
            fittingBounds.merge(leaf.bounds);
            if (projection.outsideShare > OUT_OF_FRAME_SHARE) cut.emplace_back(projection.outsideShare, leaf.entity);
        }
        const float area = (maxX - minX) * (maxY - minY);
        if (!fitting || !(area > 1e-12f)){
            return;
        }
        const float inside = std::max(0.0f, std::min(maxX, 1.0f) - std::max(minX, -1.0f)) *
                             std::max(0.0f, std::min(maxY, 1.0f) - std::max(minY, -1.0f));
        const float outside = 1.0f - inside / area;
        if (outside <= OUT_OF_FRAME_SHARE){
            return;
        }
        std::sort(cut.begin(), cut.end(), [](const auto& a, const auto& b){ return a.first > b.first; });
        std::vector<Entity> entities;
        std::string names;
        for (const auto& item : cut){
            const Entity entity = item.second;
            if (std::find(entities.begin(), entities.end(), entity) != entities.end()) continue;
            if (entities.size() == MAX_FINDINGS) break;
            names += (entities.empty() ? "" : ", ") + label(run.scene, entity);
            entities.push_back(entity);
        }
        run.add("subject_out_of_frame", "What is in view is " + percent(outside) + " outside the frame (nothing is selected)" +
                (names.empty() ? std::string(".") : ": " + names + "."), run.frameFix({}, fittingBounds, false), entities);
    }

    void checkMissingTextures(Run& run){
        Project* project = run.project;
        MissingAssetTracker& tracker = project->getMissingAssets();
        if (run.request.refreshMissingAssets){
            tracker.update(project->getAssetDatabase(), project->getAssetIndex(), true);
        }
        if (tracker.getGroups().empty()){
            return;
        }
        const std::string scenePath = run.sceneProject->filepath.empty() ? std::string()
            : project->normalizeToProjectRelative(run.sceneProject->filepath).generic_string();
        // the material files the visible renderers draw
        struct MaterialUse { Entity entity; Guid guid; std::string path; };
        std::vector<MaterialUse> materials;
        for (Entity entity : run.visible){
            const MeshComponent* mesh = run.scene->findComponent<MeshComponent>(entity);
            if (!mesh) continue;
            for (unsigned int i = 0; i < mesh->numSubmeshes; i++){
                const AssetRef& ref = mesh->submeshes[i].material.name;
                if (ref.guid.isNull() && ref.path.empty()) continue;
                materials.push_back({entity, ref.guid, ref.path});
            }
        }
        std::vector<Entity> entities;
        std::string first;
        std::string firstFrom;
        for (const MissingAssetTracker::Group& group : tracker.getGroups()){
            for (const MissingAssetTracker::Referrer& referrer : group.referrers){
                Entity hit = NULL_ENTITY;
                std::string through;
                if (!scenePath.empty() && referrer.ownerPath == scenePath && referrer.entity != 0 &&
                    run.visibleSet.count((Entity)referrer.entity) &&
                    (group.expected == AssetType::Texture || group.expected == AssetType::Material)){
                    hit = (Entity)referrer.entity;
                    through = "its " + (referrer.component.empty() ? std::string() : referrer.component + ".") + referrer.field;
                }else{
                    for (const MaterialUse& use : materials){
                        if (!use.guid.isNull() ? referrer.owner == use.guid : referrer.ownerPath == use.path){
                            hit = use.entity;
                            through = "its material " + referrer.ownerPath + " (" + referrer.field + ")";
                            break;
                        }
                    }
                }
                if (hit == NULL_ENTITY || std::find(entities.begin(), entities.end(), hit) != entities.end()) continue;
                if (entities.empty()){
                    const std::string missing = group.lastPath.empty() ? group.guid.str() : group.lastPath;
                    first = label(run.scene, hit) + " draws with a missing asset: " + through + " references " + missing + ".";
                    firstFrom = !group.guid.isNull() ? group.guid.str() : group.lastPath;
                }
                entities.push_back(hit);
            }
        }
        if (entities.empty()){
            return;
        }
        if (entities.size() > 1){
            first += " " + std::to_string(entities.size() - 1) + " more visible GameObject(s) too.";
        }
        if (entities.size() > MAX_FINDINGS) entities.resize(MAX_FINDINGS);
        run.add("texture_missing", std::move(first),
                "retarget_asset_references {from: \"" + firstFrom + "\", to: \"<an existing texture>\"} (list_missing_assets lists what is missing)",
                std::move(entities));
    }

    struct MeshCandidate {
        Entity entity = NULL_ENTITY;
        AABB bounds;
        const MeshBvh* tree = nullptr;
        Matrix4 model;
    };

    void worldTriangles(const MeshCandidate& mesh, const AABB& region, std::vector<MeshBvh::Triangle>& local, std::vector<Triangle>& out){
        AABB box = region;
        box.transform(mesh.model.inverse());
        mesh.tree->overlapping(box, local, MAX_PAIR_TRIANGLES);
        out.clear();
        out.reserve(local.size());
        for (const MeshBvh::Triangle& t : local){
            out.push_back({mesh.model * t.v0, mesh.model * t.v1, mesh.model * t.v2});
        }
    }

    // true when it ran out of time before every pair was tested
    bool checkZFighting(Run& run, const std::function<bool()>& overBudget){
        // a 2D or UI scene, or a camera without a depth test, draws in its sorting order: faces on one
        // plane never fight there
        if (run.sceneProject->sceneType != SceneType::SCENE_3D || !run.camera.depthTest){
            return false;
        }
        SceneBvh& bvh = run.scene->getPickBvh();
        std::vector<MeshCandidate> meshes;
        for (const SceneBvh::Leaf& leaf : bvh.getLeaves()){
            if (leaf.kind != PickKind::MESH || !run.visibleSet.count(leaf.entity) || !usable(leaf.bounds)) continue;
            const MeshComponent* mesh = run.scene->findComponent<MeshComponent>(leaf.entity);
            const Transform* transform = run.scene->findComponent<Transform>(leaf.entity);
            // a skinned or morphed mesh moves every frame: its bind-pose faces are not what is drawn
            if (!mesh || !transform || jasperengine::picking::isPosed(*mesh)) continue;
            PickCache::EntryPtr entry = bvh.meshEntry(leaf.entity, true);
            const MeshBvh* tree = entry ? PickCache::ready(entry) : nullptr;
            // a small mesh's tree still queued is built here (a fraction of a millisecond); a large
            // one is skipped until its background build is done
            if (!tree && entry && entry->triangles <= INLINE_BUILD_TRIANGLES) tree = PickCache::get().wait(entry);
            if (!tree || tree->empty()) continue;  // still building, or no CPU triangles
            meshes.push_back({leaf.entity, leaf.bounds, tree, transform->modelMatrix});
        }
        std::sort(meshes.begin(), meshes.end(), [](const MeshCandidate& a, const MeshCandidate& b){
            return a.bounds.getMinimum().x < b.bounds.getMinimum().x;
        });

        size_t pairs = 0;
        Entity firstA = NULL_ENTITY, firstB = NULL_ENTITY;
        Vector3 firstNormal;
        std::vector<MeshBvh::Triangle> local;
        std::vector<Triangle> facesA, facesB;
        bool stopped = false;
        const float tolerance = COPLANAR_TOLERANCE;
        for (size_t i = 0; i < meshes.size() && !stopped; i++){
            const AABB a = grown(meshes[i].bounds, tolerance);
            for (size_t j = i + 1; j < meshes.size(); j++){
                if (meshes[j].bounds.getMinimum().x - tolerance > a.getMaximum().x) break;
                if (meshes[i].entity == meshes[j].entity) continue;
                const AABB region = a.intersection(grown(meshes[j].bounds, tolerance));
                if (region.isNull()) continue;
                if (overBudget()){
                    stopped = true;
                    break;
                }
                worldTriangles(meshes[i], region, local, facesA);
                if (facesA.empty()) continue;
                worldTriangles(meshes[j], region, local, facesB);
                if (facesB.empty()) continue;
                PlaneBuckets buckets(tolerance, region.getCenter(), region.getHalfSize().length());
                buckets.build(facesA);
                for (const Triangle& face : facesB){
                    const int found = buckets.findOverlap(face);
                    if (found < 0) continue;
                    if (pairs == 0){
                        // the smaller one is moved (a decal off its floor)
                        const bool iSmaller = meshes[i].bounds.getHalfSize().length() < meshes[j].bounds.getHalfSize().length();
                        firstA = iSmaller ? meshes[j].entity : meshes[i].entity;
                        firstB = iSmaller ? meshes[i].entity : meshes[j].entity;
                        firstNormal = buckets.normalOf(found);
                    }
                    pairs++;
                    break;
                }
            }
        }
        if (pairs > 0){
            const Transform* moved = run.scene->findComponent<Transform>(firstB);
            const Vector3 position = (moved ? moved->worldPosition : Vector3::ZERO) + firstNormal * SEPARATION;
            std::string message = label(run.scene, firstA) + " and " + label(run.scene, firstB) +
                                  " have coplanar overlapping faces (within " + num(tolerance * 1000.0f) + " mm): they flicker (z-fighting).";
            if (pairs > 1) message += " " + std::to_string(pairs - 1) + " more pair(s).";
            run.add("z_fighting", std::move(message),
                    "set_transform {entity: " + std::to_string(firstB) + ", position: " + vec(position) + ", space: \"world\"} moves " +
                    label(run.scene, firstB) + " 5 mm off (delete_entities if it is a duplicate)", {firstA, firstB});
        }
        return stopped;
    }

}

// ---- the parts

Luminance framechecks::measureLuminance(const uint8_t* pixels, int width, int height, int channels, int rx, int ry, int rw, int rh){
    Luminance result;
    if (!pixels || width <= 0 || height <= 0 || channels < 3){
        return result;
    }
    if (rw <= 0 || rh <= 0){
        rx = 0; ry = 0; rw = width; rh = height;
    }
    const int x0 = std::clamp(rx, 0, width), y0 = std::clamp(ry, 0, height);
    const int x1 = std::clamp(rx + rw, 0, width), y1 = std::clamp(ry + rh, 0, height);
    if (x1 <= x0 || y1 <= y0){
        return result;
    }
    const double area = (double)(x1 - x0) * (double)(y1 - y0);
    const int step = std::max(1, (int)std::ceil(std::sqrt(area / 262144.0)));
    // in levels of 255: the thresholds on the 8-bit image
    const float clipped = CLIPPED_LUMINANCE * 255.0f;
    const float dark = DARK_LUMINANCE * 255.0f;
    double sum = 0.0;
    size_t samples = 0, clippedCount = 0, darkCount = 0;
    for (int y = y0; y < y1; y += step){
        const uint8_t* row = pixels + (size_t)y * (size_t)width * (size_t)channels;
        for (int x = x0; x < x1; x += step){
            const uint8_t* p = row + (size_t)x * (size_t)channels;
            const float l = 0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2];
            sum += l;
            samples++;
            if (l >= clipped) clippedCount++;
            if (l < dark) darkCount++;
        }
    }
    result.valid = samples > 0;
    result.samples = samples;
    result.mean = (float)(sum / (double)samples / 255.0);
    result.clippedShare = (float)clippedCount / (float)samples;
    result.darkShare = (float)darkCount / (float)samples;
    return result;
}

const char* framechecks::severityName(Severity severity){
    return severity == Severity::WARNING ? "warning" : "info";
}

bool framechecks::coplanarOverlap(const Triangle& a, const Triangle& b, float tolerance){
    Vector3 na, nb;
    if (!facePlane(a, na) || !facePlane(b, nb)){
        return false;
    }
    if (na.dotProduct(nb) < COPLANAR_COS){
        return false;
    }
    auto onPlane = [tolerance](const Triangle& t, const Vector3& normal, const Vector3& point){
        return std::fabs(normal.dotProduct(t.a - point)) <= tolerance && std::fabs(normal.dotProduct(t.b - point)) <= tolerance &&
               std::fabs(normal.dotProduct(t.c - point)) <= tolerance;
    };
    if (!onPlane(b, na, a.a) && !onPlane(a, nb, b.a)){
        return false;
    }
    // in a's plane, measured from a's first corner (exact distances, not an axis drop)
    Vector3 u = na.perpendicular();
    u.normalize();
    const Vector3 v = na.crossProduct(u);
    float pa[3][2], pb[3][2];
    const Vector3* ca[3] = {&a.a, &a.b, &a.c};
    const Vector3* cb[3] = {&b.a, &b.b, &b.c};
    for (int i = 0; i < 3; i++){
        const Vector3 da = *ca[i] - a.a;
        const Vector3 db = *cb[i] - a.a;
        pa[i][0] = da.dotProduct(u); pa[i][1] = da.dotProduct(v);
        pb[i][0] = db.dotProduct(u); pb[i][1] = db.dotProduct(v);
    }
    return !separatedBy(pa, pb, tolerance) && !separatedBy(pb, pa, tolerance);
}

namespace {
    // a face covering more grid cells than this is a wide face
    constexpr int64_t MAX_FACE_CELLS = 1024;

    int dominantAxis(const Vector3& normal){
        const float ax = std::fabs(normal.x), ay = std::fabs(normal.y), az = std::fabs(normal.z);
        return (ax >= ay && ax >= az) ? 0 : (ay >= az ? 1 : 2);
    }
    int sideGroup(const Vector3& normal, int axis){
        return axis * 2 + (normal[(unsigned)axis] < 0.0f ? 1 : 0);
    }
    // the two axes across `axis`
    void acrossAxes(int axis, unsigned& u, unsigned& v){
        u = axis == 0 ? 1u : 0u;
        v = axis == 2 ? 1u : 2u;
    }
    int32_t gridIndex(float value, float size){
        const float cell = std::floor(value / size);
        return (int32_t)std::clamp(cell, -1.0e9f, 1.0e9f);
    }
}

size_t PlaneBuckets::CellHash::operator()(const Cell& cell) const{
    uint64_t h = (uint64_t)(uint32_t)cell.group * 0x9E3779B97F4A7C15ull;
    h ^= (uint64_t)(uint32_t)cell.x + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    h ^= (uint64_t)(uint32_t)cell.y + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return (size_t)h;
}

PlaneBuckets::PlaneBuckets(float tolerance, const Vector3& origin, float radius): tolerance(tolerance), origin(origin),
    radius(std::max(radius, 0.0f)){
}

bool PlaneBuckets::cellsOf(const Triangle& triangle, int axis, int32_t& x0, int32_t& y0, int32_t& x1, int32_t& y1) const{
    unsigned u, v;
    acrossAxes(axis, u, v);
    const Vector3 a = triangle.a - origin, b = triangle.b - origin, c = triangle.c - origin;
    x0 = gridIndex(std::min(a[u], std::min(b[u], c[u])), cellSize);
    x1 = gridIndex(std::max(a[u], std::max(b[u], c[u])), cellSize);
    y0 = gridIndex(std::min(a[v], std::min(b[v], c[v])), cellSize);
    y1 = gridIndex(std::max(a[v], std::max(b[v], c[v])), cellSize);
    return ((int64_t)x1 - x0 + 1) * ((int64_t)y1 - y0 + 1) <= MAX_FACE_CELLS;
}

float PlaneBuckets::heightAt(const Vector3& normal, float distance, int axis, int32_t x, int32_t y) const{
    unsigned u, v;
    acrossAxes(axis, u, v);
    // where the plane crosses the cell's centre line along the axis
    const float cu = ((float)x + 0.5f) * cellSize, cv = ((float)y + 0.5f) * cellSize;
    return (distance - normal[u] * cu - normal[v] * cv) / normal[(unsigned)axis];
}

bool PlaneBuckets::test(uint32_t face, const Triangle& triangle, const Vector3& normal) const{
    if (testedBy[face] == queries){
        return false;
    }
    testedBy[face] = queries;
    // the cheap rejections first: facing, then the corners' distances to the other's plane (what
    // rejects nearly every parallel neighbour), before the overlap in the plane
    const Face& candidate = faces[face];
    if (candidate.normal.dotProduct(normal) < COPLANAR_COS){
        return false;
    }
    auto onPlane = [this](const Triangle& t, const Vector3& n, const Vector3& p){
        return std::fabs(n.dotProduct(t.a - p)) <= tolerance && std::fabs(n.dotProduct(t.b - p)) <= tolerance &&
               std::fabs(n.dotProduct(t.c - p)) <= tolerance;
    };
    if (!onPlane(triangle, candidate.normal, candidate.triangle.a) && !onPlane(candidate.triangle, normal, triangle.a)){
        return false;
    }
    return coplanarOverlap(candidate.triangle, triangle, tolerance);
}

void PlaneBuckets::build(std::span<const Triangle> triangles){
    faces.clear();
    cells.clear();
    entries.clear();
    for (std::vector<uint32_t>& list : wide) list.clear();
    faces.reserve(triangles.size());
    for (const Triangle& triangle : triangles){
        Face face;
        if (!facePlane(triangle, face.normal)) continue;
        face.triangle = triangle;
        face.distance = face.normal.dotProduct(triangle.a - origin);
        faces.push_back(face);
    }
    testedBy.assign(faces.size(), 0u);
    queries = 0;
    // about one face per cell; two faces that meet within tolerance at a point of a cell have plane
    // heights at its centre that differ by under 1.8 tolerance (the axis is at least 1/sqrt(3) of a
    // unit normal) plus their slopes' difference (under 0.05 for normals COPLANAR_COS apart) times
    // the 0.71 cells to the centre
    cellSize = std::max(4.0f * tolerance, 2.0f * radius / std::max(1.0f, std::sqrt((float)faces.size())));
    heightSlack = 2.0f * tolerance + 0.04f * cellSize;
    struct Item {
        Cell cell;
        Entry entry;
    };
    std::vector<Item> items;
    items.reserve(faces.size() * 4);
    for (uint32_t index = 0; index < (uint32_t)faces.size(); index++){
        const Face& face = faces[index];
        const int axis = dominantAxis(face.normal);
        const int group = sideGroup(face.normal, axis);
        int32_t x0, y0, x1, y1;
        if (!cellsOf(face.triangle, axis, x0, y0, x1, y1)){
            wide[group].push_back(index);
            continue;
        }
        for (int32_t x = x0; x <= x1; x++){
            for (int32_t y = y0; y <= y1; y++){
                items.push_back({Cell{group, x, y}, Entry{index, heightAt(face.normal, face.distance, axis, x, y)}});
            }
        }
    }
    // one flat array, a cell's entries side by side
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b){
        if (a.cell.group != b.cell.group) return a.cell.group < b.cell.group;
        if (a.cell.x != b.cell.x) return a.cell.x < b.cell.x;
        return a.cell.y < b.cell.y;
    });
    entries.reserve(items.size());
    cells.reserve(items.size());
    for (size_t i = 0; i < items.size(); i++){
        if (i == 0 || !(items[i].cell == items[i - 1].cell)){
            cells[items[i].cell] = {(uint32_t)entries.size(), 0u};
        }
        cells[items[i].cell].second++;
        entries.push_back(items[i].entry);
    }
}

int PlaneBuckets::findOverlap(const Triangle& triangle) const{
    Vector3 normal;
    if (faces.empty() || !facePlane(triangle, normal)){
        return -1;
    }
    if (++queries == 0){
        // the stamps wrapped: start them again
        std::fill(testedBy.begin(), testedBy.end(), 0u);
        queries = 1;
    }
    const float distance = normal.dotProduct(triangle.a - origin);
    const float largest = std::max(std::fabs(normal.x), std::max(std::fabs(normal.y), std::fabs(normal.z)));
    for (int axis = 0; axis < 3; axis++){
        // a normal near the boundary of two axes: its partners may be filed under the other one
        if (std::fabs(normal[(unsigned)axis]) < largest - AXIS_MARGIN) continue;
        const int group = sideGroup(normal, axis);
        for (uint32_t index : wide[group]){
            if (test(index, triangle, normal)) return (int)index;
        }
        int32_t x0, y0, x1, y1;
        if (!cellsOf(triangle, axis, x0, y0, x1, y1)){
            // a wide face of its own: every face of the side
            for (const Entry& entry : entries){
                if (sideGroup(faces[entry.face].normal, dominantAxis(faces[entry.face].normal)) != group) continue;
                if (test(entry.face, triangle, normal)) return (int)entry.face;
            }
            continue;
        }
        for (int32_t x = x0; x <= x1; x++){
            for (int32_t y = y0; y <= y1; y++){
                const auto found = cells.find(Cell{group, x, y});
                if (found == cells.end()) continue;
                const float height = heightAt(normal, distance, axis, x, y);
                for (uint32_t i = found->second.first; i < found->second.first + found->second.second; i++){
                    const Entry& entry = entries[i];
                    if (std::fabs(entry.height - height) > heightSlack) continue;
                    if (test(entry.face, triangle, normal)) return (int)entry.face;
                }
            }
        }
    }
    return -1;
}

Projection framechecks::projectBounds(const Matrix4& viewProjection, const AABB& box, const Plane* planes){
    Projection result;
    if (!usable(box)){
        return result;
    }
    if (planes){
        for (int p = 0; p < 6; p++){
            if (planes[p].getSide(box) == Plane::NEGATIVE_SIDE){
                return result;  // wholly outside the frustum
            }
        }
    }
    if (!projectedRect(viewProjection, box, result.minX, result.minY, result.maxX, result.maxY, result.inFront)){
        return result;
    }
    const float area = (result.maxX - result.minX) * (result.maxY - result.minY);
    const float inside = std::max(0.0f, std::min(result.maxX, 1.0f) - std::max(result.minX, -1.0f)) *
                         std::max(0.0f, std::min(result.maxY, 1.0f) - std::max(result.minY, -1.0f));
    if (!(area > 1e-12f)){
        // a point or a line seen edge on: in or out
        const float x = (result.minX + result.maxX) * 0.5f, y = (result.minY + result.maxY) * 0.5f;
        result.outsideShare = (x >= -1.0f && x <= 1.0f && y >= -1.0f && y <= 1.0f) ? 0.0f : 1.0f;
        result.coverage = 0.0f;
        return result;
    }
    result.outsideShare = std::clamp(1.0f - inside / area, 0.0f, 1.0f);
    result.coverage = inside / 4.0f;
    return result;
}

// ---- the run

framechecks::Result framechecks::run(const Request& request){
    Result result;
    const Clock::time_point start = Clock::now();
    auto elapsedMs = [&start](){ return std::chrono::duration<double, std::milli>(Clock::now() - start).count(); };
    if (!request.project || !request.sceneProject || !request.sceneProject->scene || !request.cameraScene){
        return result;
    }
    const CameraComponent* camera = request.cameraScene->findComponent<CameraComponent>(request.camera);
    if (!camera){
        return result;
    }

    Run run(request, *camera);
    Scene* scene = run.scene;
    run.viewProjection = camera->viewProjectionMatrix;
    for (int p = 0; p < 6; p++) run.planes[p] = camera->frustumPlanes[p];
    // an infinite far plane (farClip 0) culls nothing, as RenderSystem::isInsideCamera reads it
    if (camera->farClip == 0.0f) run.planes[FRUSTUM_PLANE_FAR] = Plane(Vector3::ZERO, 1.0f);

    // what draws: meshes, terrains, trees and what has bounds only (particles, lines, points);
    // a 2D or UI scene's UI too (a 3D scene's UI is an overlay, never out of frame)
    const bool is3D = request.sceneProject->sceneType == SceneType::SCENE_3D;
    run.filter.layerMask = request.layerMask & camera->cullingMask;
    run.filter.kinds = PICK_MESH | PICK_INSTANCE | PICK_TERRAIN | PICK_BOUNDS | PICK_TREE | (is3D ? 0u : PICK_UI);
    run.filter.waitForBvh = false;
    run.filter.excludeEditorFlags = request.sceneView ? EDITOR_HIDDEN : 0;
    SceneBvh& bvh = scene->getPickBvh();
    bvh.frustum(run.planes, run.filter, run.visible);
    run.visibleSet.insert(run.visible.begin(), run.visible.end());
    run.drawnBounds = bvh.getBounds(run.filter);
    for (const SceneBvh::Leaf& leaf : bvh.getLeaves()){
        if (run.visibleSet.count(leaf.entity) && usable(leaf.bounds)) run.visibleBounds.merge(leaf.bounds);
    }
    bool uiDrawn = false;
    if (is3D && !request.sceneView){
        PickFilter ui = run.filter;
        ui.kinds = PICK_UI;
        uiDrawn = !bvh.getBounds(ui).isNull();
    }
    run.empty = run.visible.empty() && !uiDrawn;
    for (Entity entity : request.sceneProject->selectedEntities){
        if (scene->isEntityCreated(entity)) run.selection.push_back(entity);
    }

    struct Step {
        const char* kind;
        std::function<void()> check;
    };
    bool zPartial = false;
    auto overBudget = [&](){ return elapsedMs() > request.budgetMs; };
    const Step steps[] = {
        {"exposure", [&](){ checkExposure(run); }},
        {"empty_frame", [&](){ checkEmpty(run); }},
        {"subject", [&](){ checkSubject(run); }},
        {"texture_missing", [&](){ checkMissingTextures(run); }},
        {"z_fighting", [&](){ zPartial = checkZFighting(run, overBudget); }},
    };
    std::string skipped;
    for (const Step& step : steps){
        if (overBudget()){
            skipped += (skipped.empty() ? "" : ", ") + std::string(step.kind);
            continue;
        }
        step.check();
    }
    if (zPartial){
        skipped += (skipped.empty() ? "" : ", ") + std::string("z_fighting (some pairs)");
    }

    std::vector<Finding>& findings = run.findings;
    std::stable_sort(findings.begin(), findings.end(), [](const Finding& a, const Finding& b){
        if (a.severity != b.severity) return a.severity == Severity::WARNING;
        return kindRank(a.kind) < kindRank(b.kind);
    });
    if (!skipped.empty()){
        // kept past the cut: the agent learns the list is not complete
        if (findings.size() >= MAX_FINDINGS) findings.resize(MAX_FINDINGS - 1);
        Finding info;
        info.kind = "checks_skipped";
        info.severity = Severity::INFO;
        info.message = "The frame checks passed their " + num((float)request.budgetMs) + " ms budget; not checked: " + skipped + ".";
        info.fix = "screenshot again with fewer objects in view (or a closer camera) to check the rest";
        findings.push_back(std::move(info));
    }else if (findings.size() > MAX_FINDINGS){
        findings.resize(MAX_FINDINGS);
    }
    result.findings = std::move(findings);
    result.ms = elapsedMs();
    return result;
}
