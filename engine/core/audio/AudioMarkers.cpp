// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AudioMarkers.h"

#if JASPERENGINE_AUDIO_CAPTURE

#include "AudioCapture.h"

using namespace jasperengine;

std::atomic<bool> AudioMarkers::enabled{false};

namespace {

    uint64_t markerCount = 0;

}

void AudioMarkers::setEnabled(bool on) {
    if (on) markerCount = 0;
    enabled.store(on, std::memory_order_relaxed);
}

uint64_t AudioMarkers::getCount() {
    return markerCount;
}

void AudioMarkers::collision(const Scene* scene, const char* kind, uint32_t entity, uint32_t other, float speed) {
    markerCount++;
    const std::string first = AudioCapture::entityPath(scene, entity);
    const std::string second = AudioCapture::entityPath(scene, other);
    AudioCapture::Line line(kind);
    line.text("label", std::string(kind) + " " + first + " ~ " + second);
    line.scene(scene).entity(scene, entity).entity(scene, other, "other", "other_id");
    if (speed >= 0.0f) line.number("speed", speed, 4);
}

void AudioMarkers::sensor(const Scene* scene, const char* kind, uint32_t entity, uint32_t other) {
    collision(scene, kind, entity, other, -1.0f);
}

void AudioMarkers::ui(const Scene* scene, const char* kind, uint32_t entity, float x, float y) {
    markerCount++;
    AudioCapture::Line line(kind);
    line.text("label", std::string(kind) + " " + AudioCapture::entityPath(scene, entity));
    line.scene(scene).entity(scene, entity).number("x", x, 2).number("y", y, 2);
}

void AudioMarkers::htmlClick(const Scene* scene, std::string_view element) {
    markerCount++;
    AudioCapture::Line line("html_click");
    line.text("label", "html_click " + std::string(element));
    line.scene(scene).text("element", element);
}

void AudioMarkers::animationEvent(const Scene* scene, uint32_t entity, std::string_view function, std::string_view state, double time) {
    markerCount++;
    AudioCapture::Line line("animation_event");
    line.text("label", "animation_event " + std::string(function));
    line.scene(scene).entity(scene, entity).text("function", function).text("state", state).number("time", time, 6);
}

void AudioMarkers::spawn(const Scene* scene, uint32_t root, std::string_view prefabPath, std::string_view guid) {
    markerCount++;
    const std::string path = AudioCapture::entityPath(scene, root);
    AudioCapture::Line line("spawn");
    line.text("label", "spawn " + path);
    line.scene(scene).entity(scene, root);
    if (!prefabPath.empty()) line.text("prefab", prefabPath);
    if (!guid.empty()) line.text("guid", guid);
}

void AudioMarkers::destroy(const Scene* scene, uint32_t root) {
    markerCount++;
    const std::string path = AudioCapture::entityPath(scene, root);
    AudioCapture::Line line("destroy");
    line.text("label", "destroy " + path);
    line.scene(scene).entity(scene, root);
}

void AudioMarkers::sceneLoad(std::string_view name, const char* mode) {
    markerCount++;
    AudioCapture::Line line("scene_load");
    line.text("label", "scene_load " + std::string(name));
    line.text("scene", name).text("mode", mode);
}

void AudioMarkers::input(const char* device, const char* action, int code, float x, float y) {
    AudioCapture::Line line("input");
    line.text("device", device).text("action", action).integer("code", code).number("x", x, 2).number("y", y, 2);
}

#endif
