// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "UnityControllerImport.h"
#include "UnityProject.h"
#include "UnityYaml.h"

#include "Project.h"
#include "command/type/WriteFilesCmd.h"
#include "util/ProjectFileWrites.h"

#include "animation/AnimationClipIO.h"
#include "animation/AnimatorAssetIO.h"
#include "animation/ControllerDef.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::editor::unity;
namespace fs = std::filesystem;
using Json = nlohmann::json;

namespace {

    std::string readImportText(const fs::path& file, bool& ok) {
        std::ifstream in(file, std::ios::binary);
        ok = (bool)in;
        if (!in) return std::string();
        std::stringstream buffer;
        buffer << in.rdbuf();
        return buffer.str();
    }

    std::string extensionLower(const fs::path& file) {
        std::string ext = file.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        return ext;
    }

    std::string genericImportPath(const fs::path& path) {
        return path.generic_string();
    }

    // the Unity project path of a file ("Assets/Anim/Walk.anim"), else its name
    std::string unityRelative(const jasperengine::editor::unity::Project* unityProject, const fs::path& file) {
        if (unityProject && !unityProject->root().empty()) {
            const std::string relative = unityProject->relative(file);
            if (!relative.empty() && relative.rfind("..", 0) != 0) return relative;
        }
        return file.filename().string();
    }

    // a reference to an engine asset from another one: relative to the assets folder
    std::string assetsRelativeRef(const fs::path& assets, const fs::path& file) {
        return genericImportPath(file.lexically_normal().lexically_relative(assets.lexically_normal()));
    }

    // what this import writes and references, shared by the controller and its dependencies
    struct ImportSession {
        const AssetImportOptions& options;
        jasperengine::editor::unity::Project unity;
        bool hasUnity = false;
        AssetImportResult& result;
        Json clips = Json::array();
        Json masks = Json::array();
        Json unmapped = Json::array();
        Json notes = Json::array();
        std::map<std::string, anim::AnimatorAssetRef> imported;   // Unity guid -> engine reference
        std::set<std::string> planned;                            // paths of files this import writes

        ImportSession(const AssetImportOptions& options, AssetImportResult& result) : options(options), result(result) {}

        void note(const std::string& code, const std::string& message) {
            notes.push_back({{"code", code}, {"message", message}});
        }
        void miss(const std::string& what, const std::string& reason) {
            unmapped.push_back({{"what", what}, {"reason", reason}});
        }

        // where a dependency goes: beside the asset, numbered past files that are not an
        // earlier import of the same Unity file (`sameSource` says which are)
        fs::path dependencyPath(const fs::path& unityFile, const std::function<bool(const fs::path&)>& sameSource, bool& reuse) {
            const fs::path folder = options.dest.parent_path();
            const std::string stem = unityFile.stem().string();
            const std::string ext = extensionLower(unityFile);
            for (int n = 1; n < 10000; n++) {
                const fs::path candidate = folder / (n == 1 ? stem + ext : stem + " " + std::to_string(n) + ext);
                if (planned.count(genericImportPath(candidate))) continue;
                std::error_code ec;
                if (!fs::exists(candidate, ec)) {
                    reuse = false;
                    return candidate;
                }
                if (sameSource(candidate)) {
                    reuse = true;
                    return candidate;
                }
            }
            reuse = false;
            return folder / (stem + ext);
        }

        // a Unity .anim by guid, imported beside the asset (once per import)
        bool clipRef(const std::string& guid, const fs::path& unityFile, anim::AnimatorAssetRef& out) {
            auto it = imported.find(guid);
            if (it != imported.end()) {
                out = it->second;
                return true;
            }
            const std::string unityPath = unityRelative(hasUnity ? &unity : nullptr, unityFile);
            AnimImportResult clip = importUnityClip(unityFile, options.mirrorX);
            if (!clip.ok) {
                miss(unityPath, clip.error);
                return false;
            }
            bool reuse = false;
            const fs::path path = dependencyPath(unityFile, [&](const fs::path& existing) {
                bool ok = false;
                const anim::ClipParseResult parsed = anim::parseClip(readImportText(existing, ok));
                return ok && parsed.ok() && parsed.data->source.kind == "unity" && parsed.data->source.path == unityPath;
            }, reuse);
            anim::AnimatorAssetRef ref;
            ref.path = assetsRelativeRef(options.engineAssets, path);
            if (reuse) {
                bool ok = false;
                ref.guid = anim::readClipGuid(readImportText(path, ok));
            } else {
                clip.clip.guid = anim::generateClipGuid();
                ref.guid = clip.clip.guid;
                result.files.push_back({path, anim::writeClip(clip.clip)});
                planned.insert(genericImportPath(path));
            }
            clips.push_back({{"unity", unityPath}, {"path", ref.path}, {"reused", reuse}, {"tracks", clip.report["tracks"]},
                             {"unmapped", clip.report["unmapped"].size()}, {"notes", clip.report["notes"]}});
            imported[guid] = ref;
            out = ref;
            return true;
        }
    };

