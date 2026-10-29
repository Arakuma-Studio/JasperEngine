// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "PoseSheetRenderer.h"

#include "pose/ModelRig.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <system_error>

using namespace jasperengine;

namespace {

    constexpr float PI_F = 3.14159265358979f;

    // PoseRender's default light directions for a character facing +Z with its left on +X,
    // in canonical axes (L, U, F): key from the front-left-above, fill from the front-right
    const Vector3 KEY_LIGHT_CANONICAL(-0.45f, -0.6f, -0.65f);
    const Vector3 FILL_LIGHT_CANONICAL(0.6f, -0.25f, -0.75f);

    // tips drawn past the head and the hands, in humanScale units
    constexpr float HEAD_TIP = 0.2f;
    constexpr float HAND_TIP = 0.08f;

    bool sameView(const editor::PoseView& a, const editor::PoseView& b){
        return a.kind == b.kind;
    }

    // a model file as it is on disk: a changed file is a new stamp
    std::string fileStamp(const std::string& file){
        namespace fs = std::filesystem;
        std::error_code ec;
        const fs::path path = fs::u8path(file);
        const auto size = fs::file_size(path, ec);
        if (ec) return "";
        const auto time = fs::last_write_time(path, ec);
        if (ec) return "";
        return std::to_string(static_cast<unsigned long long>(size)) + ":" +
               std::to_string(static_cast<long long>(time.time_since_epoch().count()));
    }

    struct SharedRenderer{
        std::string modelFile;
        std::string stamp;
        std::shared_ptr<editor::PoseSheetRenderer> renderer;
    };

    // never destroyed: renderers own GPU objects the editor frees itself at shutdown
    std::vector<SharedRenderer>& sharedRenderers(){
        static auto* renderers = new std::vector<SharedRenderer>();
        return *renderers;
    }

    // the direction from bone `from` to bone `to` in the rest pose, in `to`'s own frame
    Vector3 restDirection(const pose::PoseRig& rig, pose::HumanBone from, pose::HumanBone to){
        if (!rig.has(from) || !rig.has(to)){
            return Vector3(0.0f, 0.0f, 0.0f);
        }
        std::vector<Quaternion> rot;
        std::vector<Vector3> pos;
        rig.computeWorld(rig.restPose(), rot, pos);
        const int a = rig.boneIndex(from);
        const int b = rig.boneIndex(to);
        Vector3 direction = pos[b] - pos[a];
        if (direction.squaredLength() < 1e-12f){
            return Vector3(0.0f, 0.0f, 0.0f);
        }
        direction.normalize();
        return rot[b].inverse() * direction;
    }

}

const char* editor::PoseView::name(Kind kind){
    switch (kind){
        case Front: return "front";
        case Side: return "side";
        case ThreeQuarter: return "three_quarter";
        case Back: return "back";
        case Top: return "top";
    }
    return "side";
}

bool editor::PoseView::fromName(const std::string& text, PoseView& out){
    for (Kind kind : {Front, Side, ThreeQuarter, Back, Top}){
        if (text == name(kind)){
            out = PoseView();
            out.kind = kind;
            return true;
        }
    }
    return false;
}

// ---- model ----

std::shared_ptr<editor::PoseSheetRenderer> editor::PoseSheetRenderer::shared(const std::string& modelFile,
                                                                         const std::shared_ptr<pose::PoseRig>& rig,
                                                                         size_t kept, std::string* error){
    std::vector<SharedRenderer>& renderers = sharedRenderers();
    const std::string stamp = fileStamp(modelFile);
    auto it = std::find_if(renderers.begin(), renderers.end(), [&](const SharedRenderer& entry){ return entry.modelFile == modelFile; });
    std::shared_ptr<PoseSheetRenderer> renderer;
    // the same file as loaded, and the same rig or a scene model's rig of its skeleton and map
    if (it != renderers.end() && it->stamp == stamp && it->renderer->sameRig(rig) && it->renderer->setModel(modelFile, rig, error)){
        renderer = it->renderer;
    }else{
        renderer = std::make_shared<PoseSheetRenderer>();
        if (!renderer->setModel(modelFile, rig, error)) return nullptr;
    }
    if (it != renderers.end()) renderers.erase(it);
    while (!renderers.empty() && renderers.size() >= std::max<size_t>(kept, 1)) renderers.erase(renderers.begin());
    renderers.push_back({modelFile, stamp, renderer});
    return renderer;
}

