// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AnimatorEdits.h"

#include "animation/AnimationClipIO.h"
#include "animation/AnimationPlayerRuntime.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <map>
#include <set>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    // "name[3]" at `at` of `path`: the index, the position after it; false when not that form
    bool readIndexed(const std::string& path, size_t& at, const char* name, int& index){
        const std::string head = std::string(name) + "[";
        if (path.compare(at, head.size(), head) != 0) return false;
        const size_t close = path.find(']', at + head.size());
        if (close == std::string::npos) return false;
        const std::string number = path.substr(at + head.size(), close - at - head.size());
        if (number.empty() || number.find_first_not_of("0123456789") != std::string::npos) return false;
        index = std::atoi(number.c_str());
        at = close + 1;
        if (at < path.size() && path[at] == '.') at++;
        return true;
    }

    template<class Names>
    std::string uniqueName(const Names& taken, const std::string& base){
        auto has = [&](const std::string& name){ return std::find(taken.begin(), taken.end(), name) != taken.end(); };
        if (!base.empty() && !has(base)) return base;
        // "Walk 2" taken: "Walk 3", not "Walk 2 1"
        std::string root = base.empty() ? std::string("New") : base;
        const size_t space = root.find_last_of(' ');
        if (space != std::string::npos && space + 1 < root.size() && root.find_first_not_of("0123456789", space + 1) == std::string::npos){
            root = root.substr(0, space);
        }
        for (int k = 1;; k++){
            const std::string name = root + " " + std::to_string(k);
            if (!has(name)) return name;
        }
    }

    bool validLayer(const anim::ControllerData& data, int layer){
        return layer >= 0 && (size_t)layer < data.layers.size();
    }

    // the point where the segment from the box centre toward (tx, ty) leaves the box
    void edgePoint(const animator::Rect& rect, float tx, float ty, float& x, float& y){
        const float cx = rect.centerX(), cy = rect.centerY();
        const float dx = tx - cx, dy = ty - cy;
        if (dx == 0.0f && dy == 0.0f){
            x = cx;
            y = cy;
            return;
        }
        const float sx = dx != 0.0f ? (rect.w * 0.5f) / std::fabs(dx) : 1e9f;
        const float sy = dy != 0.0f ? (rect.h * 0.5f) / std::fabs(dy) : 1e9f;
        const float s = std::min(sx, sy);
        x = cx + dx * s;
        y = cy + dy * s;
    }

}

// ---- selection ----

animator::Item animator::itemOfPath(const std::string& jsonPath){
    Item item;
    size_t at = 0;
    int index = -1;
    if (readIndexed(jsonPath, at, "parameters", index)){
        item.kind = ItemKind::Parameter;
        item.index = index;
        return item;
    }
    int layer = -1;
    if (!readIndexed(jsonPath, at, "layers", layer)) return item;
    item.kind = ItemKind::Layer;
    item.layer = layer;
    if (readIndexed(jsonPath, at, "states", index)){
        item.kind = ItemKind::State;
        item.index = index;
    }else if (readIndexed(jsonPath, at, "transitions", index)){
        item.kind = ItemKind::Transition;
        item.index = index;
    }else if (jsonPath.compare(at, 9, "any_state") == 0){
        item.kind = ItemKind::AnyState;
    }
    return item;
}

// ---- names ----

std::string animator::uniqueStateName(const anim::ControllerLayer& layer, const std::string& base){
    std::vector<std::string> names;
    for (const anim::ControllerState& state : layer.states) names.push_back(state.name);
    names.push_back(anim::ANY_STATE);
    return uniqueName(names, base.empty() ? std::string("New State") : base);
}