    // ---------------------------------------------------------------- mask

    bool maskFromYaml(const YamlFile& file, const std::string& sourcePath, anim::AvatarMaskDef& mask, Json& report, std::string& error) {
        const Document* doc = nullptr;
        for (const Document& d : file.documents) {
            if (d.typeName == "AvatarMask") doc = &d;
        }
        if (!doc || !doc->body || !doc->body.IsMap()) {
            error = "no AvatarMask in the file (a Unity .mask holds one AvatarMask document)";
            return false;
        }
        const YAML::Node body = doc->body;
        Json notes = Json::array();
        // Unity's AvatarMaskBodyPart order: Root, Body, Head, LeftLeg, RightLeg, LeftArm, RightArm,
        // LeftFingers, RightFingers, LeftFootIK, RightFootIK, LeftHandIK, RightHandIK
        std::vector<int> parts;
        const YAML::Node bits = body["m_Mask"];
        if (bits && bits.IsScalar()) {
            const std::string hex = bits.Scalar();
            for (size_t i = 0; i + 8 <= hex.size(); i += 8) {
                // a little-endian uint32 per part
                const unsigned long value = std::strtoul(hex.substr(i, 2).c_str(), nullptr, 16);
                parts.push_back((int)value);
            }
        } else if (bits && bits.IsSequence()) {
            for (const YAML::Node& n : bits) parts.push_back(n.IsScalar() ? std::atoi(n.Scalar().c_str()) : 1);
        }
        bool allOn = true;
        for (size_t i = 0; i < parts.size() && i < 9; i++) allOn = allOn && parts[i] != 0;
        Json bodyParts = Json::object();
        if (!allOn) {
            using P = AvatarBodyPart;
            auto at = [&](size_t i) { return i < parts.size() && parts[i] != 0 ? 1.0f : 0.0f; };
            const std::pair<P, size_t> map[] = {{P::Root, 0}, {P::Body, 1}, {P::Head, 2}, {P::LeftLeg, 3}, {P::RightLeg, 4},
                                                {P::LeftArm, 5}, {P::LeftHand, 5}, {P::RightArm, 6}, {P::RightHand, 6},
                                                {P::LeftFingers, 7}, {P::RightFingers, 8}};
            for (const auto& [part, index] : map) {
                mask.bodyParts[(int)part] = at(index);
                bodyParts[anim::bodyPartName(part)] = at(index);
            }
        }
        for (size_t i = 9; i < parts.size() && i < 13; i++) {
            if (parts[i] == 0) {
                notes.push_back({{"code", "ik-goal"}, {"message", "the mask's IK goals are not imported (Foot IK and the IK rigs read no mask)"}});
                break;
            }
        }
        // the transform list: only the listed transforms with weight move (Unity's rule)
        std::vector<anim::MaskPathDef> listed;
        bool anyOff = false;
        const YAML::Node elements = body["m_Elements"];
        if (elements && elements.IsSequence()) {
            for (const YAML::Node& e : elements) {
                if (!e.IsMap()) continue;
                anim::MaskPathDef path;
                path.path = readString(e, "m_Path");
                path.weight = std::min(1.0f, std::max(0.0f, readFloat(e, "m_Weight", 1.0f)));
                anyOff = anyOff || path.weight < 1.0f;
                listed.push_back(path);
            }
        }
        if (anyOff) {
            mask.defaultWeight = 0.0f;
            for (const anim::MaskPathDef& path : listed) {
                if (path.weight > 0.0f) mask.paths.push_back(path);
            }
            if (!allOn) {
                notes.push_back({{"code", "transforms-and-body-parts"},
                                 {"message", "the mask has body parts and a transform list: a listed transform's weight wins over its body part "
                                             "in JasperEngine; check the humanoid bones' paths with edit_avatar_mask"}});
            }
        }
        std::vector<ClipWarning> warnings;
        anim::normalizeMask(mask, &warnings);
        for (const ClipWarning& w : warnings) notes.push_back({{"code", "normalized"}, {"message", w.path + ": " + w.message}});
        mask.unknownKeys.push_back({"source", Json{{"kind", "unity"}, {"path", sourcePath}}.dump()});
        report = {{"name", readString(body, "m_Name")}, {"body_parts", bodyParts}, {"paths", mask.paths.size()},
                  {"default", mask.defaultWeight}, {"notes", notes}};
        return true;
    }