void editor::PoseSheetRenderer::clearShared(){
    sharedRenderers().clear();
}

bool editor::PoseSheetRenderer::setModel(const std::string& modelFile, const std::shared_ptr<pose::PoseRig>& rig, std::string* error){
    if (!rig || !rig->valid()){
        if (error) *error = "no usable pose rig for " + modelFile;
        return false;
    }
    if (poseRig == rig && modelPath == modelFile && render.hasModel()){
        return true;
    }
    if (modelPath == modelFile && render.hasModel() && sameRig(rig)){
        // the same skeleton and map (a scene model of this file): the bone binding still holds
        poseRig = rig;
        batches.clear();
        headTip = restDirection(*poseRig, poseRig->effectiveParent(pose::HumanBone::Head), pose::HumanBone::Head);
        leftHandTip = restDirection(*poseRig, pose::HumanBone::LeftLowerArm, pose::HumanBone::LeftHand);
        rightHandTip = restDirection(*poseRig, pose::HumanBone::RightLowerArm, pose::HumanBone::RightHand);
        return true;
    }
    poseRig.reset();
    modelPath.clear();
    batches.clear();
    if (!render.setModel(modelFile, error)){
        return false;
    }
    poseRig = rig;
    if (!bindSkeleton(error)){
        poseRig.reset();
        render.clearModel();
        return false;
    }
    modelPath = modelFile;

    headTip = restDirection(*poseRig, poseRig->effectiveParent(pose::HumanBone::Head), pose::HumanBone::Head);
    leftHandTip = restDirection(*poseRig, pose::HumanBone::LeftLowerArm, pose::HumanBone::LeftHand);
    rightHandTip = restDirection(*poseRig, pose::HumanBone::RightLowerArm, pose::HumanBone::RightHand);
    return true;
}

bool editor::PoseSheetRenderer::sameRig(const std::shared_ptr<pose::PoseRig>& rig) const{
    if (!poseRig || !rig){
        return false;
    }
    if (poseRig == rig){
        return true;
    }
    const std::vector<pose::SkeletonBone>& a = poseRig->skeleton().bones;
    const std::vector<pose::SkeletonBone>& b = rig->skeleton().bones;
    if (a.size() != b.size() || poseRig->humanoid().bones != rig->humanoid().bones){
        return false;
    }
    for (size_t i = 0; i < a.size(); i++){
        if (a[i].name != b[i].name || a[i].parent != b[i].parent || a[i].position != b[i].position ||
                a[i].rotation != b[i].rotation || a[i].scale != b[i].scale){
            return false;
        }
    }
    return true;
}

bool editor::PoseSheetRenderer::bindSkeleton(std::string* error){
    // the renderer's own skeleton of the model, with the non-bone ancestors ModelRig folds in
    pose::Skeleton skeleton;
    std::vector<Entity> entities;
    std::vector<Matrix4> folded;
    if (!pose::buildSkeleton(render.getScene(), render.getModelEntity(), skeleton, entities, &folded)){
        if (error) *error = render.getModelPath() + " has no skeleton (bones)";
        return false;
    }

    const std::vector<pose::SkeletonBone>& rigBones = poseRig->skeleton().bones;
    renderBone.assign(rigBones.size(), -1);
    bridges.assign(rigBones.size(), Matrix4());
    bridged.assign(rigBones.size(), false);
    for (size_t i = 0; i < rigBones.size(); i++){
        for (size_t j = 0; j < skeleton.bones.size() && j < entities.size(); j++){
            if (skeleton.bones[j].name != rigBones[i].name){
                continue;
            }
            for (int k = 0; k < render.getBoneCount(); k++){
                if (render.getBone(k).entity == entities[j]){
                    renderBone[i] = k;
                    break;
                }
            }
            if (j < folded.size() && !(folded[j] == Matrix4())){
                bridges[i] = folded[j];
                bridged[i] = true;
            }
            break;
        }
    }

    for (int b = 0; b < pose::HUMAN_BONE_COUNT; b++){
        const int bone = poseRig->boneIndex(pose::boneAt(b));
        if (bone >= 0 && renderBone[bone] < 0){
            if (error) *error = "the skeleton of " + render.getModelPath() + " has no bone '" + rigBones[bone].name +
                                "' of its pose rig (" + pose::humanBoneName(pose::boneAt(b)) + ")";
            return false;
        }
    }
    return true;
}