std::string animator::uniqueParameterName(const anim::ControllerData& data, const std::string& base){
    // a Lua identifier: the number joins without a space ("Speed2")
    auto taken = [&](const std::string& name){ return data.findParameter(name) >= 0; };
    const std::string root = base.empty() ? std::string("Param") : base;
    if (!taken(root)) return root;
    size_t end = root.size();
    while (end > 0 && std::isdigit((unsigned char)root[end - 1])) end--;
    const std::string stem = end > 0 ? root.substr(0, end) : root;
    for (int k = 1;; k++){
        const std::string name = stem + std::to_string(k);
        if (!taken(name)) return name;
    }
}

std::string animator::uniqueLayerName(const anim::ControllerData& data, const std::string& base){
    std::vector<std::string> names;
    for (const anim::ControllerLayer& layer : data.layers) names.push_back(layer.name);
    return uniqueName(names, base.empty() ? std::string("New Layer") : base);
}

// ---- edits ----

int animator::addState(anim::ControllerData& data, int layer, anim::ControllerState state){
    if (!validLayer(data, layer)) return -1;
    anim::ControllerLayer& target = data.layers[(size_t)layer];
    state.name = uniqueStateName(target, state.name);
    if (!state.hasPosition){
        const std::array<float, 2> position = nextGridPosition(target);
        state.position[0] = position[0];
        state.position[1] = position[1];
        state.hasPosition = true;
    }
    target.states.push_back(std::move(state));
    return (int)target.states.size() - 1;
}

bool animator::renameState(anim::ControllerData& data, int layer, int state, const std::string& name, std::string* error){
    if (!validLayer(data, layer) || state < 0 || (size_t)state >= data.layers[(size_t)layer].states.size()){
        if (error) *error = "no such state";
        return false;
    }
    anim::ControllerLayer& target = data.layers[(size_t)layer];
    const std::string old = target.states[(size_t)state].name;
    if (name == old) return true;
    if (name.empty() || name == anim::ANY_STATE){
        if (error) *error = name.empty() ? "a state needs a name" : "\"any\" names the Any State";
        return false;
    }
    if (target.findState(name) >= 0){
        if (error) *error = "the layer already has a state named \"" + name + "\"";
        return false;
    }
    target.states[(size_t)state].name = name;
    for (anim::ControllerTransition& transition : target.transitions){
        if (transition.from == old) transition.from = name;
        if (transition.to == old) transition.to = name;
    }
    if (target.defaultState == old) target.defaultState = name;
    return true;
}

void animator::removeStates(anim::ControllerData& data, int layer, std::vector<int> states){
    if (!validLayer(data, layer)) return;
    anim::ControllerLayer& target = data.layers[(size_t)layer];
    std::set<std::string> names;
    for (int state : states){
        if (state >= 0 && (size_t)state < target.states.size()) names.insert(target.states[(size_t)state].name);
    }
    if (names.empty()) return;
    target.transitions.erase(std::remove_if(target.transitions.begin(), target.transitions.end(), [&](const anim::ControllerTransition& t){
        return names.count(t.from) || names.count(t.to);
    }), target.transitions.end());
    target.states.erase(std::remove_if(target.states.begin(), target.states.end(), [&](const anim::ControllerState& s){
        return names.count(s.name) != 0;
    }), target.states.end());
    if (names.count(target.defaultState)) target.defaultState = target.states.empty() ? std::string() : target.states[0].name;
}

bool animator::setDefaultState(anim::ControllerData& data, int layer, int state, std::string* error){
    if (!validLayer(data, layer) || state < 0 || (size_t)state >= data.layers[(size_t)layer].states.size()){
        if (error) *error = "no such state";
        return false;
    }
    data.layers[(size_t)layer].defaultState = data.layers[(size_t)layer].states[(size_t)state].name;
    return true;
}

