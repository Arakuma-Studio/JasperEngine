// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "PoseThumbnails.h"

#include "Project.h"
#include "pose/PoseAnimationIO.h"
#include "pose/PoseImage.h"
#include "pose/PoseMath.h"
#include "pose/PoseRigProvider.h"
#include "pose/PoseSampler.h"
#include "render/preview/PoseRender.h"
#include "util/FileUtils.h"
#include "util/ProjectChecks.h"
#include "util/SafeWrite.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include "util/TerrainMapFileWriter.h"
#include "profiler/GpuMemory.h"

using namespace jasperengine;

namespace fs = std::filesystem;

namespace {

    // FNV-1a over raw bytes
    uint64_t mix(uint64_t hash, const void* data, size_t size){
        const auto* bytes = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < size; i++){
            hash ^= bytes[i];
            hash *= 1099511628211ull;
        }
        return hash;
    }

    constexpr uint64_t FNV_OFFSET = 1469598103934665603ull;

    // the card background, so thumbnails sit on the strip without a visible box
    const Vector4 THUMBNAIL_BACKGROUND(0.19f, 0.19f, 0.20f, 1.0f);

    // A box framing `rig`'s neutral pose on `render`, with room for poses that reach out
    bool neutralFrame(editor::PoseRender& render, const pose::PoseRig& rig, AABB& frame, const std::vector<Matrix4>* bridges){
        if (!editor::applyPoseToRender(render, rig, rig.neutralPose(), bridges)) return false;
        AABB bounds;
        if (!render.computeBounds(bounds)) return false;
        const Vector3 min = bounds.getMinimum();
        const Vector3 max = bounds.getMaximum();
        const float height = std::max(max.y - min.y, 1e-3f);
        const float floor = rig.floorHeight();
        const float cx = (min.x + max.x) * 0.5f;
        const float cz = (min.z + max.z) * 0.5f;
        const float half = height * 0.3f;
        frame = AABB(cx - half, floor - height * 0.03f, cz - half, cx + half, floor + height * 1.12f, cz + half);
        return true;
    }

}

editor::PoseThumbnailView editor::poseThumbnailView(const pose::PoseRig& rig){
    // Front looks along -F (at the face); three-quarter = Front turned -40 degrees about U
    const pose::Quatd turn = pose::axisAngleRad(pose::Vec3d(0, 1, 0), pose::degToRad(-40.0));
    const pose::Vec3d canonical = turn * pose::Vec3d(0, 0, -1);
    const pose::Quatd character(rig.characterRotation());
    const pose::Vec3d direction = character.normalized() * canonical;
    const pose::Vec3d up = character.normalized() * pose::Vec3d(0, 1, 0);
    PoseThumbnailView view;
    view.direction = Vector3((float)direction.x, (float)direction.y, (float)direction.z);
    view.up = Vector3((float)up.x, (float)up.y, (float)up.z);
    return view;
}

bool editor::applyPoseToRender(PoseRender& render, const pose::PoseRig& rig, const pose::HumanPose& pose,
                               const std::vector<Matrix4>* bridges){
    if (!render.hasModel() || !rig.valid()) return false;
    const pose::Skeleton& skeleton = rig.skeleton();
    for (size_t i = 0; i < skeleton.bones.size(); i++){
        const int index = render.findBone(skeleton.bones[i].name);
        if (index < 0) continue;
        const pose::SkeletonBone& rest = skeleton.bones[i];
        const int slot = rig.slotOfBone(static_cast<int>(i));
        Vector3 position = slot == pose::boneIndex(pose::HumanBone::Hips) ? pose.hipsPosition : rest.position;
        Quaternion rotation = slot >= 0 ? pose.rotations[slot] : rest.rotation;
        Vector3 scale = rest.scale;
        if (bridges && i < bridges->size() && !((*bridges)[i] == Matrix4())){
            const Matrix4 local = (*bridges)[i].inverse() * Matrix4::translateMatrix(position) * rotation.getRotationMatrix() *
                                  Matrix4::scaleMatrix(scale);
            local.decompose(position, scale, rotation);
        }
        render.setBoneLocal(index, position, rotation, scale);
    }
    return true;
}

