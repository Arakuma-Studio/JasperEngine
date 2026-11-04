// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Lattice.h"
#include "component/Transform.h"

#include "subsystem/LatticeSystem.h"
#include "lattice/LatticeSolver.h"
#include "lattice/LatticePresets.h"
#include "component/MeshComponent.h"
#include "Log.h"

#include <algorithm>

using namespace jasperengine;

namespace {

    bool axisFromName(const std::string& name, lattice::LatticeAxis& axis){
        if (name == "x" || name == "X"){
            axis = lattice::LatticeAxis::X;
        }else if (name == "y" || name == "Y"){
            axis = lattice::LatticeAxis::Y;
        }else if (name == "z" || name == "Z"){
            axis = lattice::LatticeAxis::Z;
        }else{
            return false;
        }
        return true;
    }

}

Lattice::Lattice(Scene* scene): GameObject(scene){
    addComponent<LatticeComponent>();
}

Lattice::Lattice(Scene* scene, Entity entity): GameObject(scene, entity){
}

Lattice::~Lattice(){
}

Lattice::Lattice(const Lattice& rhs) = default;
Lattice& Lattice::operator=(const Lattice& rhs) = default;
Lattice::Lattice(Lattice&& rhs) noexcept = default;
Lattice& Lattice::operator=(Lattice&& rhs) noexcept = default;

int Lattice::checkedIndex(int i, int j, int k, const char* function) const{
    int rx, ry, rz;
    LatticeSystem::getResolution(getComponent<LatticeComponent>(), rx, ry, rz);
    if (i < 0 || j < 0 || k < 0 || i >= rx || j >= ry || k >= rz){
        Log::error("Lattice:%s: point (%i, %i, %i) is outside the resolution %i x %i x %i", function, i, j, k, rx, ry, rz);
        return -1;
    }
    return lattice::pointIndex(rx, ry, i, j, k);
}

int Lattice::checkedIndex(int index, const char* function) const{
    int rx, ry, rz;
    LatticeSystem::getResolution(getComponent<LatticeComponent>(), rx, ry, rz);
    if (index < 0 || index >= rx * ry * rz){
        Log::error("Lattice:%s: index %i is outside 0..%i", function, index, rx * ry * rz - 1);
        return -1;
    }
    return index;
}

LatticeComponent& Lattice::getLatticeComponent() const{
    return getComponent<LatticeComponent>();
}

void Lattice::setEnabled(bool enabled){
    LatticeComponent& lattice = getComponent<LatticeComponent>();
    if (lattice.enabled != enabled){
        lattice.enabled = enabled;
        lattice.revision++;
    }
}

bool Lattice::isEnabled() const{
    return getComponent<LatticeComponent>().enabled;
}

void Lattice::setResolution(int x, int y, int z){
    LatticeComponent& lattice = getComponent<LatticeComponent>();
    if (x < LATTICE_MIN_RESOLUTION || y < LATTICE_MIN_RESOLUTION || z < LATTICE_MIN_RESOLUTION ||
            x > LATTICE_MAX_RESOLUTION || y > LATTICE_MAX_RESOLUTION || z > LATTICE_MAX_RESOLUTION){
        Log::warn("Lattice:setResolution: %i x %i x %i clamped to %i..%i per axis", x, y, z, LATTICE_MIN_RESOLUTION, LATTICE_MAX_RESOLUTION);
    }
    LatticeSystem::setResolution(lattice, x, y, z);
}

Vector3 Lattice::getResolution() const{
    int rx, ry, rz;
    LatticeSystem::getResolution(getComponent<LatticeComponent>(), rx, ry, rz);
    return Vector3((float)rx, (float)ry, (float)rz);
}

int Lattice::getResolutionX() const{
    return (int)getResolution().x;
}

int Lattice::getResolutionY() const{
    return (int)getResolution().y;
}

int Lattice::getResolutionZ() const{
    return (int)getResolution().z;
}

int Lattice::getPointCount() const{
    int rx, ry, rz;
    LatticeSystem::getResolution(getComponent<LatticeComponent>(), rx, ry, rz);
    return rx * ry * rz;
}

int Lattice::getPointIndex(int i, int j, int k) const{
    return checkedIndex(i, j, k, "getPointIndex");
}