int animator::addTransition(anim::ControllerData& data, int layer, int from, int to){
    if (!validLayer(data, layer)) return -1;
    anim::ControllerLayer& target = data.layers[(size_t)layer];
    if (to < 0 || (size_t)to >= target.states.size() || from >= (int)target.states.size()) return -1;
    anim::ControllerTransition transition;
    transition.from = from < 0 ? std::string(anim::ANY_STATE) : target.states[(size_t)from].name;
    transition.to = target.states[(size_t)to].name;
    // Any State transitions start without an exit time (Unity: they have no source to time)
    if (from < 0) transition.hasExitTime = false;
    target.transitions.push_back(transition);
    return (int)target.transitions.size() - 1;
}

void animator::removeTransitions(anim::ControllerData& data, int layer, std::vector<int> transitions){
    if (!validLayer(data, layer)) return;
    std::sort(transitions.begin(), transitions.end());
    transitions.erase(std::unique(transitions.begin(), transitions.end()), transitions.end());
    std::vector<anim::ControllerTransition>& list = data.layers[(size_t)layer].transitions;
    for (auto it = transitions.rbegin(); it != transitions.rend(); ++it){
        if (*it >= 0 && (size_t)*it < list.size()) list.erase(list.begin() + *it);
    }
}

int animator::addParameter(anim::ControllerData& data, anim::ControllerParameterType type, const std::string& name){
    // parameter names are Lua identifiers (the engine drops others)
    if (!name.empty() && !anim::isLuaIdentifier(name)) return -1;
    anim::ControllerParameter parameter;
    parameter.type = type;
    const char* base = type == anim::ControllerParameterType::Float ? "NewFloat" : type == anim::ControllerParameterType::Int ? "NewInt" :
                       type == anim::ControllerParameterType::Bool ? "NewBool" : "NewTrigger";
    parameter.name = uniqueParameterName(data, name.empty() ? std::string(base) : name);
    data.parameters.push_back(parameter);
    return (int)data.parameters.size() - 1;
}

bool animator::renameParameter(anim::ControllerData& data, int parameter, const std::string& name, std::string* error){
    if (parameter < 0 || (size_t)parameter >= data.parameters.size()){
        if (error) *error = "no such parameter";
        return false;
    }
    const std::string old = data.parameters[(size_t)parameter].name;
    if (name == old) return true;
    if (name.empty()){
        if (error) *error = "a parameter needs a name";
        return false;
    }
    if (!anim::isLuaIdentifier(name)){
        if (error) *error = "\"" + name + "\" is not a valid parameter name: letters, digits and _, not starting with a digit (a Lua identifier)";
        return false;
    }
    if (data.findParameter(name) >= 0){
        if (error) *error = "there is already a parameter named \"" + name + "\"";
        return false;
    }
    data.parameters[(size_t)parameter].name = name;
    for (anim::ControllerLayer& layer : data.layers){
        for (anim::ControllerTransition& transition : layer.transitions){
            for (anim::ControllerCondition& condition : transition.conditions){
                if (condition.parameter == old) condition.parameter = name;
            }
        }
        for (anim::ControllerState& state : layer.states){
            if (state.speedParameter == old) state.speedParameter = name;
            if (state.hasBlendTree && state.blendTree.parameter == old) state.blendTree.parameter = name;
        }
    }
    return true;
}

void animator::removeParameter(anim::ControllerData& data, int parameter){
    if (parameter < 0 || (size_t)parameter >= data.parameters.size()) return;
    const std::string name = data.parameters[(size_t)parameter].name;
    data.parameters.erase(data.parameters.begin() + parameter);
    for (anim::ControllerLayer& layer : data.layers){
        for (anim::ControllerTransition& transition : layer.transitions){
            transition.conditions.erase(std::remove_if(transition.conditions.begin(), transition.conditions.end(),
                                                       [&](const anim::ControllerCondition& c){ return c.parameter == name; }),
                                        transition.conditions.end());
        }
        for (anim::ControllerState& state : layer.states){
            if (state.speedParameter == name) state.speedParameter.clear();
            if (state.hasBlendTree && state.blendTree.parameter == name) state.blendTree.parameter.clear();
        }
    }
}