    bool sameMaskSource(const fs::path& existing, const std::string& unityPath) {
        bool ok = false;
        const anim::MaskParseResult parsed = anim::parseMask(readImportText(existing, ok));
        if (!ok || !parsed.ok) return false;
        for (const auto& [key, value] : parsed.data.unknownKeys) {
            if (key != "source") continue;
            const Json source = Json::parse(value, nullptr, false);
            return source.is_object() && source.value("kind", "") == "unity" && source.value("path", "") == unityPath;
        }
        return false;
    }

    // ---------------------------------------------------------------- controller

    struct ControllerReader {
        ImportSession& session;
        const YamlFile& file;
        anim::ControllerDef def;
        // per state / machine document: its path from the layer's root machine ("/Combat/Attack")
        // and its machine's document
        std::map<int64_t, std::string> paths;
        std::map<int64_t, int64_t> owner;
        int states = 0;
        std::string layerName;

        ControllerReader(ImportSession& session, const YamlFile& file) : session(session), file(file) {}

        const YAML::Node bodyOf(int64_t id, const char* type) const {
            const Document* doc = file.find(id);
            if (!doc || doc->typeName != type || !doc->body || !doc->body.IsMap()) return YAML::Node();
            return doc->body;
        }

        // names the engine accepts: no '/' or '.', not a special node, unique in the machine
        static std::string engineName(std::string name, std::set<std::string>& used) {
            for (char& c : name) {
                if (c == '/' || c == '.') c = '_';
            }
            if (name.empty()) name = "State";
            if (name == "Entry" || name == "Exit" || name == "Any State") name += " State";
            std::string candidate = name;
            for (int n = 2; used.count(candidate); n++) candidate = name + " " + std::to_string(n);
            used.insert(candidate);
            return candidate;
        }

        void collectNames(int64_t machineId, const std::string& machinePath, std::map<int64_t, std::string>& names) {
            const YAML::Node body = bodyOf(machineId, "AnimatorStateMachine");
            if (!body) return;
            std::set<std::string> used;
            auto each = [&](const char* list, const char* member, const char* type, bool machine) {
                const YAML::Node children = body[list];
                if (!children || !children.IsSequence()) return;
                for (const YAML::Node& child : children) {
                    const int64_t id = readRef(child, member).fileID;
                    const YAML::Node childBody = bodyOf(id, type);
                    if (!childBody) continue;
                    const std::string name = engineName(readString(childBody, "m_Name"), used);
                    names[id] = name;
                    paths[id] = machinePath + "/" + name;
                    owner[id] = machineId;
                    if (machine) collectNames(id, machinePath + "/" + name, names);
                }
            };
            each("m_ChildStates", "m_State", "AnimatorState", false);
            each("m_ChildStateMachines", "m_StateMachine", "AnimatorStateMachine", true);
        }

        // a transition's target from the machine `from`: a name in it, else the path from the root
        std::string target(const YAML::Node& t, int64_t from) {
            if (readBool(t, "m_IsExit")) return "Exit";
            int64_t dst = readRef(t, "m_DstState").fileID;
            if (dst == 0) dst = readRef(t, "m_DstStateMachine").fileID;
            auto it = paths.find(dst);
            if (it == paths.end()) return std::string();
            auto o = owner.find(dst);
            if (o != owner.end() && o->second == from) return it->second.substr(it->second.rfind('/') + 1);
            return it->second;
        }