Vector3 Lattice::getOffset(int i, int j, int k) const{
    const int index = checkedIndex(i, j, k, "getOffset");
    if (index < 0){
        return Vector3(0, 0, 0);
    }
    const LatticeComponent& lattice = getComponent<LatticeComponent>();
    return (size_t)index < lattice.offsets.size() ? lattice.offsets[index] : Vector3(0, 0, 0);
}

void Lattice::setOffset(int i, int j, int k, Vector3 offset){
    const int index = checkedIndex(i, j, k, "setOffset");
    if (index >= 0){
        setOffsetAt(index, offset);
    }
}

Vector3 Lattice::getOffsetAt(int index) const{
    if (checkedIndex(index, "getOffsetAt") < 0){
        return Vector3(0, 0, 0);
    }
    const LatticeComponent& lattice = getComponent<LatticeComponent>();
    return (size_t)index < lattice.offsets.size() ? lattice.offsets[index] : Vector3(0, 0, 0);
}

void Lattice::setOffsetAt(int index, Vector3 offset){
    if (checkedIndex(index, "setOffsetAt") < 0){
        return;
    }
    LatticeComponent& lattice = getComponent<LatticeComponent>();
    // a list of another size (data saved inconsistently) is first fitted to the resolution
    const size_t count = (size_t)getPointCount();
    if (lattice.offsets.size() != count){
        lattice.offsets.resize(count, Vector3(0, 0, 0));
    }
    LatticeSystem::stopTween(lattice);
    lattice.offsets[index] = offset;
    lattice.revision++;
}

Vector3 Lattice::getRestPosition(int i, int j, int k) const{
    if (checkedIndex(i, j, k, "getRestPosition") < 0){
        return Vector3(0, 0, 0);
    }
    int rx, ry, rz;
    LatticeSystem::getResolution(getComponent<LatticeComponent>(), rx, ry, rz);
    return lattice::restPosition(rx, ry, rz, i, j, k);
}

Vector3 Lattice::getPointPosition(int i, int j, int k) const{
    if (checkedIndex(i, j, k, "getPointPosition") < 0){
        return Vector3(0, 0, 0);
    }
    return getRestPosition(i, j, k) + getOffset(i, j, k);
}

void Lattice::setPointPosition(int i, int j, int k, Vector3 position){
    const int index = checkedIndex(i, j, k, "setPointPosition");
    if (index >= 0){
        setOffsetAt(index, position - getRestPosition(i, j, k));
    }
}

Vector3 Lattice::getPointWorldPosition(int i, int j, int k) const{
    if (checkedIndex(i, j, k, "getPointWorldPosition") < 0){
        return Vector3(0, 0, 0);
    }
    return getModelMatrix() * getPointPosition(i, j, k);
}

void Lattice::setPointWorldPosition(int i, int j, int k, Vector3 position){
    const int index = checkedIndex(i, j, k, "setPointWorldPosition");
    if (index < 0){
        return;
    }
    const Matrix4 model = getModelMatrix();
    if (std::fabs(model.linear().determinant()) <= 1e-8f){
        Log::error("Lattice:setPointWorldPosition: the lattice has a zero scale");
        return;
    }
    setOffsetAt(index, model.inverse() * position - getRestPosition(i, j, k));
}

void Lattice::resetOffsets(){
    LatticeComponent& lattice = getComponent<LatticeComponent>();
    LatticeSystem::stopTween(lattice);
    lattice.offsets.assign((size_t)getPointCount(), Vector3(0, 0, 0));
    lattice.revision++;
}

void Lattice::setInterpolation(LatticeInterpolation interpolation){
    LatticeComponent& lattice = getComponent<LatticeComponent>();
    if (lattice.interpolation != interpolation){
        lattice.interpolation = interpolation;
        lattice.revision++;
    }
}

LatticeInterpolation Lattice::getInterpolation() const{
    return getComponent<LatticeComponent>().interpolation;
}

void Lattice::setOutside(LatticeOutside outside){
    LatticeComponent& lattice = getComponent<LatticeComponent>();
    if (lattice.outside != outside){
        lattice.outside = outside;
        lattice.revision++;
    }
}

LatticeOutside Lattice::getOutside() const{
    return getComponent<LatticeComponent>().outside;
}