int animator::addLayer(anim::ControllerData& data, const std::string& name){
    anim::ControllerLayer layer;
    layer.name = uniqueLayerName(data, name);
    data.layers.push_back(layer);
    return (int)data.layers.size() - 1;
}

bool animator::removeLayer(anim::ControllerData& data, int layer, std::string* error){
    if (layer == 0){
        if (error) *error = "the base layer can't be removed";
        return false;
    }
    if (!validLayer(data, layer)){
        if (error) *error = "no such layer";
        return false;
    }
    data.layers.erase(data.layers.begin() + layer);
    return true;
}

bool animator::renameLayer(anim::ControllerData& data, int layer, const std::string& name, std::string* error){
    if (!validLayer(data, layer)){
        if (error) *error = "no such layer";
        return false;
    }
    if (data.layers[(size_t)layer].name == name) return true;
    if (name.empty()){
        if (error) *error = "a layer needs a name";
        return false;
    }
    if (data.findLayer(name) >= 0){
        if (error) *error = "there is already a layer named \"" + name + "\"";
        return false;
    }
    data.layers[(size_t)layer].name = name;
    return true;
}

bool animator::moveLayer(anim::ControllerData& data, int from, int to, std::string* error){
    if (!validLayer(data, from) || !validLayer(data, to)){
        if (error) *error = "no such layer";
        return false;
    }
    if (from == 0 || to == 0){
        if (error) *error = "the base layer stays first (its weight is always 1)";
        return false;
    }
    if (from == to) return true;
    anim::ControllerLayer moved = data.layers[(size_t)from];
    data.layers.erase(data.layers.begin() + from);
    data.layers.insert(data.layers.begin() + to, moved);
    return true;
}

animator::Clipboard animator::copyStates(const anim::ControllerData& data, int layer, const std::vector<int>& states){
    Clipboard clipboard;
    if (!validLayer(data, layer)) return clipboard;
    const anim::ControllerLayer& source = data.layers[(size_t)layer];
    std::set<std::string> names;
    for (int state : states){
        if (state < 0 || (size_t)state >= source.states.size()) continue;
        anim::ControllerState copy = source.states[(size_t)state];
        const std::array<float, 2> position = displayPosition(source, state);
        copy.position[0] = position[0];
        copy.position[1] = position[1];
        copy.hasPosition = true;
        names.insert(copy.name);
        clipboard.states.push_back(copy);
    }
    for (const anim::ControllerTransition& transition : source.transitions){
        if (names.count(transition.from) && names.count(transition.to)) clipboard.transitions.push_back(transition);
    }
    return clipboard;
}

std::vector<int> animator::pasteStates(anim::ControllerData& data, int layer, const Clipboard& clipboard, const std::array<float, 2>& at){
    std::vector<int> pasted;
    if (!validLayer(data, layer) || clipboard.empty()) return pasted;
    float minX = clipboard.states[0].position[0], minY = clipboard.states[0].position[1];
    for (const anim::ControllerState& state : clipboard.states){
        minX = std::min(minX, state.position[0]);
        minY = std::min(minY, state.position[1]);
    }
    std::map<std::string, std::string> renamed;
    for (const anim::ControllerState& state : clipboard.states){
        anim::ControllerState copy = state;
        copy.position[0] = state.position[0] - minX + at[0];
        copy.position[1] = state.position[1] - minY + at[1];
        const int index = addState(data, layer, copy);
        renamed[state.name] = data.layers[(size_t)layer].states[(size_t)index].name;
        pasted.push_back(index);
    }
    for (anim::ControllerTransition transition : clipboard.transitions){
        transition.from = renamed[transition.from];
        transition.to = renamed[transition.to];
        data.layers[(size_t)layer].transitions.push_back(transition);
    }
    return pasted;
}

// ---- graph geometry ----