        anim::TransitionDef transition(int64_t id, int64_t from, const std::string& where, bool& solo) {
            anim::TransitionDef out;
            const Document* doc = file.find(id);
            const YAML::Node t = doc && doc->body && doc->body.IsMap() ? doc->body : YAML::Node();
            if (!t) return out;
            out.to = target(t, from);
            if (out.to.empty()) session.miss(where, "the transition's destination is not in this controller: dropped");
            const YAML::Node conditions = t["m_Conditions"];
            if (conditions && conditions.IsSequence()) {
                for (const YAML::Node& c : conditions) {
                    anim::ConditionDef condition;
                    condition.parameter = readString(c, "m_ConditionEvent");
                    condition.threshold = readFloat(c, "m_EventTreshold");
                    switch (readInt(c, "m_ConditionMode", 1)) {
                        case 2: condition.mode = AnimatorConditionMode::IfNot; break;
                        case 3: condition.mode = AnimatorConditionMode::Greater; break;
                        case 4: condition.mode = AnimatorConditionMode::Less; break;
                        case 6: condition.mode = AnimatorConditionMode::Equals; break;
                        case 7: condition.mode = AnimatorConditionMode::NotEqual; break;
                        default: condition.mode = AnimatorConditionMode::If; break;
                    }
                    out.conditions.push_back(condition);
                }
            }
            out.mute = readBool(t, "m_Mute");
            solo = readBool(t, "m_Solo");
            if (doc->typeName == "AnimatorStateTransition") {
                out.hasExitTime = readBool(t, "m_HasExitTime");
                out.exitTime = readFloat(t, "m_ExitTime", 0.75f);
                out.fixedDuration = readBool(t, "m_HasFixedDuration", true);
                out.duration = readFloat(t, "m_TransitionDuration", 0.25f);
                out.offset = readFloat(t, "m_TransitionOffset");
                switch (readInt(t, "m_InterruptionSource")) {
                    case 1: out.interruptionSource = TransitionInterruptionSource::Current; break;
                    case 2: out.interruptionSource = TransitionInterruptionSource::Next; break;
                    case 3: out.interruptionSource = TransitionInterruptionSource::CurrentThenNext; break;
                    case 4: out.interruptionSource = TransitionInterruptionSource::NextThenCurrent; break;
                    default: out.interruptionSource = TransitionInterruptionSource::None; break;
                }
                out.orderedInterruption = readBool(t, "m_OrderedInterruption", true);
                out.canTransitionToSelf = readBool(t, "m_CanTransitionToSelf", true);
            } else {
                out.hasExitTime = false;
                out.duration = 0.0f;
            }
            return out;
        }

        // a list of transition refs; Unity's Solo: only the solo ones are taken
        std::vector<anim::TransitionDef> transitions(const YAML::Node& list, int64_t from, const std::string& where) {
            std::vector<anim::TransitionDef> out;
            std::vector<bool> solos;
            if (!list || !list.IsSequence()) return out;
            for (const YAML::Node& ref : list) {
                bool solo = false;
                anim::TransitionDef t = transition(toRef(ref).fileID, from, where + "[" + std::to_string(out.size()) + "]", solo);
                if (t.to.empty()) continue;
                out.push_back(t);
                solos.push_back(solo);
            }
            if (std::find(solos.begin(), solos.end(), true) != solos.end()) {
                for (size_t i = 0; i < out.size(); i++) out[i].mute = out[i].mute || !solos[i];
            }
            return out;
        }

