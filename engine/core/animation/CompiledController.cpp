// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "CompiledController.h"

#include "animation/AnimatorAssetIO.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <map>
#include <set>

using namespace jasperengine;
using namespace jasperengine::anim;

namespace{

    struct NodeRef{
        TransitionTargetKind kind = TransitionTargetKind::None;
        int index = -1;
    };

    class Compiler{
    public:
        Compiler(const ControllerDef& def, CompiledController& out): def(def), out(out){}

        void run(){
            out.guid = def.guid;
            out.parameters = def.parameters;
            for (size_t l = 0; l < def.layers.size(); l++){
                const LayerDef& layerDef = def.layers[l];
                CompiledLayer layer;
                layer.name = layerDef.name;
                layer.weight = l == 0 ? 1.0f : layerDef.weight;
                layer.blending = layerDef.blending;
                layer.mask = layerDef.mask;
                layer.slot = layerDef.slot;
                layer.stateBegin = (uint32_t)out.states.size();
                out.layers.push_back(layer);
                const std::string at = "layers[" + std::to_string(l) + "].state_machine";
                const int root = allocateMachine(layerDef.machine, (int)l, -1, layerDef.name, at);
                out.layers[l].rootMachine = root;
                out.layers[l].stateCount = (uint32_t)out.states.size() - out.layers[l].stateBegin;
                if (out.layers[l].stateCount == 0){
                    warn("layers[" + std::to_string(l) + "]", "the layer has no states: it plays nothing");
                }
            }
            // transitions after every node exists (targets anywhere in the layer)
            for (size_t m = 0; m < machineDefs.size(); m++) compileStateTransitions((int)m);
            for (size_t m = 0; m < machineDefs.size(); m++) compileMachineLists((int)m);
            for (size_t l = 0; l < out.layers.size(); l++){
                out.layers[l].anyBegin = (uint32_t)out.transitions.size();
                for (size_t m = 0; m < machineDefs.size(); m++){
                    if (out.machines[m].layer != (int)l) continue;
                    compileList(machineDefs[m]->anyState, (int)m, machinePaths[m] + ".any_state", -1, false, true);
                }
                out.layers[l].anyCount = (uint32_t)out.transitions.size() - out.layers[l].anyBegin;
            }
            for (size_t m = 0; m < machineDefs.size(); m++) resolveDefault((int)m);
            for (size_t l = 0; l < out.layers.size(); l++) checkReachable((int)l);
        }

    private:
        const ControllerDef& def;
        CompiledController& out;
        std::vector<const StateMachineDef*> machineDefs;    // per compiled machine
        std::vector<std::string> machinePaths;              // JSON paths
        std::vector<std::map<std::string, NodeRef>> machineNodes;
        std::vector<const StateDef*> stateDefs;             // per compiled state
        std::vector<std::string> statePaths;                // JSON paths

        void warn(const std::string& path, const std::string& message){
            out.diagnostics.push_back({path, message});
        }

        int parameter(const std::string& name, const std::string& path, const char* what){
            if (name.empty()) return -1;
            const int index = def.findParameter(name);
            if (index < 0) warn(path, std::string(what) + ": unknown parameter \"" + name + "\"");
            return index;
        }

        int floatParameter(const std::string& name, const std::string& path, const char* what){
            const int index = parameter(name, path, what);
            if (index >= 0 && def.parameters[(size_t)index].type != AnimatorParameterType::Float &&
                    def.parameters[(size_t)index].type != AnimatorParameterType::Int){
                warn(path, std::string(what) + ": \"" + name + "\" is a " +
                     (def.parameters[(size_t)index].type == AnimatorParameterType::Bool ? "bool" : "trigger") + ", not a number");
            }
            return index;
        }

        int clipSlot(const AnimatorAssetRef& ref){
            for (size_t i = 0; i < out.clips.size(); i++){
                if (!ref.path.empty() ? out.clips[i].path == ref.path : (!ref.guid.empty() && out.clips[i].guid == ref.guid)) return (int)i;
            }
            out.clips.push_back(ref);
            return (int)out.clips.size() - 1;
        }

