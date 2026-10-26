// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SceneViewState.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    const char* const OWN_KEYS[] = {"view", "cameraSettings", "bookmarks", "measures", "layerFilter", "hiddenEntities", "pickLockedEntities"};

    bool finite(float value){
        return std::isfinite(value);
    }

    float readFloat(const YAML::Node& node, float fallback){
        if (!node || !node.IsScalar()){
            return fallback;
        }
        try {
            const float value = node.as<float>();
            return finite(value) ? value : fallback;
        } catch (const std::exception&){
            return fallback;
        }
    }

    bool readBool(const YAML::Node& node, bool fallback){
        if (!node || !node.IsScalar()){
            return fallback;
        }
        try {
            return node.as<bool>();
        } catch (const std::exception&){
            return fallback;
        }
    }

    uint32_t readMask(const YAML::Node& node, uint32_t fallback){
        if (!node || !node.IsScalar()){
            return fallback;
        }
        try {
            // a mask written as a signed int (Unity's -1 for Everything) or unsigned
            return (uint32_t)node.as<int64_t>();
        } catch (const std::exception&){
            return fallback;
        }
    }

    YAML::Node encodeVector(const Vector3& v){
        YAML::Node node(YAML::NodeType::Sequence);
        node.SetStyle(YAML::EmitterStyle::Flow);
        node.push_back(v.x);
        node.push_back(v.y);
        node.push_back(v.z);
        return node;
    }

    Vector3 decodeVector(const YAML::Node& node, const Vector3& fallback){
        if (!node || !node.IsSequence() || node.size() != 3){
            return fallback;
        }
        const Vector3 v(readFloat(node[0], NAN), readFloat(node[1], NAN), readFloat(node[2], NAN));
        return (finite(v.x) && finite(v.y) && finite(v.z)) ? v : fallback;
    }

    void decodeEntities(const YAML::Node& node, std::set<Entity>& entities){
        entities.clear();
        if (!node || !node.IsSequence()){
            return;
        }
        for (const auto& item : node){
            try {
                const Entity entity = item.as<Entity>();
                if (entity != NULL_ENTITY){
                    entities.insert(entity);
                }
            } catch (const std::exception&){
            }
        }
    }

    YAML::Node encodeEntities(const std::set<Entity>& entities){
        YAML::Node node(YAML::NodeType::Sequence);
        node.SetStyle(YAML::EmitterStyle::Flow);
        for (Entity entity : entities){
            node.push_back(entity);
        }
        return node;
    }

}

void SceneCameraSettings::clamp(){
    fieldOfView = std::clamp(finite(fieldOfView) ? fieldOfView : 60.0f, MIN_FOV, MAX_FOV);
    nearClip = std::clamp(finite(nearClip) ? nearClip : 0.3f, 0.0001f, 100000.0f);
    farClip = std::clamp(finite(farClip) ? farClip : 4000.0f, 0.01f, 1000000.0f);
    if (nearClip >= farClip){
        farClip = nearClip + 1.0f;
    }
    speedMin = std::clamp(finite(speedMin) ? speedMin : 0.5f, MIN_SPEED, MAX_SPEED);
    speedMax = std::clamp(finite(speedMax) ? speedMax : MAX_SPEED, MIN_SPEED, MAX_SPEED);
    if (speedMin > speedMax){
        std::swap(speedMin, speedMax);
    }
    speed = std::clamp(finite(speed) ? speed : 10.0f, speedMin, speedMax);
    shiftMultiplier = std::clamp(finite(shiftMultiplier) ? shiftMultiplier : 3.0f, 1.0f, 100.0f);
}

bool SceneCameraSettings::operator==(const SceneCameraSettings& other) const{
    return fieldOfView == other.fieldOfView && dynamicClipping == other.dynamicClipping && nearClip == other.nearClip &&
           farClip == other.farClip && speed == other.speed && speedMin == other.speedMin && speedMax == other.speedMax &&
           shiftMultiplier == other.shiftMultiplier && acceleration == other.acceleration && easing == other.easing;
}

SceneViewAxis SceneViewState::defaultPaneAxis(SceneViewLayout layout, int pane){
    if (pane <= 0){
        return SceneViewAxis::FREE;
    }
    if (layout == SceneViewLayout::SPLIT){
        return SceneViewAxis::TOP;
    }
    // Quad: Top (Y), Front (Z) and Side (X) besides the perspective pane
    return pane == 1 ? SceneViewAxis::TOP : (pane == 2 ? SceneViewAxis::FRONT : SceneViewAxis::RIGHT);
}