// ---- poses ----

void editor::PoseSheetRenderer::applyPose(const pose::HumanPose& pose){
    render.resetToRest();
    const std::vector<pose::SkeletonBone>& rigBones = poseRig->skeleton().bones;
    for (int b = 0; b < pose::HUMAN_BONE_COUNT; b++){
        const pose::HumanBone slot = pose::boneAt(b);
        const int bone = poseRig->boneIndex(slot);
        if (bone < 0 || renderBone[bone] < 0){
            continue;
        }
        // the pose is in skeleton-local terms (as the bake writes it): rest position and scale,
        // the slot's rotation, the hips' position
        const pose::SkeletonBone& rest = rigBones[bone];
        Vector3 position = slot == pose::HumanBone::Hips ? pose.hipsPosition : rest.position;
        Quaternion rotation = pose.rotations[b];
        Vector3 scale = rest.scale;
        if (bridged[bone]){
            const Matrix4 local = bridges[bone].inverse() * Matrix4::translateMatrix(position) *
                                  rotation.getRotationMatrix() * Matrix4::scaleMatrix(scale);
            local.decompose(position, scale, rotation);
        }
        render.setBoneLocal(renderBone[bone], position, rotation, scale);
    }
}

void editor::PoseSheetRenderer::posePoints(const pose::HumanPose& pose, std::vector<Vector3>& out) const{
    std::vector<Matrix4> world;
    poseRig->computeWorldMatrices(pose, world);

    const pose::PoseSkin* skin = poseRig->skin();
    bool skinned = false;
    if (skin && !skin->vertices.empty()){
        const size_t boneCount = std::min(world.size(), skin->inverseBind.size());
        std::vector<Matrix4> skinning(boneCount);
        for (size_t j = 0; j < boneCount; j++){
            skinning[j] = world[j] * skin->inverseBind[j];
        }
        for (const pose::PoseSkinVertex& v : skin->vertices){
            Vector3 p(0.0f, 0.0f, 0.0f);
            bool any = false;
            for (int i = 0; i < 4; i++){
                const int bone = v.bones[i];
                if (bone < 0 || static_cast<size_t>(bone) >= boneCount || v.weights[i] == 0.0f){
                    continue;
                }
                p += (skinning[bone] * v.position) * v.weights[i];
                any = true;
            }
            if (any){
                out.push_back(p);
                skinned = true;
            }
        }
    }
    if (!skinned){
        for (const Matrix4& m : world){
            out.push_back(Vector3(m.get(3, 0), m.get(3, 1), m.get(3, 2)));
        }
    }
    // the joints always (a skin sample may skip an extremity)
    for (int b = 0; b < pose::HUMAN_BONE_COUNT; b++){
        const int bone = poseRig->boneIndex(pose::boneAt(b));
        if (bone >= 0 && static_cast<size_t>(bone) < world.size()){
            const Matrix4& m = world[bone];
            out.push_back(Vector3(m.get(3, 0), m.get(3, 1), m.get(3, 2)));
        }
    }
}

// ---- views and framing ----

Vector3 editor::PoseSheetRenderer::canonicalToModel(const Vector3& c) const{
    return poseRig->characterLeft() * c.x + poseRig->characterUp() * c.y + poseRig->characterForward() * c.z;
}

bool editor::PoseSheetRenderer::viewAxes(const PoseView& view, Vector3& direction, Vector3& up) const{
    if (!poseRig){
        return false;
    }
    // canonical: L = +x, U = +y, F = +z; the direction is where the camera looks
    Vector3 dir(1.0f, 0.0f, 0.0f);
    Vector3 screenUp(0.0f, 1.0f, 0.0f);
    switch (view.kind){
        case PoseView::Front:
            dir = Vector3(0.0f, 0.0f, -1.0f);
            break;
        case PoseView::Side:
            dir = Vector3(1.0f, 0.0f, 0.0f);
            break;
        case PoseView::ThreeQuarter:{
            // Front rotated -40 degrees about U: the camera toward the character's front-right
            const float a = -40.0f * PI_F / 180.0f;
            dir = Vector3(-std::sin(a), 0.0f, -std::cos(a));
            break;
        }
        case PoseView::Back:
            dir = Vector3(0.0f, 0.0f, 1.0f);
            break;
        case PoseView::Top:
            dir = Vector3(0.0f, -1.0f, 0.0f);
            screenUp = Vector3(0.0f, 0.0f, 1.0f);
            break;
    }
    direction = canonicalToModel(dir);
    up = canonicalToModel(screenUp);
    return true;
}

