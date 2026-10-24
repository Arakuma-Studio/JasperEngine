// SPDX-License-Identifier: MIT
// STUB (contract only): replaced by the real implementation.

#include "PlacementSession.h"

#include <cstring>

using namespace jasperengine;
using namespace jasperengine::editor;

std::vector<Entity> editor::placementRoots(Project*, size_t, const std::vector<Entity>& selection) { return selection; }
ConvexDecompositionCache* editor::placementDecompositionCache(Project*) { return nullptr; }
void editor::placementProjectSwitching() {}

struct PlacementSession::Impl {
    Project* project = nullptr;
    size_t sceneId = 0;
    PhysicsPlacementSettings settings;
};

PlacementSession::PlacementSession(Project* project, size_t sceneId): impl(new Impl) { impl->project = project; impl->sceneId = sceneId; }
PlacementSession::~PlacementSession() = default;
size_t PlacementSession::getSceneId() const { return impl->sceneId; }
void PlacementSession::setSettings(const PhysicsPlacementSettings& s) { impl->settings = s; }
const PhysicsPlacementSettings& PlacementSession::getSettings() const { return impl->settings; }
bool PlacementSession::begin(PlacementGesture, const std::vector<Entity>&) { return false; }
bool PlacementSession::isGestureActive() const { return false; }
bool PlacementSession::isBusy() const { return false; }
PlacementGesture PlacementSession::getGesture() const { return PlacementGesture::None; }
PlacementPhase PlacementSession::getPhase() const { return PlacementPhase::Idle; }
void PlacementSession::setMoveTarget(const Vector3&) {}
void PlacementSession::setRotateDelta(const Quaternion&) {}
void PlacementSession::setGatherScale(const Vector3&) {}
void PlacementSession::setSnapRay(const Ray&) {}
void PlacementSession::setPointerRay(const Ray&) {}
void PlacementSession::end() {}
void PlacementSession::cancel() {}
bool PlacementSession::distribute(const std::vector<Entity>&, int) { return false; }
bool PlacementSession::gather(const std::vector<Entity>&) { return false; }
void PlacementSession::tick(double) {}
void PlacementSession::notifySceneChanged() {}
void PlacementSession::setSelection(const std::vector<Entity>&) {}
PlacementHud PlacementSession::getHud() const { PlacementHud h; h.status = "Ready"; return h; }
void PlacementSession::collectOverlay(PlacementOverlay& out) const { out = PlacementOverlay(); }
Vector3 PlacementSession::getMoversCenter() const { return Vector3(); }

const char* editor::placementModeName(PlacementMode m) {
    switch (m) {
        case PlacementMode::Drop: return "drop"; case PlacementMode::Separate: return "separate"; case PlacementMode::Move: return "move";
        case PlacementMode::Snap: return "snap"; case PlacementMode::Gather: return "gather"; case PlacementMode::Distribute: return "distribute";
    }
    return "drop";
}
bool editor::parsePlacementMode(const char* n, PlacementMode& out) {
    if (!n) return false;
    const PlacementMode all[] = {PlacementMode::Drop, PlacementMode::Separate, PlacementMode::Move, PlacementMode::Snap, PlacementMode::Gather, PlacementMode::Distribute};
    for (PlacementMode m : all) if (!std::strcmp(n, placementModeName(m))) { out = m; return true; }
    return false;
}
std::string PlacementSpec::fingerprint() const { return std::string(); }
std::vector<std::string> editor::checkPlacementSpec(Project*, const PlacementSpec&) { return {"physics placement is not implemented yet"}; }

struct PlacementRun::Impl { PlacementResult result; bool done = false; };
PlacementRun::PlacementRun(): impl(new Impl) {}
PlacementRun::~PlacementRun() = default;
bool PlacementRun::start(Project*, const PlacementSpec&) { impl->result.error = "physics placement is not implemented yet"; impl->done = true; return false; }
bool PlacementRun::advance(double) { return true; }
bool PlacementRun::isDone() const { return impl->done; }
void PlacementRun::cancel() { impl->result.cancelled = true; impl->done = true; }
float PlacementRun::progress() const { return 1.0f; }
std::string PlacementRun::progressText() const { return std::string(); }
const PlacementResult& PlacementRun::result() const { return impl->result; }
bool editor::commitPlacement(Project*, const PlacementResult&, std::string& error) { error = "physics placement is not implemented yet"; return false; }
nlohmann::json editor::placementResultJson(Project*, const PlacementResult&, bool) { return nlohmann::json::object(); }
