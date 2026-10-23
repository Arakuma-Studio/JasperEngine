// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract stub: the fluid-sim branch replaces these bodies with the GPU implementation.

#include "FluidSimulator.h"

using namespace jasperengine;

struct FluidSimulator::Impl {
    Framebuffer output;
    Texture outputTexture;
    FluidStats stats;
    std::string error;
};

FluidSimulator::FluidSimulator(): impl(new Impl()) {}
FluidSimulator::~FluidSimulator() { delete impl; }

bool FluidSimulator::isSupported(std::string* reason) {
    if (reason) *reason = "not implemented";
    return false;
}

bool FluidSimulator::update(FluidSimulationComponent& component, float aspectRatio, double dt) {
    (void)component; (void)aspectRatio; (void)dt;
    return false;
}

void FluidSimulator::reset(const FluidSimulationComponent& component) { (void)component; }
void FluidSimulator::step(FluidSimulationComponent& component, float aspectRatio, int steps) { (void)component; (void)aspectRatio; (void)steps; }
void FluidSimulator::pointerDown(int id, Vector2 position) { (void)id; (void)position; }
void FluidSimulator::pointerMove(int id, Vector2 position) { (void)id; (void)position; }
void FluidSimulator::pointerUp(int id) { (void)id; }
void FluidSimulator::pointerCancelAll() {}
Framebuffer* FluidSimulator::getOutputFramebuffer() { return &impl->output; }
Texture& FluidSimulator::getOutputTexture() { return impl->outputTexture; }
Framebuffer* FluidSimulator::getDyeFramebuffer() { return nullptr; }
Framebuffer* FluidSimulator::getVelocityFramebuffer() { return nullptr; }
const FluidStats& FluidSimulator::getStats() const { return impl->stats; }
const std::string& FluidSimulator::getError() const { return impl->error; }
void FluidSimulator::release() {}