void editor::PoseSheetRenderer::beginBatch(const std::vector<pose::HumanPose>& posesForFraming, const PoseView& view){
    if (!poseRig){
        return;
    }
    Batch batch;
    batch.view = view;
    for (const pose::HumanPose& pose : posesForFraming){
        posePoints(pose, batch.points);
    }
    batches.erase(std::remove_if(batches.begin(), batches.end(), [&](const Batch& b) { return sameView(b.view, view); }),
                  batches.end());
    batches.push_back(std::move(batch));
}

void editor::PoseSheetRenderer::endBatches(){
    batches.clear();
}

void editor::PoseSheetRenderer::setCamera(const pose::HumanPose& pose, const PoseView& view){
    Vector3 direction, up;
    viewAxes(view, direction, up);

    std::vector<Vector3> points;
    const Batch* batch = nullptr;
    for (const Batch& b : batches){
        if (sameView(b.view, view)) batch = &b;
    }
    if (batch){
        points = batch->points;
    }else{
        posePoints(pose, points);
    }
    // the floor under the framed points stays in frame (a jump shows its height)
    if (!points.empty()){
        Vector3 low = points[0];
        Vector3 high = points[0];
        for (const Vector3& p : points){
            low = Vector3(std::min(low.x, p.x), std::min(low.y, p.y), std::min(low.z, p.z));
            high = Vector3(std::max(high.x, p.x), std::max(high.y, p.y), std::max(high.z, p.z));
        }
        points.push_back(Vector3((low.x + high.x) * 0.5f, poseRig->floorHeight(), (low.z + high.z) * 0.5f));
    }
    render.setView(direction, up, points, MARGIN);
}

void editor::PoseSheetRenderer::prepare(const pose::HumanPose& pose, const PoseView& view, int width, int height,
                                         const PoseRenderStyle& style){
    applyPose(pose);
    setCamera(pose, view);
    render.setViewSize(width, height);

    PoseRender::Style look;
    look.background = style.background;
    look.floorVisible = true;
    look.floorHeight = poseRig->floorHeight();
    look.shaded = style.shaded;
    look.modelVisible = true;
    look.opaque = true;
    render.setStyle(look);
    render.setLighting(canonicalToModel(KEY_LIGHT_CANONICAL), canonicalToModel(FILL_LIGHT_CANONICAL));
}

// ---- overlay ----