        void motion(const Ref& ref, anim::MotionDef& out, const std::string& where, int depth = 0) {
            if (ref.isNull()) return;
            if (ref.isLocal()) {
                const YAML::Node tree = bodyOf(ref.fileID, "BlendTree");
                if (!tree || depth > 16) {
                    session.miss(where, "the motion is not a blend tree of this controller");
                    return;
                }
                anim::BlendTreeDef def;
                switch (readInt(tree, "m_BlendType")) {
                    case 1: def.type = BlendTreeType::SimpleDirectional2D; break;
                    case 2: def.type = BlendTreeType::FreeformDirectional2D; break;
                    case 3: def.type = BlendTreeType::FreeformCartesian2D; break;
                    case 4: def.type = BlendTreeType::Direct; break;
                    default: def.type = BlendTreeType::Simple1D; break;
                }
                def.parameter = readString(tree, "m_BlendParameter");
                if (def.type != BlendTreeType::Simple1D && def.type != BlendTreeType::Direct) def.parameterY = readString(tree, "m_BlendParameterY");
                const YAML::Node children = tree["m_Childs"];
                if (children && children.IsSequence()) {
                    for (const YAML::Node& c : children) {
                        anim::BlendChildDef child;
                        child.threshold = readFloat(c, "m_Threshold");
                        const YAML::Node position = c["m_Position"];
                        if (position && position.IsMap()) child.position = Vector2(readFloat(position, "x"), readFloat(position, "y"));
                        child.timeScale = readFloat(c, "m_TimeScale", 1.0f);
                        child.directParameter = readString(c, "m_DirectBlendParameter");
                        child.mirror = readBool(c, "m_Mirror");
                        if (readFloat(c, "m_CycleOffset") != 0.0f) session.note("child-cycle-offset", where + ": a blend tree child's Cycle Offset is not imported");
                        motion(readRef(c, "m_Motion"), child.motion, where + ".children[" + std::to_string(def.children.size()) + "]", depth + 1);
                        def.children.push_back(std::move(child));
                    }
                }
                out.tree.push_back(std::move(def));
                return;
            }
            if (jasperengine::editor::unity::Project::isBuiltinGuid(ref.guid)) {
                session.miss(where, "a Unity built-in motion");
                return;
            }
            const fs::path asset = session.hasUnity ? session.unity.pathOf(ref.guid) : fs::path();
            if (asset.empty()) {
                session.miss(where, "the motion's GUID " + ref.guid + " is not in the Unity project" +
                                    (session.hasUnity ? std::string() : std::string(" (no Unity project found above the file: Assets/ and ProjectSettings/)")));
                return;
            }
            const std::string ext = extensionLower(asset);
            if (ext == ".anim") {
                session.clipRef(ref.guid, asset, out.clip);
                return;
            }
            // a clip inside a model file: its name from the .meta (Unity 2019+ internalIDToNameTable, older fileIDToRecycleName)
            std::string clipName;
            const YAML::Node meta = session.unity.metaOf(asset);
            if (meta && meta.IsMap()) {
                const YAML::Node importer = meta["ModelImporter"];
                if (importer && importer.IsMap()) {
                    const YAML::Node table = importer["internalIDToNameTable"];
                    if (table && table.IsSequence()) {
                        for (const YAML::Node& entry : table) {
                            const YAML::Node first = entry["first"];
                            if (first && first.IsMap() && first["74"] && first["74"].IsScalar() &&
                                std::strtoll(first["74"].Scalar().c_str(), nullptr, 10) == ref.fileID) clipName = readString(entry, "second");
                        }
                    }
                    const YAML::Node recycle = importer["fileIDToRecycleName"];
                    if (clipName.empty() && recycle && recycle.IsMap()) clipName = readString(recycle, std::to_string(ref.fileID).c_str());
                }
            }
            session.miss(where, "a clip of the model " + unityRelative(&session.unity, asset) + (clipName.empty() ? std::string() : " (\"" + clipName + "\")") +
                                ": import the model into the project, then set the state's clip to \"<model path>#" +
                                (clipName.empty() ? std::string("<clip>") : clipName) + "\" with edit_animator_controller set_state");
        }