        CompiledMotion compileMotion(const MotionDef& motion, const std::string& path, int depth, uint32_t& leaves){
            CompiledMotion result;
            leaves = 0;
            if (motion.isTree()){
                result.kind = CompiledMotion::Kind::Tree;
                result.index = compileTree(motion.tree.front(), path + ".blend_tree", depth + 1);
                leaves = out.trees[(size_t)result.index].leafCount;
                out.maxTreeDepth = std::max(out.maxTreeDepth, depth + 1);
            }else if (!motion.clip.empty()){
                if (motion.clip.path.empty()){
                    warn(path + ".clip", "a clip reference without a path plays nothing");
                    return result;
                }
                result.kind = CompiledMotion::Kind::Clip;
                result.index = clipSlot(motion.clip);
                leaves = 1;
            }
            return result;
        }

        int compileTree(const BlendTreeDef& tree, const std::string& path, int depth){
            const int index = (int)out.trees.size();
            out.trees.emplace_back();
            CompiledTree compiled;
            compiled.type = tree.type;
            const bool twoD = tree.type == BlendTreeType::SimpleDirectional2D || tree.type == BlendTreeType::FreeformDirectional2D ||
                              tree.type == BlendTreeType::FreeformCartesian2D;
            if (tree.type != BlendTreeType::Direct){
                compiled.parameterX = floatParameter(tree.parameter, path + ".parameter", "blend parameter");
                if (tree.parameter.empty()) warn(path + ".parameter", "the blend tree has no parameter: it stays at 0");
            }
            if (twoD){
                compiled.parameterY = floatParameter(tree.parameterY, path + ".parameter_y", "blend parameter");
                if (tree.parameterY.empty()) warn(path + ".parameter_y", "the 2D blend tree has no y parameter: it stays at 0");
            }
            compiled.childBegin = (uint32_t)out.children.size();
            compiled.childCount = (uint32_t)std::min(tree.children.size(), MAX_BLEND_CHILDREN);
            out.children.resize(out.children.size() + compiled.childCount);
            uint32_t leaves = 0;
            for (uint32_t c = 0; c < compiled.childCount; c++){
                const BlendChildDef& childDef = tree.children[c];
                const std::string at = path + ".children[" + std::to_string(c) + "]";
                CompiledTreeChild child;
                uint32_t childLeaves = 0;
                child.motion = compileMotion(childDef.motion, at + ".motion", depth, childLeaves);
                leaves += childLeaves;
                child.threshold = childDef.threshold;
                child.x = childDef.position.x;
                child.y = childDef.position.y;
                child.timeScale = childDef.timeScale;
                if (tree.type == BlendTreeType::Direct){
                    child.directParameter = floatParameter(childDef.directParameter, at + ".direct_parameter", "direct weight");
                    if (childDef.directParameter.empty()) warn(at + ".direct_parameter", "a direct child without a weight parameter never plays");
                }
                if (child.motion.kind == CompiledMotion::Kind::None) warn(at, "the child has no motion");
                out.children[compiled.childBegin + c] = child;
            }
            compiled.leafCount = leaves;
            if (compiled.childCount == 0) warn(path, "the blend tree has no children");
            if (twoD) precompute2D(compiled, path);
            out.trees[(size_t)index] = compiled;
            return index;
        }