void editor::makePoseAnimationIcon(int size, std::vector<uint8_t>& rgba){
    PoseImage image(size, size);
    const float s = static_cast<float>(size) / 64.0f;
    const PoseColor card = PoseColor::hex(0x3A6EA5);
    const PoseColor figure = PoseColor::hex(0xF2F2F2);
    const PoseColor left = PoseColor::hex(0xFF7A45);
    const PoseColor right = PoseColor::hex(0x36B3FF);
    // the card
    image.fillRect(static_cast<int>(10 * s), static_cast<int>(4 * s), static_cast<int>(44 * s), static_cast<int>(56 * s), card);
    image.fillRect(static_cast<int>(10 * s), static_cast<int>(50 * s), static_cast<int>(44 * s), static_cast<int>(10 * s), PoseColor::hex(0x2C5D87));
    // the figure: head, body, arms and legs mid-stride (left limbs coral, right limbs cyan)
    const float w = 3.2f * s;
    image.fillCircle(32 * s, 13 * s, 8 * s, figure);
    image.drawLine(32 * s, 18 * s, 31 * s, 33 * s, w, figure);
    image.drawLine(32 * s, 21 * s, 22 * s, 28 * s, w, right);
    image.drawLine(22 * s, 28 * s, 19 * s, 21 * s, w, right);
    image.drawLine(32 * s, 21 * s, 41 * s, 27 * s, w, left);
    image.drawLine(41 * s, 27 * s, 45 * s, 33 * s, w, left);
    image.drawLine(31 * s, 33 * s, 24 * s, 41 * s, w, right);
    image.drawLine(24 * s, 41 * s, 20 * s, 48 * s, w, right);
    image.drawLine(31 * s, 33 * s, 37 * s, 41 * s, w, left);
    image.drawLine(37 * s, 41 * s, 44 * s, 46 * s, w, left);
    // the timing marks under it
    for (int i = 0; i < 4; i++){
        image.fillRect(static_cast<int>((15 + i * 10) * s), static_cast<int>(54 * s), static_cast<int>(6 * s), static_cast<int>(2 * s), figure);
    }
    rgba = image.getPixels();
}

// ---- PoseThumbnails ----

editor::PoseThumbnails::PoseThumbnails(size_t capacity): capacity(std::max<size_t>(capacity, 2)), alive(std::make_shared<bool>(true)){
}

editor::PoseThumbnails::~PoseThumbnails(){
    *alive = false;
    render.reset();   // pending callbacks run now with ok false and see !alive
    for (auto& entry : entries) entry.second.texture.destroy();
    for (Retired& old : retired) old.texture.destroy();
}

void editor::PoseThumbnails::setModel(const std::string& file, const std::shared_ptr<pose::PoseRig>& newRig,
                                      const std::vector<Matrix4>& newBridges){
    if (file == modelFile && newRig == rig) return;
    clear();
    modelFile = file;
    rig = newRig;
    bridges = newBridges;
}

void editor::PoseThumbnails::clear(){
    generation++;
    if (render){
        render->cancelRenders();
    }
    render.reset();
    inFlight = 0;
    framed = false;
    framingFailed = false;
    for (auto& entry : entries){
        retired.push_back({entry.second.texture, std::move(entry.second.pixels)});
    }
    entries.clear();
    slotKeys.clear();
    modelFile.clear();
    rig.reset();
    bridges.clear();
}

void editor::PoseThumbnails::beginFrame(){
    frameCounter++;
    for (Retired& old : retired) old.texture.destroy();
    retired.clear();
}

uint64_t editor::PoseThumbnails::hashPose(const pose::HumanPose& pose){
    uint64_t hash = FNV_OFFSET;
    for (const Quaternion& q : pose.rotations){
        const float v[4] = {q.w, q.x, q.y, q.z};
        hash = mix(hash, v, sizeof(v));
    }
    const float p[3] = {pose.hipsPosition.x, pose.hipsPosition.y, pose.hipsPosition.z};
    return mix(hash, p, sizeof(p));
}

uint64_t editor::PoseThumbnails::hashKey(const pose::PoseKey& key){
    uint64_t hash = FNV_OFFSET;
    hash = mix(hash, key.body.angles.data(), sizeof(float) * key.body.angles.size());
    const float values[4] = {key.body.offset.x, key.body.offset.z, key.lift, key.grounded ? 1.0f : 0.0f};
    return mix(hash, values, sizeof(values));
}