void SceneViewState::setLayout(SceneViewLayout newLayout){
    layout = newLayout;
    for (int pane = 1; pane < MAX_PANES; pane++){
        if (pane >= paneCount()){
            continue;
        }
        // a pane keeps the axis it was given before; one shown first takes the layout's own
        if (!panes[(size_t)pane].posed){
            panes[(size_t)pane].axis = defaultPaneAxis(newLayout, pane);
        }
    }
    if (activePane >= paneCount()){
        activePane = 0;
    }
    if (maximizedPane >= paneCount()){
        maximizedPane = -1;
    }
}

const char* SceneViewStateStream::axisName(SceneViewAxis axis){
    switch (axis){
        case SceneViewAxis::FREE: return "free";
        case SceneViewAxis::TOP: return "top";
        case SceneViewAxis::BOTTOM: return "bottom";
        case SceneViewAxis::FRONT: return "front";
        case SceneViewAxis::BACK: return "back";
        case SceneViewAxis::LEFT: return "left";
        case SceneViewAxis::RIGHT: return "right";
    }
    return "free";
}

bool SceneViewStateStream::parseAxis(const std::string& name, SceneViewAxis& axis){
    static const SceneViewAxis axes[] = {SceneViewAxis::FREE, SceneViewAxis::TOP, SceneViewAxis::BOTTOM, SceneViewAxis::FRONT,
                                         SceneViewAxis::BACK, SceneViewAxis::LEFT, SceneViewAxis::RIGHT};
    for (SceneViewAxis candidate : axes){
        if (name == axisName(candidate)){
            axis = candidate;
            return true;
        }
    }
    return false;
}

const char* SceneViewStateStream::layoutName(SceneViewLayout layout){
    switch (layout){
        case SceneViewLayout::SINGLE: return "single";
        case SceneViewLayout::SPLIT: return "split";
        case SceneViewLayout::QUAD: return "quad";
    }
    return "single";
}

bool SceneViewStateStream::parseLayout(const std::string& name, SceneViewLayout& layout){
    for (SceneViewLayout candidate : {SceneViewLayout::SINGLE, SceneViewLayout::SPLIT, SceneViewLayout::QUAD}){
        if (name == layoutName(candidate)){
            layout = candidate;
            return true;
        }
    }
    return false;
}

bool SceneViewStateStream::isOwnKey(const std::string& key){
    return std::find(std::begin(OWN_KEYS), std::end(OWN_KEYS), key) != std::end(OWN_KEYS);
}

YAML::Node SceneViewStateStream::encodePose(const SceneCameraPose& pose){
    YAML::Node node;
    node["position"] = encodeVector(pose.position);
    node["target"] = encodeVector(pose.target);
    node["up"] = encodeVector(pose.up);
    node["projection"] = pose.orthographic ? "orthographic" : "perspective";
    node["orthoSize"] = pose.orthoSize;
    node["fov"] = pose.fieldOfView;
    return node;
}

SceneCameraPose SceneViewStateStream::decodePose(const YAML::Node& node, const SceneCameraPose& fallback){
    SceneCameraPose pose = fallback;
    if (!node || !node.IsMap()){
        return pose;
    }
    pose.position = decodeVector(node["position"], fallback.position);
    pose.target = decodeVector(node["target"], fallback.target);
    pose.up = decodeVector(node["up"], fallback.up);
    if (node["projection"] && node["projection"].IsScalar()){
        pose.orthographic = node["projection"].as<std::string>() == "orthographic";
    }
    pose.orthoSize = std::max(0.0001f, readFloat(node["orthoSize"], fallback.orthoSize));
    pose.fieldOfView = std::clamp(readFloat(node["fov"], fallback.fieldOfView), SceneCameraSettings::MIN_FOV, SceneCameraSettings::MAX_FOV);
    return pose;
}