        void precompute2D(CompiledTree& tree, const std::string& path){
            const uint32_t n = tree.childCount;
            for (uint32_t i = 0; i < n; i++){
                for (uint32_t j = i + 1; j < n; j++){
                    const CompiledTreeChild& a = out.children[tree.childBegin + i];
                    const CompiledTreeChild& b = out.children[tree.childBegin + j];
                    if (std::fabs(a.x - b.x) < 1e-6f && std::fabs(a.y - b.y) < 1e-6f){
                        warn(path + ".children[" + std::to_string(j) + "].position", "the same position as child " + std::to_string(i) +
                             ": only one of them plays there");
                    }
                }
            }
            if (tree.type == BlendTreeType::SimpleDirectional2D){
                for (uint32_t i = 0; i < n; i++){
                    const CompiledTreeChild& c = out.children[tree.childBegin + i];
                    if (std::fabs(c.x) < 1e-6f && std::fabs(c.y) < 1e-6f){
                        if (tree.centre < 0) tree.centre = (int)i;
                        continue;
                    }
                    for (uint32_t j = i + 1; j < n; j++){
                        const CompiledTreeChild& d = out.children[tree.childBegin + j];
                        if (std::fabs(d.x) < 1e-6f && std::fabs(d.y) < 1e-6f) continue;
                        const float cross = c.x * d.y - c.y * d.x;
                        const float dot = c.x * d.x + c.y * d.y;
                        if (std::fabs(cross) < 1e-6f && dot > 0.0f){
                            warn(path, "children " + std::to_string(i) + " and " + std::to_string(j) +
                                 " point the same direction: a simple directional tree needs one motion per direction "
                                 "(use a freeform tree for several speeds)");
                        }
                    }
                }
                return;
            }
            tree.pairBegin = (uint32_t)out.pairs.size();
            out.pairs.resize(out.pairs.size() + (size_t)n * n * 4, 0.0f);
            for (uint32_t i = 0; i < n; i++){
                const CompiledTreeChild& a = out.children[tree.childBegin + i];
                for (uint32_t j = 0; j < n; j++){
                    if (i == j) continue;
                    const CompiledTreeChild& b = out.children[tree.childBegin + j];
                    float* pair = &out.pairs[tree.pairBegin + ((size_t)i * n + j) * 4];
                    float vx, vy, scale = 1.0f;
                    if (tree.type == BlendTreeType::FreeformCartesian2D){
                        vx = b.x - a.x;
                        vy = b.y - a.y;
                    }else{
                        const float ma = std::sqrt(a.x * a.x + a.y * a.y);
                        const float mb = std::sqrt(b.x * b.x + b.y * b.y);
                        const float average = 0.5f * (ma + mb);
                        scale = average > 1e-6f ? average : 1.0f;
                        float angle = 0.0f;
                        if (ma > 1e-6f && mb > 1e-6f) angle = std::atan2(a.x * b.y - a.y * b.x, a.x * b.x + a.y * b.y);
                        vx = (mb - ma) / scale;
                        vy = angle * 2.0f;
                    }
                    const float len2 = vx * vx + vy * vy;
                    pair[0] = vx;
                    pair[1] = vy;
                    pair[2] = len2 > 1e-12f ? 1.0f / len2 : 0.0f;
                    pair[3] = scale;
                }
            }
        }

        int allocateMachine(const StateMachineDef& machineDef, int layer, int parent, const std::string& path, const std::string& at){
            const int index = (int)out.machines.size();
            CompiledMachine machine;
            machine.name = machineDef.name;
            machine.path = path;
            machine.layer = layer;
            machine.parent = parent;
            out.machines.push_back(machine);
            machineDefs.push_back(&machineDef);
            machinePaths.push_back(at);
            machineNodes.emplace_back();
            for (size_t s = 0; s < machineDef.states.size(); s++){
                const StateDef& stateDef = machineDef.states[s];
                const std::string stateAt = at + ".states[" + std::to_string(s) + "]";
                CompiledState state;
                state.name = stateDef.name;
                state.fullPath = path + "." + stateDef.name;
                state.tag = stateDef.tag;
                state.layer = layer;
                state.machine = index;
                uint32_t leaves = 0;
                state.motion = compileMotion(stateDef.motion, stateAt + ".motion", 0, leaves);
                out.maxMotionLeaves = std::max(out.maxMotionLeaves, std::max<uint32_t>(leaves, 1));
                state.speed = stateDef.speed;
                state.speedParameter = floatParameter(stateDef.speedParameter, stateAt + ".speed_parameter", "speed multiplier");
                state.cycleOffset = stateDef.cycleOffset;
                state.cycleOffsetParameter = floatParameter(stateDef.cycleOffsetParameter, stateAt + ".cycle_offset_parameter", "cycle offset");
                state.timeParameter = floatParameter(stateDef.timeParameter, stateAt + ".time_parameter", "motion time");
                state.loop = stateDef.loop;
                state.additiveReference = stateDef.additiveReference;
                state.additiveTime = stateDef.additiveTime;
                state.nameHash = animatorNameHash(state.name);
                state.fullPathHash = animatorNameHash(state.fullPath);
                machineNodes[(size_t)index][stateDef.name] = {TransitionTargetKind::State, (int)out.states.size()};
                out.states.push_back(state);
                stateDefs.push_back(&stateDef);
                statePaths.push_back(stateAt);
            }
            for (size_t m = 0; m < machineDef.subMachines.size(); m++){
                const StateMachineDef& sub = machineDef.subMachines[m];
                const int child = allocateMachine(sub, layer, index, path + "." + sub.name, at + ".sub_state_machines[" + std::to_string(m) + "]");
                machineNodes[(size_t)index][sub.name] = {TransitionTargetKind::Machine, child};
            }
            return index;
        }