void Lattice::setFadeDistance(float fadeDistance){
    LatticeComponent& lattice = getComponent<LatticeComponent>();
    if (lattice.fadeDistance != fadeDistance){
        lattice.fadeDistance = fadeDistance;
        lattice.revision++;
    }
}

float Lattice::getFadeDistance() const{
    return getComponent<LatticeComponent>().fadeDistance;
}

bool Lattice::applyPreset(const std::string& name, const std::string& axis, float amount, const std::string& toward){
    lattice::PresetParams params;
    if (!lattice::presetFromName(name, params.preset)){
        std::string names;
        for (const std::string& preset : lattice::presetNames()){
            names += (names.empty() ? "" : ", ") + preset;
        }
        Log::error("Lattice:applyPreset: unknown preset '%s' (one of: %s)", name.c_str(), names.c_str());
        return false;
    }
    if (!axisFromName(axis, params.axis)){
        Log::error("Lattice:applyPreset: axis '%s' is not x, y or z", axis.c_str());
        return false;
    }
    // toward "-x": the other way (bend and shear: the amount negated)
    const bool negative = toward.size() == 2 && toward[0] == '-';
    const bool signedAxis = toward.size() == 2 && (toward[0] == '-' || toward[0] == '+');
    if (toward.empty()){
        params.toward = (lattice::LatticeAxis)(((int)params.axis + 1) % 3);
    }else if (!axisFromName(signedAxis ? toward.substr(1) : toward, params.toward)){
        Log::error("Lattice:applyPreset: toward '%s' is not x, y or z (or -x, -y, -z)", toward.c_str());
        return false;
    }
    const bool directed = params.preset == lattice::LatticePreset::BEND || params.preset == lattice::LatticePreset::SHEAR;
    params.amount = (negative && directed) ? -amount : amount;

    LatticeComponent& lattice = getComponent<LatticeComponent>();
    int rx, ry, rz;
    LatticeSystem::getResolution(lattice, rx, ry, rz);
    lattice.offsets.resize((size_t)(rx * ry * rz), Vector3(0, 0, 0));
    LatticeSystem::stopTween(lattice);
    // bend and twist in the cage's proportions: true angles in the world
    params.proportions = lattice::presetProportions(getComponent<Transform>().modelMatrix);
    lattice::applyPreset(rx, ry, rz, lattice.offsets, params);
    lattice.revision++;
    return true;
}

void Lattice::savePose(const std::string& name){
    LatticeSystem::savePose(getComponent<LatticeComponent>(), name);
}

bool Lattice::hasPose(const std::string& name) const{
    return LatticeSystem::findPose(getComponent<LatticeComponent>(), name) != nullptr;
}

bool Lattice::removePose(const std::string& name){
    LatticeComponent& lattice = getComponent<LatticeComponent>();
    auto it = std::find_if(lattice.poses.begin(), lattice.poses.end(), [&](const LatticePose& pose){ return pose.name == name; });
    if (it == lattice.poses.end()){
        return false;
    }
    lattice.poses.erase(it);
    lattice.revision++;
    return true;
}

bool Lattice::applyPose(const std::string& name, float weight){
    if (!LatticeSystem::applyPose(getComponent<LatticeComponent>(), name, weight)){
        Log::error("Lattice:applyPose: no pose named '%s'", name.c_str());
        return false;
    }
    return true;
}

bool Lattice::tweenToPose(const std::string& name, float duration, EaseType ease){
    LatticeComponent& lattice = getComponent<LatticeComponent>();
    const LatticePose* pose = LatticeSystem::findPose(lattice, name);
    if (!pose){
        Log::error("Lattice:tweenToPose: no pose named '%s'", name.c_str());
        return false;
    }
    LatticeSystem::startTween(lattice, LatticeSystem::getPoseOffsets(lattice, *pose), duration, ease);
    return true;
}

void Lattice::tweenToRest(float duration, EaseType ease){
    LatticeSystem::startTween(getComponent<LatticeComponent>(), std::vector<Vector3>((size_t)getPointCount(), Vector3(0, 0, 0)), duration, ease);
}

bool Lattice::isTweening() const{
    return getComponent<LatticeComponent>().tweening;
}

void Lattice::stopTween(){
    LatticeSystem::stopTween(getComponent<LatticeComponent>());
}