std::array<float, 2> animator::displayPosition(const anim::ControllerLayer& layer, int state){
    if (state < 0 || (size_t)state >= layer.states.size()) return {0.0f, 0.0f};
    const anim::ControllerState& data = layer.states[(size_t)state];
    if (data.hasPosition) return {data.position[0], data.position[1]};
    // the n-th state without a position: a grid of four columns right of the Any State
    int n = 0;
    for (int i = 0; i < state; i++){
        if (!layer.states[(size_t)i].hasPosition) n++;
    }
    return {ANY_STATE_X + GRID_X * (float)(1 + n % 4), ANY_STATE_Y + GRID_Y * (float)(n / 4)};
}

animator::Rect animator::stateRect(const anim::ControllerLayer& layer, int state){
    const std::array<float, 2> position = displayPosition(layer, state);
    return {position[0], position[1], STATE_WIDTH, STATE_HEIGHT};
}

animator::Rect animator::anyStateRect(const anim::ControllerLayer& layer){
    if (layer.hasAnyStatePosition) return {layer.anyStatePosition[0], layer.anyStatePosition[1], STATE_WIDTH, STATE_HEIGHT};
    return {ANY_STATE_X, ANY_STATE_Y, STATE_WIDTH, STATE_HEIGHT};
}

std::array<float, 2> animator::nextGridPosition(const anim::ControllerLayer& layer){
    std::vector<Rect> taken;
    taken.push_back(anyStateRect(layer));
    for (size_t i = 0; i < layer.states.size(); i++) taken.push_back(stateRect(layer, (int)i));
    for (int cell = 0; cell < 4096; cell++){
        const Rect candidate{ANY_STATE_X + GRID_X * (float)(1 + cell % 4), ANY_STATE_Y + GRID_Y * (float)(cell / 4), STATE_WIDTH, STATE_HEIGHT};
        bool free = true;
        for (const Rect& rect : taken) free = free && !rect.overlaps(candidate);
        if (free) return {candidate.x, candidate.y};
    }
    return {ANY_STATE_X + GRID_X, ANY_STATE_Y};
}

int animator::hitState(const anim::ControllerLayer& layer, float x, float y){
    for (int i = (int)layer.states.size() - 1; i >= 0; i--){
        if (stateRect(layer, i).contains(x, y)) return i;
    }
    if (anyStateRect(layer).contains(x, y)) return -2;
    return -1;
}

std::vector<int> animator::statesIn(const anim::ControllerLayer& layer, const Rect& box){
    std::vector<int> out;
    for (size_t i = 0; i < layer.states.size(); i++){
        if (stateRect(layer, (int)i).overlaps(box)) out.push_back((int)i);
    }
    return out;
}

std::vector<animator::Arrow> animator::arrows(const anim::ControllerLayer& layer){
    std::vector<Arrow> out;
    for (size_t t = 0; t < layer.transitions.size(); t++){
        const anim::ControllerTransition& transition = layer.transitions[t];
        const int from = transition.from == anim::ANY_STATE ? -1 : layer.findState(transition.from);
        const int to = layer.findState(transition.to);
        if (to < 0 || (from < 0 && transition.from != anim::ANY_STATE)) continue;
        auto existing = std::find_if(out.begin(), out.end(), [&](const Arrow& a){ return a.from == from && a.to == to; });
        if (existing == out.end()){
            Arrow arrow;
            arrow.from = from;
            arrow.to = to;
            out.push_back(arrow);
            existing = out.end() - 1;
        }
        existing->transitions.push_back((int)t);
    }
    for (Arrow& arrow : out){
        const Rect a = arrow.from < 0 ? anyStateRect(layer) : stateRect(layer, arrow.from);
        const Rect b = stateRect(layer, arrow.to);
        if (arrow.from == arrow.to){
            // a self transition: a short arrow down the box's right side
            arrow.self = true;
            arrow.ax = a.x + a.w + 6.0f;
            arrow.ay = a.y + 4.0f;
            arrow.bx = a.x + a.w + 6.0f;
            arrow.by = a.y + a.h - 4.0f;
            continue;
        }
        edgePoint(a, b.centerX(), b.centerY(), arrow.ax, arrow.ay);
        edgePoint(b, a.centerX(), a.centerY(), arrow.bx, arrow.by);
        const bool reverse = std::any_of(out.begin(), out.end(), [&](const Arrow& other){ return other.from == arrow.to && other.to == arrow.from; });
        if (reverse){
            float dx = arrow.bx - arrow.ax, dy = arrow.by - arrow.ay;
            const float length = std::sqrt(dx * dx + dy * dy);
            if (length > 0.0f){
                // shifted to the arrow's right: the two directions sit side by side
                const float nx = -dy / length * 7.0f, ny = dx / length * 7.0f;
                arrow.ax += nx;
                arrow.ay += ny;
                arrow.bx += nx;
                arrow.by += ny;
            }
        }
    }
    return out;
}