        void machine(int64_t id, anim::StateMachineDef& out, const std::string& where, const std::map<int64_t, std::string>& names) {
            const YAML::Node body = bodyOf(id, "AnimatorStateMachine");
            if (!body) return;
            auto position = [](const YAML::Node& n, Vector2 fallback) {
                return n && n.IsMap() ? Vector2(readFloat(n, "x"), readFloat(n, "y")) : fallback;
            };
            out.entryPosition = position(body["m_EntryPosition"], out.entryPosition);
            out.anyStatePosition = position(body["m_AnyStatePosition"], out.anyStatePosition);
            out.exitPosition = position(body["m_ExitPosition"], out.exitPosition);
            const YAML::Node behaviours = body["m_StateMachineBehaviours"];
            if (behaviours && behaviours.IsSequence() && behaviours.size() > 0) {
                session.miss(where, "state machine behaviours (scripts) are not imported");
            }
            const YAML::Node childStates = body["m_ChildStates"];
            if (childStates && childStates.IsSequence()) {
                for (const YAML::Node& child : childStates) {
                    const int64_t stateId = readRef(child, "m_State").fileID;
                    const YAML::Node s = bodyOf(stateId, "AnimatorState");
                    if (!s) continue;
                    anim::StateDef state;
                    state.name = names.at(stateId);
                    const std::string at = where + ".states[" + state.name + "]";
                    state.position = position(child["m_Position"], state.position);
                    state.speed = readFloat(s, "m_Speed", 1.0f);
                    state.cycleOffset = readFloat(s, "m_CycleOffset");
                    state.mirror = readBool(s, "m_Mirror");
                    state.footIK = readBool(s, "m_IKOnFeet");
                    state.tag = readString(s, "m_Tag");
                    if (readBool(s, "m_SpeedParameterActive")) state.speedParameter = readString(s, "m_SpeedParameter");
                    if (readBool(s, "m_CycleOffsetParameterActive")) state.cycleOffsetParameter = readString(s, "m_CycleOffsetParameter");
                    if (readBool(s, "m_TimeParameterActive")) state.timeParameter = readString(s, "m_TimeParameter");
                    if (readBool(s, "m_MirrorParameterActive")) session.miss(at, "a mirror driven by a parameter (Mirror Parameter) is not supported");
                    const YAML::Node stateBehaviours = s["m_StateMachineBehaviours"];
                    if (stateBehaviours && stateBehaviours.IsSequence() && stateBehaviours.size() > 0) {
                        session.miss(at, "state machine behaviours (scripts) are not imported");
                    }
                    motion(readRef(s, "m_Motion"), state.motion, at + ".motion");
                    state.transitions = transitions(s["m_Transitions"], id, at + ".transitions");
                    out.states.push_back(std::move(state));
                    states++;
                }
            }
            const YAML::Node childMachines = body["m_ChildStateMachines"];
            std::map<int64_t, size_t> subIndex;
            if (childMachines && childMachines.IsSequence()) {
                for (const YAML::Node& child : childMachines) {
                    const int64_t subId = readRef(child, "m_StateMachine").fileID;
                    if (!bodyOf(subId, "AnimatorStateMachine")) continue;
                    anim::StateMachineDef sub;
                    sub.name = names.at(subId);
                    sub.position = position(child["m_Position"], sub.position);
                    machine(subId, sub, where + ".sub_state_machines[" + sub.name + "]", names);
                    subIndex[subId] = out.subMachines.size();
                    out.subMachines.push_back(std::move(sub));
                }
            }
            out.entry = transitions(body["m_EntryTransitions"], id, where + ".entry");
            out.anyState = transitions(body["m_AnyStateTransitions"], id, where + ".any_state");
            // a sub-state machine's exits, listed by its parent: {first: machine, second: [transitions]}
            const YAML::Node exits = body["m_StateMachineTransitions"];
            if (exits && exits.IsSequence()) {
                for (const YAML::Node& pair : exits) {
                    const int64_t sub = readRef(pair, "first").fileID;
                    auto at = subIndex.find(sub);
                    if (at == subIndex.end()) continue;
                    anim::StateMachineDef& subMachine = out.subMachines[at->second];
                    subMachine.exitTransitions = transitions(pair["second"], id, where + ".sub_state_machines[" + subMachine.name + "].exit");
                }
            }
            const int64_t defaultId = readRef(body, "m_DefaultState").fileID;
            auto d = names.find(defaultId);
            if (d != names.end()) out.defaultState = d->second;
        }

        bool read(std::string& error) {
            const Document* doc = nullptr;
            for (const Document& d : file.documents) {
                if (d.typeName == "AnimatorController") doc = &d;
            }
            if (!doc || !doc->body || !doc->body.IsMap()) {
                error = "no AnimatorController in the file (a Unity .controller holds one AnimatorController document)";
                return false;
            }
            const YAML::Node body = doc->body;
            const YAML::Node parameters = body["m_AnimatorParameters"];
            if (parameters && parameters.IsSequence()) {
                for (const YAML::Node& p : parameters) {
                    anim::ParameterDef parameter;
                    parameter.name = readString(p, "m_Name");
                    switch (readInt(p, "m_Type", 1)) {
                        case 3:
                            parameter.type = AnimatorParameterType::Int;
                            parameter.defaultValue = (float)readInt(p, "m_DefaultInt");
                            break;
                        case 4:
                            parameter.type = AnimatorParameterType::Bool;
                            parameter.defaultValue = readBool(p, "m_DefaultBool") ? 1.0f : 0.0f;
                            break;
                        case 9:
                            parameter.type = AnimatorParameterType::Trigger;
                            break;
                        default:
                            parameter.type = AnimatorParameterType::Float;
                            parameter.defaultValue = readFloat(p, "m_DefaultFloat");
                            break;
                    }
                    def.parameters.push_back(parameter);
                }
            }
            const YAML::Node layers = body["m_AnimatorLayers"];
            if (layers && layers.IsSequence()) {
                std::set<std::string> layerNames;
                for (const YAML::Node& l : layers) {
                    anim::LayerDef layer;
                    std::string name = readString(l, "m_Name");
                    if (name.empty()) name = "Layer";
                    layer.name = name;
                    for (int n = 2; layerNames.count(layer.name); n++) layer.name = name + " " + std::to_string(n);
                    layerNames.insert(layer.name);
                    const std::string where = "layers[" + layer.name + "]";
                    layer.weight = def.layers.empty() ? 1.0f : std::min(1.0f, std::max(0.0f, readFloat(l, "m_DefaultWeight", 1.0f)));
                    layer.blending = readInt(l, "m_BlendingMode") == 1 ? AnimatorLayerBlending::Additive : AnimatorLayerBlending::Override;
                    if (readInt(l, "m_SyncedLayerIndex", -1) >= 0) {
                        session.miss(where, "a synced layer (it plays another layer's states with other motions) is not supported: imported without states");
                    }
                    if (readBool(l, "m_IKPass")) session.note("ik-pass", where + ": the IK Pass (OnAnimatorIK) has no equivalent: use the Animator's IK rigs");
                    const Ref maskRef = readRef(l, "m_Mask");
                    if (!maskRef.isNull()) maskLayer(maskRef, layer.mask, where + ".mask");
                    const int64_t root = readRef(l, "m_StateMachine").fileID;
                    std::map<int64_t, std::string> names;
                    paths.clear();
                    owner.clear();
                    collectNames(root, "", names);
                    machine(root, layer.machine, where, names);
                    def.layers.push_back(std::move(layer));
                }
            }
            return true;
        }