void Lattice::setJiggle(bool jiggle){
    LatticeComponent& lattice = getComponent<LatticeComponent>();
    if (lattice.jiggle != jiggle){
        lattice.jiggle = jiggle;
        LatticeSystem::resetJiggle(lattice);
        lattice.revision++;
    }
}

bool Lattice::isJiggle() const{
    return getComponent<LatticeComponent>().jiggle;
}

void Lattice::setJiggleStiffness(float stiffness){
    LatticeComponent& lattice = getComponent<LatticeComponent>();
    lattice.jiggleStiffness = stiffness;
    lattice.revision++;
}

float Lattice::getJiggleStiffness() const{
    return getComponent<LatticeComponent>().jiggleStiffness;
}

void Lattice::setJiggleDamping(float damping){
    LatticeComponent& lattice = getComponent<LatticeComponent>();
    lattice.jiggleDamping = damping;
    lattice.revision++;
}

float Lattice::getJiggleDamping() const{
    return getComponent<LatticeComponent>().jiggleDamping;
}

void Lattice::setJiggleInertia(float inertia){
    LatticeComponent& lattice = getComponent<LatticeComponent>();
    lattice.jiggleInertia = inertia;
    lattice.revision++;
}

float Lattice::getJiggleInertia() const{
    return getComponent<LatticeComponent>().jiggleInertia;
}

void Lattice::setJiggleAnchor(LatticeJiggleAnchor anchor){
    LatticeComponent& lattice = getComponent<LatticeComponent>();
    lattice.jiggleAnchor = anchor;
    lattice.revision++;
}

LatticeJiggleAnchor Lattice::getJiggleAnchor() const{
    return getComponent<LatticeComponent>().jiggleAnchor;
}

void Lattice::setJiggleMaxOffset(float maxOffset){
    LatticeComponent& lattice = getComponent<LatticeComponent>();
    lattice.jiggleMaxOffset = maxOffset;
    lattice.revision++;
}

float Lattice::getJiggleMaxOffset() const{
    return getComponent<LatticeComponent>().jiggleMaxOffset;
}

bool Lattice::isJiggleSleeping() const{
    return getComponent<LatticeComponent>().jiggleSleeping;
}

Vector3 Lattice::getJiggleOffset(int i, int j, int k) const{
    const int index = checkedIndex(i, j, k, "getJiggleOffset");
    const LatticeComponent& lattice = getComponent<LatticeComponent>();
    if (index < 0 || (size_t)index >= lattice.dynamicOffsets.size()){
        return Vector3(0, 0, 0);
    }
    return lattice.dynamicOffsets[index];
}

void Lattice::resetJiggle(){
    LatticeSystem::resetJiggle(getComponent<LatticeComponent>());
}

void Lattice::poke(int i, int j, int k, Vector3 impulse){
    const int index = checkedIndex(i, j, k, "poke");
    if (index >= 0){
        LatticeSystem::poke(getComponent<LatticeComponent>(), index, impulse);
    }
}

void Lattice::pokeAt(Vector3 worldPoint, Vector3 impulse, float radius){
    LatticeSystem::pokeAt(getComponent<LatticeComponent>(), getModelMatrix(), worldPoint, impulse, radius);
}

Vector3 Lattice::deformPoint(Vector3 worldPoint) const{
    return scene->getSystem<LatticeSystem>()->deformWorldPoint(entity, worldPoint);
}

bool Lattice::bind(Entity target, float weight){
    if (target == NULL_ENTITY || !scene->isEntityCreated(target)){
        Log::error("Lattice:bind: entity %u is not in this scene", target);
        return false;
    }
    if (target == entity){
        Log::error("Lattice:bind: a lattice cannot deform itself");
        return false;
    }
    weight = std::clamp(weight, 0.0f, 2.0f);

    LatticeModifierComponent* modifier = scene->findComponent<LatticeModifierComponent>(target);
    if (!modifier){
        LatticeModifierComponent component;
        // an entity without a mesh of its own is a group: its descendants' meshes follow
        component.includeChildren = scene->findComponent<MeshComponent>(target) == nullptr;
        scene->addComponent<LatticeModifierComponent>(target, component);
        modifier = scene->findComponent<LatticeModifierComponent>(target);
    }

    for (LatticeBinding& binding : modifier->bindings){
        if (binding.lattice == entity){
            binding.weight = weight;
            return true;
        }
    }
    LatticeBinding binding;
    binding.lattice = entity;
    binding.weight = weight;
    modifier->bindings.push_back(binding);
    return true;
}