void SceneViewStateStream::encode(const SceneViewState& state, YAML::Node& node){
    if (state.unknownKeys && state.unknownKeys.IsMap()){
        for (const auto& entry : state.unknownKeys){
            const std::string key = entry.first.as<std::string>();
            if (!node[key]){
                node[key] = YAML::Clone(entry.second);
            }
        }
    }

    YAML::Node view;
    view["layout"] = layoutName(state.layout);
    view["activePane"] = state.activePane;
    if (state.maximizedPane >= 0){
        view["maximizedPane"] = state.maximizedPane;
    }
    view["projection"] = state.orthographic ? "orthographic" : "perspective";
    view["axisOrtho"] = state.axisOrtho;
    view["orthoSize"] = state.orthoSize;
    YAML::Node panes(YAML::NodeType::Sequence);
    for (int pane = 1; pane < SceneViewState::MAX_PANES; pane++){
        const SceneViewPane& p = state.panes[(size_t)pane];
        YAML::Node paneNode;
        paneNode["axis"] = axisName(p.axis);
        paneNode["shading"] = p.shading == PaneShading::SHADED_SHADOWS ? "shaded_shadows" : "shaded";
        if (p.posed){
            paneNode["camera"] = encodePose(p.pose);
        }
        panes.push_back(paneNode);
    }
    view["panes"] = panes;
    node["view"] = view;

    YAML::Node camera;
    camera["fov"] = state.camera.fieldOfView;
    camera["dynamicClipping"] = state.camera.dynamicClipping;
    camera["near"] = state.camera.nearClip;
    camera["far"] = state.camera.farClip;
    camera["speed"] = state.camera.speed;
    camera["speedMin"] = state.camera.speedMin;
    camera["speedMax"] = state.camera.speedMax;
    camera["shiftMultiplier"] = state.camera.shiftMultiplier;
    camera["acceleration"] = state.camera.acceleration;
    camera["easing"] = state.camera.easing;
    node["cameraSettings"] = camera;

    YAML::Node bookmarks(YAML::NodeType::Sequence);
    for (int slot = 0; slot < SceneViewState::BOOKMARK_SLOTS; slot++){
        const CameraBookmark& bookmark = state.bookmarks[(size_t)slot];
        if (!bookmark.defined){
            continue;
        }
        YAML::Node bookmarkNode = encodePose(bookmark.pose);
        bookmarkNode["slot"] = slot;
        if (!bookmark.name.empty()){
            bookmarkNode["name"] = bookmark.name;
        }
        bookmarks.push_back(bookmarkNode);
    }
    node["bookmarks"] = bookmarks;

    YAML::Node measures(YAML::NodeType::Sequence);
    for (const SceneMeasure& measure : state.measures){
        YAML::Node measureNode;
        measureNode["id"] = measure.id;
        measureNode["from"] = encodeVector(measure.from);
        measureNode["to"] = encodeVector(measure.to);
        measures.push_back(measureNode);
    }
    node["measures"] = measures;

    YAML::Node filter;
    filter["visible"] = state.layerFilter.visible;
    filter["pickable"] = state.layerFilter.pickable;
    node["layerFilter"] = filter;

    node["hiddenEntities"] = encodeEntities(state.hiddenEntities);
    node["pickLockedEntities"] = encodeEntities(state.pickLockedEntities);
}