        void maskLayer(const Ref& ref, anim::AnimatorAssetRef& out, const std::string& where) {
            auto it = session.imported.find(ref.guid);
            if (it != session.imported.end()) {
                out = it->second;
                return;
            }
            const fs::path asset = session.hasUnity && !ref.guid.empty() ? session.unity.pathOf(ref.guid) : fs::path();
            if (asset.empty() || extensionLower(asset) != ".mask") {
                session.miss(where, ref.guid.empty() ? "the layer's mask is not a .mask file" : "the mask's GUID " + ref.guid + " is not in the Unity project");
                return;
            }
            const std::string unityPath = unityRelative(&session.unity, asset);
            YamlFile maskFile;
            std::string error;
            anim::AvatarMaskDef mask;
            Json report;
            if (!maskFile.load(asset, error) || !maskFromYaml(maskFile, unityPath, mask, report, error)) {
                session.miss(where, unityPath + ": " + error);
                return;
            }
            bool reuse = false;
            const fs::path path = session.dependencyPath(asset, [&](const fs::path& existing) { return sameMaskSource(existing, unityPath); }, reuse);
            out.path = assetsRelativeRef(session.options.engineAssets, path);
            if (reuse) {
                bool ok = false;
                out.guid = anim::readAnimatorAssetGuid(readImportText(path, ok));
            } else {
                mask.guid = anim::generateClipGuid();
                out.guid = mask.guid;
                session.result.files.push_back({path, anim::writeMask(mask)});
                session.planned.insert(genericImportPath(path));
            }
            session.masks.push_back({{"unity", unityPath}, {"path", out.path}, {"reused", reuse}, {"notes", report["notes"]}});
            session.imported[ref.guid] = out;
        }
    };

}

bool jasperengine::editor::unity::isUnityAnimationAsset(const fs::path& file) {
    const std::string ext = extensionLower(file);
    if (ext != ".anim" && ext != ".controller" && ext != ".mask") return false;
    std::ifstream in(file, std::ios::binary);
    if (!in) return false;
    // the header and the first document are enough
    std::string head(4096, '\0');
    in.read(&head[0], (std::streamsize)head.size());
    head.resize((size_t)in.gcount());
    if (ext == ".anim") return anim::isUnityYamlClip(head);
    if (ext == ".controller") return anim::isUnityYamlController(head);
    return anim::isUnityYamlMask(head);
}

AnimImportResult jasperengine::editor::unity::importUnityClip(const fs::path& file, bool mirrorX) {
    jasperengine::editor::unity::Project unity;
    std::string error;
    const fs::path root = jasperengine::editor::unity::Project::findRoot(file);
    const bool hasUnity = !root.empty() && unity.open(root, error);
    AnimImportOptions options;
    options.mirrorX = mirrorX;
    options.source = unityRelative(hasUnity ? &unity : nullptr, file);
    AnimImportResult result = importAnimFile(file, options);
    if (result.ok) {
        result.clip.source.kind = "unity";
        result.clip.source.path = options.source;
        result.clip.source.name = result.report.value("name", std::string());
    }
    return result;
}

fs::path jasperengine::editor::unity::freeImportPath(const fs::path& folder, const fs::path& file) {
    const std::string stem = file.stem().string();
    const std::string ext = extensionLower(file);
    for (int n = 1; n < 10000; n++) {
        const fs::path candidate = folder / (n == 1 ? stem + ext : stem + " " + std::to_string(n) + ext);
        std::error_code ec;
        if (!fs::exists(candidate, ec)) return candidate;
    }
    return folder / (stem + ext);
}