        // `to` relative to `machine` (a leading '/': from the layer's root machine)
        NodeRef resolve(int machine, const std::string& to){
            NodeRef none;
            if (to.empty()) return none;
            int current = machine;
            size_t start = 0;
            if (to[0] == '/'){
                current = out.layers[(size_t)out.machines[(size_t)machine].layer].rootMachine;
                start = 1;
            }
            while (true){
                const size_t slash = to.find('/', start);
                const std::string segment = to.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
                if (segment.empty()) return none;
                if (slash == std::string::npos){
                    if (segment == "Exit") return {TransitionTargetKind::Exit, current};
                    auto it = machineNodes[(size_t)current].find(segment);
                    return it == machineNodes[(size_t)current].end() ? none : it->second;
                }
                auto it = machineNodes[(size_t)current].find(segment);
                if (it == machineNodes[(size_t)current].end() || it->second.kind != TransitionTargetKind::Machine) return none;
                current = it->second.index;
                start = slash + 1;
            }
        }

        void compileList(const std::vector<TransitionDef>& list, int machine, const std::string& at, int ownerState, bool entry, bool anyState){
            for (size_t i = 0; i < list.size(); i++){
                const TransitionDef& def_ = list[i];
                const std::string tAt = at + "[" + std::to_string(i) + "]";
                CompiledTransition t;
                t.to = def_.to;
                const NodeRef target = resolve(machine, def_.to);
                if (target.kind == TransitionTargetKind::None){
                    warn(tAt + ".to", "no state or state machine named \"" + def_.to + "\"");
                }else if ((entry || anyState) && target.kind == TransitionTargetKind::Exit){
                    warn(tAt + ".to", std::string(entry ? "an entry" : "an any-state") + " transition cannot go to Exit");
                }else{
                    t.kind = target.kind;
                    t.target = target.index;
                }
                t.conditionBegin = (uint32_t)out.conditions.size();
                for (size_t c = 0; c < def_.conditions.size(); c++){
                    const ConditionDef& cDef = def_.conditions[c];
                    const std::string cAt = tAt + ".conditions[" + std::to_string(c) + "]";
                    CompiledCondition condition;
                    condition.parameter = parameter(cDef.parameter, cAt + ".parameter", "condition");
                    condition.mode = cDef.mode;
                    condition.threshold = cDef.threshold;
                    if (condition.parameter >= 0){
                        const AnimatorParameterType type = def.parameters[(size_t)condition.parameter].type;
                        const AnimatorConditionMode mode = cDef.mode;
                        const bool boolMode = mode == AnimatorConditionMode::If || mode == AnimatorConditionMode::IfNot;
                        bool ok = true;
                        if (type == AnimatorParameterType::Trigger) ok = mode == AnimatorConditionMode::If;
                        else if (type == AnimatorParameterType::Bool) ok = boolMode;
                        else if (type == AnimatorParameterType::Float) ok = mode == AnimatorConditionMode::Greater || mode == AnimatorConditionMode::Less;
                        else ok = !boolMode;
                        if (!ok){
                            warn(cAt + ".mode", std::string("\"") + conditionModeName(mode) + "\" does not apply to the " +
                                 parameterTypeName(type) + " parameter \"" + cDef.parameter + "\": the condition never holds");
                            condition.parameter = -1;
                        }
                    }
                    out.conditions.push_back(condition);
                }
                t.conditionCount = (uint32_t)def_.conditions.size();
                t.hasExitTime = entry ? false : def_.hasExitTime;
                t.exitTime = def_.exitTime;
                t.fixedDuration = def_.fixedDuration;
                t.duration = entry ? 0.0f : def_.duration;
                t.offset = def_.offset;
                t.interruptionSource = def_.interruptionSource;
                t.orderedInterruption = def_.orderedInterruption;
                t.canTransitionToSelf = def_.canTransitionToSelf;
                t.mute = def_.mute;
                t.ownerState = ownerState;
                t.indexInOwner = ownerState >= 0 ? (int)i : -1;
                if (!entry && !t.hasExitTime && t.conditionCount == 0 && !t.mute && ownerState >= 0){
                    warn(tAt, "no condition and no exit time: the transition never fires");
                }
                if (anyState && !t.hasExitTime && t.conditionCount == 0 && !t.mute){
                    warn(tAt, "an any-state transition without conditions never fires");
                }
                out.transitions.push_back(t);
            }
        }