bool editor::PoseThumbnails::ensureFraming(){
    if (framed) return true;
    if (framingFailed || !rig || modelFile.empty()) return false;
    if (!render){
        render = std::make_unique<PoseRender>();
        std::string error;
        if (!render->setModel(modelFile, &error)){
            framingFailed = true;
            return false;
        }
        PoseRender::Style style;
        style.background = THUMBNAIL_BACKGROUND;
        style.floorVisible = true;
        style.floorHeight = rig->floorHeight();
        style.shaded = true;
        render->setStyle(style);
    }
    AABB frame;
    if (!neutralFrame(*render, *rig, frame, &bridges)){
        return false;   // no graphics view yet: try again next frame
    }
    const PoseThumbnailView view = poseThumbnailView(*rig);
    render->setView(view.direction, view.up, frame, 0.02f);
    const Vector3 min = frame.getMinimum();
    const Vector3 max = frame.getMaximum();
    frameMin[0] = min.x; frameMin[1] = min.y; frameMin[2] = min.z;
    frameMax[0] = max.x; frameMax[1] = max.y; frameMax[2] = max.z;
    framed = true;
    return true;
}

TextureRender* editor::PoseThumbnails::get(uint64_t key, const pose::HumanPose& pose, int width, int height, int slot){
    auto it = entries.find(key);
    if (it != entries.end() && it->second.width == width && it->second.height == height){
        Entry& entry = it->second;
        entry.lastUsedFrame = frameCounter;
        if (entry.ready){
            if (slot >= 0) slotKeys[slot] = key;
            TextureRender* texture = entry.texture.getRender();
            if (texture && texture->isCreated()) return texture;
        }
    }else if (!framingFailed && inFlight < MAX_IN_FLIGHT){
        request(key, pose, width, height);
    }
    // a card whose pose changed shows its previous picture until the new one is ready
    if (slot >= 0){
        auto last = slotKeys.find(slot);
        if (last != slotKeys.end()){
            auto previous = entries.find(last->second);
            if (previous != entries.end() && previous->second.ready){
                previous->second.lastUsedFrame = frameCounter;
                TextureRender* texture = previous->second.texture.getRender();
                if (texture && texture->isCreated()) return texture;
            }
        }
    }
    return nullptr;
}

void editor::PoseThumbnails::request(uint64_t key, const pose::HumanPose& pose, int width, int height){
    if (!ensureFraming()) return;
    Entry& entry = entries[key];
    entry.width = width;
    entry.height = height;
    entry.ready = false;
    entry.lastUsedFrame = frameCounter;
    applyPoseToRender(*render, *rig, pose, &bridges);
    inFlight++;
    const int requestGeneration = generation;
    std::shared_ptr<bool> guard = alive;
    render->requestRender(width, height, [this, guard, requestGeneration, key](bool ok, PoseImage&& image){
        if (!*guard || requestGeneration != generation) return;
        inFlight = std::max(0, inFlight - 1);
        if (!ok){
            auto it = entries.find(key);
            if (it != entries.end()) entries.erase(it);
            return;
        }
        store(key, std::move(image));
    });
    evict();
}

void editor::PoseThumbnails::store(uint64_t key, PoseImage&& image){
    auto it = entries.find(key);
    if (it == entries.end() || image.isEmpty()) return;
    Entry& entry = it->second;
    entry.pixels = image.getPixels();
    entry.width = image.getWidth();
    entry.height = image.getHeight();
    TextureData data(entry.width, entry.height, static_cast<unsigned int>(entry.pixels.size()), ColorFormat::RGBA, 4,
                     entry.pixels.data());
    entry.texture.setData("editor:pose-thumbnail:" + std::to_string(reinterpret_cast<uintptr_t>(this)) + ":" +
                          std::to_string(generation) + ":" + std::to_string(key), data);
    TextureRender* texture = entry.texture.getRender();
    entry.ready = texture && texture->isCreated();
    entry.failed = !entry.ready;
}

void editor::PoseThumbnails::evict(){
    if (entries.size() <= capacity) return;
    std::vector<std::pair<int, uint64_t>> byAge;
    for (const auto& entry : entries){
        if (entry.second.ready) byAge.push_back({entry.second.lastUsedFrame, entry.first});
    }
    std::sort(byAge.begin(), byAge.end());
    size_t excess = entries.size() - capacity;
    for (const auto& old : byAge){
        if (excess == 0) break;
        if (old.first >= frameCounter - 1) break;   // still on screen
        auto it = entries.find(old.second);
        retired.push_back({it->second.texture, std::move(it->second.pixels)});
        entries.erase(it);
        for (auto slot = slotKeys.begin(); slot != slotKeys.end();){
            slot = slot->second == old.second ? slotKeys.erase(slot) : std::next(slot);
        }
        excess--;
    }
}