AssetImportResult jasperengine::editor::unity::importUnityAnimationAsset(const AssetImportOptions& options) {
    AssetImportResult result;
    const std::string ext = extensionLower(options.file);
    if (ext != ".anim" && ext != ".controller" && ext != ".mask") {
        result.error = "not a Unity animation asset (.anim, .controller or .mask): " + options.file.string();
        return result;
    }
    if (extensionLower(options.dest) != ext) {
        result.error = "the destination must be a " + ext + " file like the source: " + options.dest.filename().string();
        return result;
    }
    if (!isUnityAnimationAsset(options.file)) {
        std::error_code ec;
        result.error = fs::exists(options.file, ec) ? options.file.filename().string() + " is not a Unity YAML " + ext + " (it is already an engine asset, or another file)"
                                                    : "cannot read " + options.file.string();
        return result;
    }
    std::error_code ec;
    if (fs::exists(options.dest, ec)) {
        result.error = options.dest.filename().string() + " already exists: choose another destination";
        return result;
    }
    ImportSession session(options, result);
    const fs::path root = options.unityProject.empty() ? jasperengine::editor::unity::Project::findRoot(options.file) : options.unityProject;
    std::string error;
    session.hasUnity = !root.empty() && session.unity.open(root, error);
    const std::string unityPath = unityRelative(session.hasUnity ? &session.unity : nullptr, options.file);
    session.planned.insert(genericImportPath(options.dest));
    const std::string destRelative = assetsRelativeRef(options.engineAssets, options.dest);

    if (ext == ".anim") {
        AnimImportResult clip = importUnityClip(options.file, options.mirrorX);
        if (!clip.ok) {
            result.error = clip.error;
            return result;
        }
        clip.clip.guid = anim::generateClipGuid();
        result.files.push_back({options.dest, anim::writeClip(clip.clip)});
        result.report = clip.report;
        result.report["kind"] = "clip";
        result.report["path"] = destRelative;
        result.ok = true;
        return result;
    }

    YamlFile file;
    if (!file.load(options.file, error)) {
        result.error = error;
        return result;
    }
    if (ext == ".mask") {
        anim::AvatarMaskDef mask;
        Json report;
        if (!maskFromYaml(file, unityPath, mask, report, error)) {
            result.error = error;
            return result;
        }
        mask.guid = anim::generateClipGuid();
        result.files.push_back({options.dest, anim::writeMask(mask)});
        result.report = report;
        result.report["source"] = unityPath;
        result.report["kind"] = "mask";
        result.report["path"] = destRelative;
        result.ok = true;
        return result;
    }

    ControllerReader reader(session, file);
    if (!reader.read(error)) {
        result.error = error;
        return result;
    }
    std::vector<ClipWarning> warnings;
    anim::normalizeController(reader.def, &warnings);
    for (const ClipWarning& w : warnings) session.note("normalized", w.path + ": " + w.message);
    reader.def.guid = anim::generateClipGuid();
    reader.def.unknownKeys.push_back({"source", Json{{"kind", "unity"}, {"path", unityPath}}.dump()});
    result.files.insert(result.files.begin(), {options.dest, anim::writeController(reader.def)});
    result.report = {{"source", unityPath}, {"kind", "controller"}, {"path", destRelative}, {"parameters", reader.def.parameters.size()},
                     {"layers", reader.def.layers.size()}, {"states", reader.states}, {"clips", session.clips}, {"masks", session.masks},
                     {"unmapped", session.unmapped}, {"notes", session.notes}};
    result.ok = true;
    return result;
}

AssetImportResult jasperengine::editor::unity::importUnityAnimationIntoProject(jasperengine::editor::Project* project, AssetImportOptions options) {
    if (!project || project->getProjectPath().empty()) {
        AssetImportResult result;
        result.error = "no project is open";
        return result;
    }
    options.engineAssets = project->getAssetsPath();
    AssetImportResult result = importUnityAnimationAsset(options);
    if (!result.ok) return result;
    std::vector<WriteFilesCmd::Item> items;
    for (const auto& [path, bytes] : result.files) items.push_back({path, bytes});
    std::string error;
    if (!ProjectFileWrites::writeUndoable(project, std::move(items), error, nullptr, nullptr, project->getProjectPath(), false,
                                          "Import Unity Animation " + options.file.filename().string())) {
        result.ok = false;
        result.error = error.empty() ? std::string("the files could not be written") : error;
    }
    return result;
}