        void compileStateTransitions(int machine){
            for (size_t s = 0; s < out.states.size(); s++){
                CompiledState& state = out.states[s];
                if (state.machine != machine) continue;
                state.transitionBegin = (uint32_t)out.transitions.size();
                compileList(stateDefs[s]->transitions, machine, statePaths[s] + ".transitions", (int)s, false, false);
                state.transitionCount = (uint32_t)out.transitions.size() - state.transitionBegin;
            }
        }

        void compileMachineLists(int machine){
            const StateMachineDef& machineDef = *machineDefs[(size_t)machine];
            out.machines[(size_t)machine].entryBegin = (uint32_t)out.transitions.size();
            compileList(machineDef.entry, machine, machinePaths[(size_t)machine] + ".entry", -1, true, false);
            out.machines[(size_t)machine].entryCount = (uint32_t)out.transitions.size() - out.machines[(size_t)machine].entryBegin;
            const int parent = out.machines[(size_t)machine].parent;
            out.machines[(size_t)machine].exitBegin = (uint32_t)out.transitions.size();
            if (parent >= 0){
                compileList(machineDef.exitTransitions, parent, machinePaths[(size_t)machine] + ".exit_transitions", -1, true, false);
            }
            out.machines[(size_t)machine].exitCount = (uint32_t)out.transitions.size() - out.machines[(size_t)machine].exitBegin;
        }

        void resolveDefault(int machine){
            CompiledMachine& compiled = out.machines[(size_t)machine];
            const StateMachineDef& machineDef = *machineDefs[(size_t)machine];
            NodeRef target;
            if (!machineDef.defaultState.empty()){
                target = resolve(machine, machineDef.defaultState);
                if (target.kind == TransitionTargetKind::None || target.kind == TransitionTargetKind::Exit){
                    warn(machinePaths[(size_t)machine] + ".default_state", "no state or state machine named \"" + machineDef.defaultState +
                         "\": the first state is the default");
                    target = NodeRef();
                }
            }
            if (target.kind == TransitionTargetKind::None){
                if (!machineDef.states.empty()){
                    target = machineNodes[(size_t)machine][machineDef.states.front().name];
                }else if (!machineDef.subMachines.empty()){
                    target = machineNodes[(size_t)machine][machineDef.subMachines.front().name];
                }
            }
            compiled.defaultKind = target.kind;
            compiled.defaultTarget = target.index;
        }

        // the states a machine node leads to when entered (its entries and its default)
        void enterTargets(int machine, std::vector<NodeRef>& out_){
            const CompiledMachine& m = out.machines[(size_t)machine];
            for (uint32_t i = 0; i < m.entryCount; i++){
                const CompiledTransition& t = out.transitions[m.entryBegin + i];
                if (t.kind != TransitionTargetKind::None) out_.push_back({t.kind, t.target});
            }
            if (m.defaultKind != TransitionTargetKind::None) out_.push_back({m.defaultKind, m.defaultTarget});
        }