// ---- PoseAssetThumbnailer ----

editor::PoseAssetThumbnailer::PoseAssetThumbnailer(): alive(std::make_shared<bool>(true)){
}

editor::PoseAssetThumbnailer::~PoseAssetThumbnailer(){
    *alive = false;
    render.reset();
}

void editor::PoseAssetThumbnailer::request(Project* project, const fs::path& asset, const fs::path& thumbnailPath,
                                           std::function<void(bool)> done){
    for (const Job& job : queue){
        if (job.asset == asset) return;
    }
    if (active && current.asset == asset) return;
    queue.push_back({project, asset, thumbnailPath, std::move(done)});
}

void editor::PoseAssetThumbnailer::cancel(){
    std::deque<Job> jobs;
    jobs.swap(queue);
    if (render) render->cancelRenders();
    active = false;
    for (Job& job : jobs){
        if (job.done) job.done(false);
    }
}

void editor::PoseAssetThumbnailer::process(){
    if (active || queue.empty()) return;
    Job job = std::move(queue.front());
    queue.pop_front();
    start(std::move(job));
}

void editor::PoseAssetThumbnailer::finish(bool ok){
    active = false;
    Job job = std::move(current);
    current = Job();
    if (job.done) job.done(ok);
}

void editor::PoseAssetThumbnailer::start(Job job){
    current = std::move(job);
    active = true;
    std::string text;
    pose::PoseAnimationData data;
    std::vector<std::string> warnings;
    if (!ProjectChecks::readFile(current.asset, text) || !pose::parsePoseAnimation(text, data, warnings, nullptr) ||
        data.poses.empty() || data.previewModel.empty() || !current.project){
        finish(false);
        return;
    }
    const fs::path model = current.project->resolveAssetPath(data.previewModel);
    std::shared_ptr<const PoseRigInfo> info = PoseRigProvider::forModelFile(current.project, model);
    if (!info || !info->rig){
        finish(false);
        return;
    }
    const std::shared_ptr<pose::PoseRig> rig = info->rig;
    const std::vector<pose::HumanPose> keys = pose::realiseKeys(data, *rig, rig->skin());
    if (keys.empty()){
        finish(false);
        return;
    }
    if (!render || render->getModelPath() != model.string()){
        render = std::make_unique<PoseRender>();
        if (!render->setModel(model.string(), nullptr)){
            render.reset();
            finish(false);
            return;
        }
    }
    PoseRender::Style style;
    style.background = Vector4(0.16f, 0.16f, 0.17f, 1.0f);
    style.floorHeight = rig->floorHeight();
    render->setStyle(style);
    AABB frame;
    if (!neutralFrame(*render, *rig, frame, nullptr)){
        finish(false);
        return;
    }
    const PoseThumbnailView view = poseThumbnailView(*rig);
    render->setView(view.direction, view.up, frame, 0.02f);
    applyPoseToRender(*render, *rig, keys[keys.size() / 2]);
    std::shared_ptr<bool> guard = alive;
    const fs::path target = current.thumbnail;
    render->requestRender(SIZE, SIZE, [this, guard, target](bool ok, PoseImage&& image){
        if (!*guard) return;
        bool written = false;
        if (ok && !image.isEmpty()){
            // through FileUtils (scope-aware: beneath the project in a write scope, no link followed);
            // the thumbnail cache is the editor's own .jasperengine/thumbs, so the writer names the
            // one file it writes (and its folder only to make it when missing: util/SafeWrite.h)
            std::vector<uint8_t> png;
            if (image.encodePng(png)){
                std::error_code ec;
                if (!fs::is_directory(target.parent_path(), ec)){
                    safewrite::EditorInternal makeThumbs(target.parent_path());
                    FileUtils::createDirectories(target.parent_path(), ec);
                }
                safewrite::EditorInternal ownThumb(target);
                written = FileUtils::writeFileReplacing(target, std::string(png.begin(), png.end()));
            }
        }
        finish(written);
    });
}