SceneViewState SceneViewStateStream::decode(const YAML::Node& node){
    SceneViewState state;
    if (!node || !node.IsMap()){
        return state;
    }

    // what this version does not read, kept for the next save (S03)
    YAML::Node unknown(YAML::NodeType::Map);
    for (const auto& entry : node){
        const std::string key = entry.first.as<std::string>();
        if (!isOwnKey(key) && key != "editorCamera" && key != "displaySettings"){
            unknown[key] = YAML::Clone(entry.second);
        }
    }
    state.unknownKeys = unknown;

    if (const YAML::Node view = node["view"]; view && view.IsMap()){
        if (view["layout"] && view["layout"].IsScalar()){
            SceneViewLayout layout = SceneViewLayout::SINGLE;
            if (parseLayout(view["layout"].as<std::string>(), layout)){
                state.layout = layout;
            }
        }
        if (view["projection"] && view["projection"].IsScalar()){
            state.orthographic = view["projection"].as<std::string>() == "orthographic";
        }
        state.axisOrtho = readBool(view["axisOrtho"], false);
        state.orthoSize = std::max(0.0001f, readFloat(view["orthoSize"], state.orthoSize));
        if (const YAML::Node panes = view["panes"]; panes && panes.IsSequence()){
            for (size_t i = 0; i < panes.size() && i + 1 < (size_t)SceneViewState::MAX_PANES; i++){
                const YAML::Node paneNode = panes[i];
                SceneViewPane& pane = state.panes[i + 1];
                if (!paneNode.IsMap()){
                    continue;
                }
                if (paneNode["axis"] && paneNode["axis"].IsScalar()){
                    SceneViewAxis axis = SceneViewAxis::FREE;
                    if (parseAxis(paneNode["axis"].as<std::string>(), axis)){
                        pane.axis = axis;
                    }
                }
                if (paneNode["shading"] && paneNode["shading"].IsScalar()){
                    pane.shading = paneNode["shading"].as<std::string>() == "shaded_shadows" ? PaneShading::SHADED_SHADOWS : PaneShading::SHADED;
                }
                if (paneNode["camera"]){
                    pane.pose = decodePose(paneNode["camera"]);
                    pane.posed = true;
                }
            }
        }
        // the layout's own axes for the panes the file names none
        for (int pane = 1; pane < state.paneCount(); pane++){
            if (!state.panes[(size_t)pane].posed && state.panes[(size_t)pane].axis == SceneViewAxis::FREE){
                state.panes[(size_t)pane].axis = SceneViewState::defaultPaneAxis(state.layout, pane);
            }
        }
        state.activePane = std::clamp(view["activePane"] ? (int)readFloat(view["activePane"], 0.0f) : 0, 0, state.paneCount() - 1);
        const int maximized = view["maximizedPane"] ? (int)readFloat(view["maximizedPane"], -1.0f) : -1;
        state.maximizedPane = (maximized >= 0 && maximized < state.paneCount()) ? maximized : -1;
    }

    if (const YAML::Node camera = node["cameraSettings"]; camera && camera.IsMap()){
        SceneCameraSettings& c = state.camera;
        c.fieldOfView = readFloat(camera["fov"], c.fieldOfView);
        c.dynamicClipping = readBool(camera["dynamicClipping"], c.dynamicClipping);
        c.nearClip = readFloat(camera["near"], c.nearClip);
        c.farClip = readFloat(camera["far"], c.farClip);
        c.speed = readFloat(camera["speed"], c.speed);
        c.speedMin = readFloat(camera["speedMin"], c.speedMin);
        c.speedMax = readFloat(camera["speedMax"], c.speedMax);
        c.shiftMultiplier = readFloat(camera["shiftMultiplier"], c.shiftMultiplier);
        c.acceleration = readBool(camera["acceleration"], c.acceleration);
        c.easing = readBool(camera["easing"], c.easing);
    }else if (const YAML::Node editorCamera = node["editorCamera"]; editorCamera && editorCamera.IsMap()){
        // an older workspace: the fly speed was an offset from 10, and near, far and the field
        // of view were the editor camera's own (R36)
        SceneCameraSettings& c = state.camera;
        c.speed = 10.0f + readFloat(editorCamera["walkSpeed"], 0.0f);
        c.nearClip = readFloat(editorCamera["nearClip"], c.nearClip);
        c.farClip = readFloat(editorCamera["farClip"], c.farClip);
        // yfov is the camera component's, in radians
        const float yfov = readFloat(editorCamera["yfov"], NAN);
        if (finite(yfov) && yfov > 0.0f){
            c.fieldOfView = yfov * 57.29577951f;
        }
    }
    state.camera.clamp();

    if (const YAML::Node bookmarks = node["bookmarks"]; bookmarks && bookmarks.IsSequence()){
        for (const auto& bookmarkNode : bookmarks){
            if (!bookmarkNode.IsMap()){
                continue;
            }
            const int slot = (int)readFloat(bookmarkNode["slot"], -1.0f);
            if (slot < 0 || slot >= SceneViewState::BOOKMARK_SLOTS){
                continue;
            }
            CameraBookmark& bookmark = state.bookmarks[(size_t)slot];
            bookmark.defined = true;
            bookmark.pose = decodePose(bookmarkNode);
            if (bookmarkNode["name"] && bookmarkNode["name"].IsScalar()){
                bookmark.name = bookmarkNode["name"].as<std::string>();
            }
        }
    }

    if (const YAML::Node measures = node["measures"]; measures && measures.IsSequence()){
        for (const auto& measureNode : measures){
            if (!measureNode.IsMap()){
                continue;
            }
            SceneMeasure measure;
            measure.id = (uint32_t)std::max(0.0f, readFloat(measureNode["id"], 0.0f));
            measure.from = decodeVector(measureNode["from"], Vector3(NAN, NAN, NAN));
            measure.to = decodeVector(measureNode["to"], Vector3(NAN, NAN, NAN));
            if (measure.id == 0 || !finite(measure.from.x) || !finite(measure.to.x)){
                continue;
            }
            state.nextMeasureId = std::max(state.nextMeasureId, measure.id + 1);
            state.measures.push_back(measure);
        }
    }

    if (const YAML::Node filter = node["layerFilter"]; filter && filter.IsMap()){
        state.layerFilter.visible = readMask(filter["visible"], LAYER_MASK_EVERYTHING);
        state.layerFilter.pickable = readMask(filter["pickable"], LAYER_MASK_EVERYTHING);
    }

    decodeEntities(node["hiddenEntities"], state.hiddenEntities);
    decodeEntities(node["pickLockedEntities"], state.pickLockedEntities);
    return state;
}