editor::PoseSheetRenderer::Overlay editor::PoseSheetRenderer::makeOverlay(const pose::HumanPose& pose, const PoseRenderStyle& style) const{
    Overlay overlay;
    const Vector3 direction = render.getViewDirection();

    if (style.floorLine){
        // the floor is horizontal (model up is +Y): a line across the cell in views with up +Y
        const Vector3 up = render.getViewUp();
        if (std::fabs(up.y) > 0.999f){
            overlay.floor = true;
            overlay.floorY = render.project(Vector3(0.0f, poseRig->floorHeight(), 0.0f)).y;
        }
    }
    if (!style.skeletonOverlay){
        return overlay;
    }

    std::vector<Quaternion> rot;
    std::vector<Vector3> pos;
    poseRig->computeWorld(pose, rot, pos);
    const float scale = poseRig->humanScale();

    auto colorOf = [](pose::HumanBone bone){
        switch (pose::humanBoneInfo(bone).side){
            case pose::BodySide::Left: return LEFT_COLOR;
            case pose::BodySide::Right: return RIGHT_COLOR;
            default: return CENTER_COLOR;
        }
    };
    auto addSegment = [&](const Vector3& a, const Vector3& b, PoseColor color){
        Segment segment;
        segment.a = render.project(a);
        segment.b = render.project(b);
        segment.depth = ((a + b) * 0.5f).dotProduct(direction);
        segment.color = color;
        overlay.segments.push_back(segment);
    };
    auto addTip = [&](pose::HumanBone bone, const Vector3& restDir, float length){
        if (!poseRig->has(bone) || restDir.squaredLength() < 1e-12f){
            return;
        }
        const int index = poseRig->boneIndex(bone);
        addSegment(pos[index], pos[index] + (rot[index] * restDir) * (length * scale), colorOf(bone));
    };

    for (int b = 0; b < pose::HUMAN_BONE_COUNT; b++){
        const pose::HumanBone bone = pose::boneAt(b);
        if (!poseRig->has(bone)){
            continue;
        }
        const Vector3& at = pos[poseRig->boneIndex(bone)];
        const pose::HumanBone parent = poseRig->effectiveParent(bone);
        if (parent != pose::HumanBone::Count && poseRig->has(parent)){
            addSegment(pos[poseRig->boneIndex(parent)], at, colorOf(bone));
        }
        Joint joint;
        joint.at = render.project(at);
        joint.depth = at.dotProduct(direction);
        const bool effector = bone == pose::HumanBone::LeftHand || bone == pose::HumanBone::RightHand ||
                              bone == pose::HumanBone::LeftFoot || bone == pose::HumanBone::RightFoot;
        joint.diameter = effector ? EFFECTOR_DIAMETER : JOINT_DIAMETER;
        joint.color = colorOf(bone);
        overlay.joints.push_back(joint);
    }
    addTip(pose::HumanBone::Head, headTip, HEAD_TIP);
    addTip(pose::HumanBone::LeftHand, leftHandTip, HAND_TIP);
    addTip(pose::HumanBone::RightHand, rightHandTip, HAND_TIP);

    // far to near: the side nearer the camera is drawn over the other
    std::stable_sort(overlay.segments.begin(), overlay.segments.end(),
                     [](const Segment& a, const Segment& b) { return a.depth > b.depth; });
    std::stable_sort(overlay.joints.begin(), overlay.joints.end(),
                     [](const Joint& a, const Joint& b) { return a.depth > b.depth; });
    return overlay;
}

void editor::PoseSheetRenderer::drawOverlay(const Overlay& overlay, PoseImage& image){
    if (overlay.floor && std::isfinite(overlay.floorY)){
        // pixel aligned, so it stays crisp when the sheet is scaled down
        const int y = static_cast<int>(std::lround(overlay.floorY)) - 1;
        image.fillRect(0, y, image.getWidth(), static_cast<int>(FLOOR_LINE_WIDTH), FLOOR_COLOR);
    }
    for (const Segment& s : overlay.segments){
        image.drawLine(s.a.x, s.a.y, s.b.x, s.b.y, LINE_WIDTH, s.color);
    }
    for (const Joint& j : overlay.joints){
        image.fillCircle(j.at.x, j.at.y, j.diameter, j.color);
    }
}

void editor::PoseSheetRenderer::projectJoints(const pose::HumanPose& pose, PoseJoints& out) const{
    out.clear();
    std::vector<Quaternion> rot;
    std::vector<Vector3> pos;
    poseRig->computeWorld(pose, rot, pos);
    for (int b = 0; b < pose::HUMAN_BONE_COUNT; b++){
        const pose::HumanBone bone = pose::boneAt(b);
        if (poseRig->has(bone)){
            out.emplace_back(bone, render.project(pos[poseRig->boneIndex(bone)]));
        }
    }
}

// ---- rendering ----

void editor::PoseSheetRenderer::requestCell(const pose::HumanPose& pose, const PoseView& view, int width, int height,
                                            const PoseRenderStyle& style, CellCallback done, PoseJoints* joints){
    if (!poseRig){
        if (done) done(false, PoseImage());
        return;
    }
    prepare(pose, view, width, height, style);
    Overlay overlay = makeOverlay(pose, style);
    if (joints){
        projectJoints(pose, *joints);
    }
    render.requestRender(width, height, [overlay = std::move(overlay), done = std::move(done)](bool ok, PoseImage&& image){
        if (ok){
            drawOverlay(overlay, image);
        }
        if (done) done(ok, std::move(image));
    });
}
