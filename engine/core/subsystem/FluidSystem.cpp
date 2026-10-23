// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract stub: the fluid-sim branch replaces these bodies (and registers the system in Scene).

#include "FluidSystem.h"

using namespace jasperengine;

FluidSystem::FluidSystem(Scene* scene): SubSystem(scene) {}
FluidSystem::~FluidSystem() {}
void FluidSystem::load() {}
void FluidSystem::draw() {}
void FluidSystem::destroy() {}
void FluidSystem::update(double dt) { (void)dt; }
void FluidSystem::onComponentAdded(Entity entity, ComponentId componentId) { (void)entity; (void)componentId; }
void FluidSystem::onComponentRemoved(Entity entity, ComponentId componentId) { (void)entity; (void)componentId; }
void FluidSystem::setPreview(Entity entity, bool preview) { (void)entity; (void)preview; }
bool FluidSystem::isPreview(Entity entity) const { (void)entity; return false; }
void FluidSystem::renderFluids() {}