int animator::hitArrow(const std::vector<Arrow>& arrows, float x, float y, float tolerance){
    int best = -1;
    float bestDistance = tolerance;
    for (size_t i = 0; i < arrows.size(); i++){
        const Arrow& arrow = arrows[i];
        const float dx = arrow.bx - arrow.ax, dy = arrow.by - arrow.ay;
        const float length2 = dx * dx + dy * dy;
        float t = length2 > 0.0f ? ((x - arrow.ax) * dx + (y - arrow.ay) * dy) / length2 : 0.0f;
        t = std::min(1.0f, std::max(0.0f, t));
        const float px = arrow.ax + dx * t - x, py = arrow.ay + dy * t - y;
        const float distance = std::sqrt(px * px + py * py);
        if (distance <= bestDistance){
            bestDistance = distance;
            best = (int)i;
        }
    }
    return best;
}

bool animator::bounds(const std::vector<Rect>& rects, Rect& out){
    if (rects.empty()) return false;
    float x0 = rects[0].x, y0 = rects[0].y, x1 = rects[0].x + rects[0].w, y1 = rects[0].y + rects[0].h;
    for (const Rect& rect : rects){
        x0 = std::min(x0, rect.x);
        y0 = std::min(y0, rect.y);
        x1 = std::max(x1, rect.x + rect.w);
        y1 = std::max(y1, rect.y + rect.h);
    }
    out = {x0, y0, x1 - x0, y1 - y0};
    return true;
}

void animator::frame(const Rect& content, float width, float height, float margin, float minZoom, float maxZoom,
                     float& panX, float& panY, float& zoom){
    const float usableW = std::max(1.0f, width - 2.0f * margin);
    const float usableH = std::max(1.0f, height - 2.0f * margin);
    zoom = std::min(usableW / std::max(1.0f, content.w), usableH / std::max(1.0f, content.h));
    zoom = std::min(maxZoom, std::max(minZoom, zoom));
    // the content's centre at the view's centre
    panX = content.centerX() - (width * 0.5f) / zoom;
    panY = content.centerY() - (height * 0.5f) / zoom;
}

animator::Timeline animator::transitionTimeline(const anim::ControllerTransition& transition, float sourceLength, float destinationLength,
                                                float destinationCycleOffset){
    Timeline timeline;
    timeline.sourceLength = sourceLength > 0.0f ? sourceLength : 1.0f;
    timeline.destinationLength = destinationLength > 0.0f ? destinationLength : 1.0f;
    const float exit = transition.hasExitTime ? transition.exitTime : 0.0f;
    // below 1 at that fraction of the first loop, from 1 at that value
    timeline.exitStart = exit * timeline.sourceLength;
    timeline.blend = transition.fixedDuration ? transition.duration : transition.duration * timeline.sourceLength;
    const float offset = transition.offset > 0.0f ? transition.offset : destinationCycleOffset;
    timeline.destinationStart = offset * timeline.destinationLength;
    return timeline;
}