bool Lattice::bind(EntityHandle* target, float weight){
    if (!target || target->getScene() != scene){
        Log::error("Lattice:bind: the target is not in this scene");
        return false;
    }
    return bind(target->getEntity(), weight);
}

bool Lattice::unbind(Entity target){
    if (target == NULL_ENTITY || !scene->isEntityCreated(target)){
        return false;
    }
    LatticeModifierComponent* modifier = scene->findComponent<LatticeModifierComponent>(target);
    if (!modifier){
        return false;
    }
    const size_t before = modifier->bindings.size();
    modifier->bindings.erase(std::remove_if(modifier->bindings.begin(), modifier->bindings.end(),
        [this](const LatticeBinding& binding){ return binding.lattice == entity; }), modifier->bindings.end());
    return modifier->bindings.size() != before;
}

bool Lattice::unbind(EntityHandle* target){
    if (!target || target->getScene() != scene){
        return false;
    }
    return unbind(target->getEntity());
}

bool Lattice::addFollower(Entity follower, Entity rest, bool applyPosition, bool applyRotation, bool applyScale, float weight){
    if (follower == NULL_ENTITY || rest == NULL_ENTITY || !scene->isEntityCreated(follower) || !scene->isEntityCreated(rest)){
        Log::error("Lattice:addFollower: the follower and its rest must be entities of this scene");
        return false;
    }
    if (!scene->findComponent<Transform>(follower) || !scene->findComponent<Transform>(rest)){
        Log::error("Lattice:addFollower: the follower and its rest need a Transform");
        return false;
    }
    if (!scene->getSystem<LatticeSystem>()->isValidFollower(follower, entity, rest)){
        Log::error("Lattice:addFollower: a follower cannot be its own rest, nor an ancestor of the rest or of the lattice");
        return false;
    }

    LatticeFollowerComponent component;
    component.lattice = entity;
    component.rest = rest;
    component.applyPosition = applyPosition;
    component.applyRotation = applyRotation;
    component.applyScale = applyScale;
    component.weight = weight;

    if (LatticeFollowerComponent* existing = scene->findComponent<LatticeFollowerComponent>(follower)){
        component.enabled = existing->enabled;
        *existing = component;
    }else{
        scene->addComponent<LatticeFollowerComponent>(follower, component);
    }
    return true;
}

bool Lattice::addFollower(EntityHandle* follower, EntityHandle* rest, bool applyPosition, bool applyRotation, bool applyScale, float weight){
    if (!follower || !rest || follower->getScene() != scene || rest->getScene() != scene){
        Log::error("Lattice:addFollower: the follower and its rest must be GameObjects of this scene");
        return false;
    }
    return addFollower(follower->getEntity(), rest->getEntity(), applyPosition, applyRotation, applyScale, weight);
}

bool Lattice::removeFollower(Entity follower){
    if (follower == NULL_ENTITY || !scene->isEntityCreated(follower)){
        return false;
    }
    LatticeFollowerComponent* component = scene->findComponent<LatticeFollowerComponent>(follower);
    if (!component || component->lattice != entity){
        return false;
    }
    scene->removeComponent<LatticeFollowerComponent>(follower);
    return true;
}

bool Lattice::removeFollower(EntityHandle* follower){
    if (!follower || follower->getScene() != scene){
        return false;
    }
    return removeFollower(follower->getEntity());
}

LatticeModifierComponent* Lattice::getModifierComponent(Entity target) const{
    if (target == NULL_ENTITY || !scene->isEntityCreated(target)){
        return nullptr;
    }
    return scene->findComponent<LatticeModifierComponent>(target);
}

LatticeFollowerComponent* Lattice::getFollowerComponent(Entity follower) const{
    if (follower == NULL_ENTITY || !scene->isEntityCreated(follower)){
        return nullptr;
    }
    LatticeFollowerComponent* component = scene->findComponent<LatticeFollowerComponent>(follower);
    return (component && component->lattice == entity) ? component : nullptr;
}