        void checkReachable(int layer){
            const CompiledLayer& l = out.layers[(size_t)layer];
            if (l.stateCount == 0 || l.rootMachine < 0) return;
            std::vector<bool> reached(out.states.size(), false);
            std::vector<bool> machineEntered(out.machines.size(), false);
            std::vector<bool> machineExited(out.machines.size(), false);
            std::deque<NodeRef> queue;
            queue.push_back({TransitionTargetKind::Machine, l.rootMachine});
            for (uint32_t i = 0; i < l.anyCount; i++){
                const CompiledTransition& t = out.transitions[l.anyBegin + i];
                if (t.kind != TransitionTargetKind::None && !t.mute) queue.push_back({t.kind, t.target});
            }
            while (!queue.empty()){
                const NodeRef node = queue.front();
                queue.pop_front();
                if (node.kind == TransitionTargetKind::State){
                    if (reached[(size_t)node.index]) continue;
                    reached[(size_t)node.index] = true;
                    const CompiledState& s = out.states[(size_t)node.index];
                    for (uint32_t i = 0; i < s.transitionCount; i++){
                        const CompiledTransition& t = out.transitions[s.transitionBegin + i];
                        if (t.kind != TransitionTargetKind::None && !t.mute) queue.push_back({t.kind, t.target});
                    }
                }else if (node.kind == TransitionTargetKind::Machine){
                    if (machineEntered[(size_t)node.index]) continue;
                    machineEntered[(size_t)node.index] = true;
                    std::vector<NodeRef> targets;
                    enterTargets(node.index, targets);
                    for (const NodeRef& target : targets) queue.push_back(target);
                }else if (node.kind == TransitionTargetKind::Exit){
                    if (machineExited[(size_t)node.index]) continue;
                    machineExited[(size_t)node.index] = true;
                    const CompiledMachine& m = out.machines[(size_t)node.index];
                    if (m.parent < 0){
                        // the root's exit re-enters the root
                        queue.push_back({TransitionTargetKind::Machine, node.index});
                        continue;
                    }
                    for (uint32_t i = 0; i < m.exitCount; i++){
                        const CompiledTransition& t = out.transitions[m.exitBegin + i];
                        if (t.kind != TransitionTargetKind::None) queue.push_back({t.kind, t.target});
                    }
                    queue.push_back({TransitionTargetKind::Machine, m.parent});
                }
            }
            for (uint32_t s = l.stateBegin; s < l.stateBegin + l.stateCount; s++){
                if (!reached[s]) warn(statePaths[s], "\"" + out.states[s].fullPath + "\" is unreachable (no transition leads to it)");
            }
        }
    };

}

uint32_t anim::animatorNameHash(const std::string& text){
    uint32_t hash = 2166136261u;
    for (unsigned char c : text){
        hash ^= c;
        hash *= 16777619u;
    }
    return hash;
}

int anim::CompiledController::findParameter(const std::string& name) const{
    for (size_t i = 0; i < parameters.size(); i++){
        if (parameters[i].name == name) return (int)i;
    }
    return -1;
}

int anim::CompiledController::findLayer(const std::string& name) const{
    for (size_t i = 0; i < layers.size(); i++){
        if (layers[i].name == name) return (int)i;
    }
    return -1;
}

int anim::CompiledController::findState(const std::string& nameOrPath, int layer) const{
    const bool path = nameOrPath.find('.') != std::string::npos;
    for (size_t i = 0; i < states.size(); i++){
        if (layer >= 0 && states[i].layer != layer) continue;
        if (path ? states[i].fullPath == nameOrPath : states[i].name == nameOrPath) return (int)i;
    }
    // a state name with a '.' in a layer name: try the plain name too
    if (path){
        for (size_t i = 0; i < states.size(); i++){
            if (layer >= 0 && states[i].layer != layer) continue;
            if (states[i].name == nameOrPath) return (int)i;
        }
    }
    return -1;
}

std::shared_ptr<const CompiledController> anim::compileController(const ControllerDef& def){
    auto compiled = std::make_shared<CompiledController>();
    Compiler compiler(def, *compiled);
    compiler.run();
    return compiled;
}
