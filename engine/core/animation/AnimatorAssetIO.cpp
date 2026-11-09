// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AnimatorAssetIO.h"

#include "animation/AnimJson.h"
#include "animation/CompiledController.h"
#include "animation/AnimationClipIO.h"
#include "animation/UpstreamAnimFormats.h"
#include "json.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <type_traits>
#include <variant>

using namespace jasperengine;
using namespace jasperengine::anim;

namespace{

    using OJson = nlohmann::ordered_json;

    // ---------------------------------------------------------------- name tables

    template<class E>
    struct NameEntry{
        E value;
        const char* name;
    };

    constexpr NameEntry<AnimatorParameterType> kParameterTypes[] = {
        {AnimatorParameterType::Float, "float"}, {AnimatorParameterType::Int, "int"},
        {AnimatorParameterType::Bool, "bool"}, {AnimatorParameterType::Trigger, "trigger"},
    };
    constexpr NameEntry<AnimatorConditionMode> kConditionModes[] = {
        {AnimatorConditionMode::If, "if"}, {AnimatorConditionMode::IfNot, "if_not"},
        {AnimatorConditionMode::Greater, "greater"}, {AnimatorConditionMode::Less, "less"},
        {AnimatorConditionMode::Equals, "equals"}, {AnimatorConditionMode::NotEqual, "not_equal"},
    };
    constexpr NameEntry<AnimatorLayerBlending> kBlendings[] = {
        {AnimatorLayerBlending::Override, "override"}, {AnimatorLayerBlending::Additive, "additive"},
    };
    constexpr NameEntry<BlendTreeType> kTreeTypes[] = {
        {BlendTreeType::Simple1D, "simple_1d"}, {BlendTreeType::SimpleDirectional2D, "simple_directional"},
        {BlendTreeType::FreeformDirectional2D, "freeform_directional"}, {BlendTreeType::FreeformCartesian2D, "freeform_cartesian"},
        {BlendTreeType::Direct, "direct"},
    };
    constexpr NameEntry<TransitionInterruptionSource> kSources[] = {
        {TransitionInterruptionSource::None, "none"}, {TransitionInterruptionSource::Current, "current"},
        {TransitionInterruptionSource::Next, "next"}, {TransitionInterruptionSource::CurrentThenNext, "current_then_next"},
        {TransitionInterruptionSource::NextThenCurrent, "next_then_current"},
    };
    constexpr NameEntry<AnimatorStateLoop> kLoops[] = {
        {AnimatorStateLoop::Inherit, "inherit"}, {AnimatorStateLoop::Loop, "loop"}, {AnimatorStateLoop::Once, "once"},
        {AnimatorStateLoop::PingPong, "ping_pong"}, {AnimatorStateLoop::ClampForever, "clamp_forever"}, {AnimatorStateLoop::Repeat, "repeat"},
    };
    constexpr NameEntry<AdditiveReference> kReferences[] = {
        {AdditiveReference::FirstFrame, "first_frame"}, {AdditiveReference::Rest, "rest"}, {AdditiveReference::Time, "time"},
        {AdditiveReference::Clip, "clip"},
    };
    constexpr NameEntry<AvatarBodyPart> kBodyParts[] = {
        {AvatarBodyPart::Root, "root"}, {AvatarBodyPart::Body, "body"}, {AvatarBodyPart::Head, "head"},
        {AvatarBodyPart::LeftArm, "left_arm"}, {AvatarBodyPart::RightArm, "right_arm"},
        {AvatarBodyPart::LeftHand, "left_hand"}, {AvatarBodyPart::RightHand, "right_hand"},
        {AvatarBodyPart::LeftLeg, "left_leg"}, {AvatarBodyPart::RightLeg, "right_leg"},
        {AvatarBodyPart::LeftFingers, "left_fingers"}, {AvatarBodyPart::RightFingers, "right_fingers"},
    };
    constexpr NameEntry<AnimatorIKType> kIKTypes[] = {
        {AnimatorIKType::TwoBone, "two_bone"}, {AnimatorIKType::Fabrik, "fabrik"}, {AnimatorIKType::LookAt, "look_at"},
        {AnimatorIKType::FootIK, "foot_ik"}, {AnimatorIKType::FootLock, "foot_lock"},
    };

    template<class E, size_t N>
    const char* nameOf(const NameEntry<E> (&table)[N], E value){
        for (const NameEntry<E>& entry : table){
            if (entry.value == value) return entry.name;
        }
        return table[0].name;
    }

    template<class E, size_t N>
    bool valueOf(const NameEntry<E> (&table)[N], const std::string& name, E& out){
        for (const NameEntry<E>& entry : table){
            if (name == entry.name){
                out = entry.value;
                return true;
            }
        }
        return false;
    }

    template<class E, size_t N>
    std::string namesOf(const NameEntry<E> (&table)[N]){
        std::string text;
        for (const NameEntry<E>& entry : table){
            if (!text.empty()) text += ", ";
            text += entry.name;
        }
        return text;
    }

    // ---------------------------------------------------------------- parsing

    class SaxParser : public nlohmann::detail::json_sax_dom_parser<OJson>{
    public:
        explicit SaxParser(OJson& root): json_sax_dom_parser<OJson>(root, false){}

        template<class Exception>
        bool parse_error(std::size_t position, const std::string& lastToken, const Exception& ex){
            failed = true;
            errorPosition = position;
            errorToken = lastToken;
            message = ex.what();
            return json_sax_dom_parser<OJson>::parse_error(position, lastToken, ex);
        }

        bool failed = false;
        std::size_t errorPosition = 0;
        std::string errorToken;
        std::string message;
    };

    void textPosition(const std::string& text, size_t index, int& line, int& column){
        if (index > text.size()) index = text.size();
        line = 1;
        size_t lineStart = text.compare(0, 3, "\xEF\xBB\xBF") == 0 ? std::min<size_t>(3, index) : 0;
        for (size_t i = lineStart; i < index; i++){
            if (text[i] == '\n'){
                line++;
                lineStart = i + 1;
            }
        }
        column = 1;
        for (size_t i = lineStart; i < index; i++){
            if (((unsigned char)text[i] & 0xC0) != 0x80) column++;
        }
    }

    std::string shortNumber(double value){
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "%g", value);
        return buffer;
    }

    // The JSON object of `text` after the format/version checks, or false with `error`
    bool parseRoot(const std::string& text, const char* format, const char* what, int newest, OJson& root, int& versionRead, AnimatorParseError& error){
        SaxParser sax(root);
        OJson::sax_parse(text, &sax);
        if (sax.failed){
            textPosition(text, sax.errorPosition > 0 ? sax.errorPosition - 1 : 0, error.line, error.column);
            const size_t at = sax.message.find("syntax error");
            error.message = "not valid JSON: " + (at != std::string::npos ? sax.message.substr(at) : sax.message);
            return false;
        }
        if (!root.is_object()){
            error.message = std::string("not ") + what + ": the top level must be a JSON object";
            return false;
        }
        auto formatIt = root.find("format");
        if (formatIt == root.end() || !formatIt->is_string() || formatIt->get<std::string>() != format){
            error.message = std::string("not ") + what + ": \"format\" must be \"" + format + "\"";
            return false;
        }
        auto version = root.find("version");
        if (version == root.end()){
            error.message = "missing \"version\" (" + std::to_string(newest) + ")";
            return false;
        }
        if (!version->is_number() || version->get<double>() != std::floor(version->get<double>()) || version->get<double>() < 1.0){
            error.message = "invalid \"version\": expected a whole number from 1";
            return false;
        }
        if (version->get<double>() > newest){
            error.message = "version " + shortNumber(version->get<double>()) + " was made by a newer JasperEngine (this one reads up to " +
                            std::to_string(newest) + ")";
            return false;
        }
        versionRead = (int)version->get<double>();
        return true;
    }

    // Reads members of one JSON object with warnings carrying their JSON path
    class Reader{
    public:
        Reader(std::vector<ClipWarning>& warnings): warnings(warnings){}

        void warn(const std::string& path, const std::string& message){
            warnings.push_back({path, message});
        }

        static std::string member(const std::string& path, const std::string& name){
            return path.empty() ? name : path + "." + name;
        }
        static std::string index(const std::string& path, size_t i){
            return path + "[" + std::to_string(i) + "]";
        }

        bool number(const OJson& object, const std::string& path, const char* name, float& out){
            auto it = object.find(name);
            if (it == object.end() || it->is_null()) return false;
            if (!it->is_number() || !std::isfinite(it->get<double>())){
                warn(member(path, name), "expected a number");
                return false;
            }
            out = (float)it->get<double>();
            return true;
        }

        bool boolean(const OJson& object, const std::string& path, const char* name, bool& out){
            auto it = object.find(name);
            if (it == object.end() || it->is_null()) return false;
            if (!it->is_boolean()){
                warn(member(path, name), "expected true or false");
                return false;
            }
            out = it->get<bool>();
            return true;
        }

        bool string(const OJson& object, const std::string& path, const char* name, std::string& out){
            auto it = object.find(name);
            if (it == object.end() || it->is_null()) return false;
            if (!it->is_string()){
                warn(member(path, name), "expected a string");
                return false;
            }
            out = it->get<std::string>();
            return true;
        }

        bool vector2(const OJson& object, const std::string& path, const char* name, Vector2& out){
            auto it = object.find(name);
            if (it == object.end() || it->is_null()) return false;
            if (!it->is_array() || it->size() != 2 || !(*it)[0].is_number() || !(*it)[1].is_number() ||
                !std::isfinite((*it)[0].get<double>()) || !std::isfinite((*it)[1].get<double>())){
                warn(member(path, name), "expected [x, y]");
                return false;
            }
            out = Vector2((float)(*it)[0].get<double>(), (float)(*it)[1].get<double>());
            return true;
        }

        template<class E, size_t N>
        bool enumeration(const OJson& object, const std::string& path, const char* name, const NameEntry<E> (&table)[N], E& out){
            std::string text;
            if (!string(object, path, name, text)) return false;
            if (!valueOf(table, text, out)){
                warn(member(path, name), "unknown value \"" + text + "\" (one of " + namesOf(table) + ")");
                return false;
            }
            return true;
        }

        // an array member: empty with a warning when it is not an array
        const OJson* array(const OJson& object, const std::string& path, const char* name){
            auto it = object.find(name);
            if (it == object.end() || it->is_null()) return nullptr;
            if (!it->is_array()){
                warn(member(path, name), "expected an array");
                return nullptr;
            }
            return &*it;
        }

        bool assetRef(const OJson& object, const std::string& path, const char* name, AnimatorAssetRef& out){
            auto it = object.find(name);
            if (it == object.end() || it->is_null()) return false;
            if (it->is_string()){
                out.path = it->get<std::string>();
                out.guid.clear();
                return true;
            }
            if (!it->is_object()){
                warn(member(path, name), "expected {\"path\", \"guid\"}");
                return false;
            }
            const std::string at = member(path, name);
            out = AnimatorAssetRef();
            string(*it, at, "path", out.path);
            string(*it, at, "guid", out.guid);
            if (!out.guid.empty() && !isClipGuid(out.guid)){
                warn(member(at, "guid"), "expected 32 lowercase hex digits: dropped");
                out.guid.clear();
            }
            return !out.empty();
        }

        // unknown members of an object: kept as compact JSON (written back), with a warning
        void keep(const OJson& object, const std::string& path, std::initializer_list<const char*> known, AnimatorUnknownKeys& out){
            std::string names;
            for (auto it = object.begin(); it != object.end(); ++it){
                bool found = false;
                for (const char* name : known){
                    if (it.key() == name){
                        found = true;
                        break;
                    }
                }
                if (found) continue;
                out.push_back({it.key(), it->dump(-1, ' ', false, OJson::error_handler_t::replace)});
                names += (names.empty() ? "" : ", ") + it.key();
            }
            if (!names.empty()) warn(path, "unknown keys kept as they are (written back on save): " + names);
        }

        // unknown members of an object, warned about (not kept: a motion's wrapper object)
        void unknown(const OJson& object, const std::string& path, std::initializer_list<const char*> known){
            for (auto it = object.begin(); it != object.end(); ++it){
                bool found = false;
                for (const char* name : known){
                    if (it.key() == name){
                        found = true;
                        break;
                    }
                }
                if (!found) warn(member(path, it.key()), "unknown key ignored");
            }
        }

        std::vector<ClipWarning>& warnings;
    };

    bool readEventArgs(Reader& reader, const OJson& object, const std::string& path, std::vector<AnimationEventArg>& out){
        const OJson* args = reader.array(object, path, "args");
        if (!args) return true;
        for (size_t i = 0; i < args->size(); i++){
            const OJson& arg = (*args)[i];
            if (out.size() >= MAX_ANIMATION_EVENT_ARGS){
                reader.warn(Reader::member(path, "args"), "more than " + std::to_string(MAX_ANIMATION_EVENT_ARGS) + " args: the rest dropped");
                break;
            }
            if (arg.is_boolean()) out.push_back(AnimationEvent::boolArg(arg.get<bool>()));
            else if (arg.is_string()) out.push_back(AnimationEvent::stringArg(arg.get<std::string>()));
            else if (arg.is_number() && std::isfinite(arg.get<double>())) out.push_back(AnimationEvent::numberArg(arg.get<double>()));
            else reader.warn(Reader::index(Reader::member(path, "args"), i), "expected a number, string or bool: dropped");
        }
        return true;
    }

    // ---------------------------------------------------------------- writing

    std::string quote(const std::string& text){
        return OJson(text).dump(-1, ' ', false, OJson::error_handler_t::replace);
    }

    std::string number(double value){
        if (!std::isfinite(value)) return "0";
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "%.7g", value);
        std::string text = buffer;
        return text == "-0" ? "0" : text;
    }

    std::string argNumber(double value){
        if (!std::isfinite(value)) return "0";
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "%.15g", value);
        std::string text = buffer;
        return text == "-0" ? "0" : text;
    }

    // A canonical writer: members in insertion order, one per line; arrays of scalars inline
    class Writer{
    public:
        std::string text;

        void open(char bracket){
            text += bracket;
            first.push_back(true);
        }
        void close(char bracket){
            const bool empty = first.back();
            first.pop_back();
            if (!empty){
                text += '\n';
                indent();
            }
            text += bracket;
        }
        // a member key (inside an object)
        void key(const char* name){
            separator();
            text += quote(name);
            text += ": ";
        }
        // an element (inside an array)
        void element(){
            separator();
        }
        void raw(const std::string& value){ text += value; }
        void str(const std::string& value){ text += quote(value); }
        void num(double value){ text += number(value); }
        void boolean(bool value){ text += value ? "true" : "false"; }
        void null(){ text += "null"; }
        void vec2(const Vector2& value){ text += "[" + number(value.x) + ", " + number(value.y) + "]"; }

    private:
        std::vector<bool> first;

        void indent(){
            text.append(first.size() * 2, ' ');
        }
        void separator(){
            if (!first.back()) text += ',';
            first.back() = false;
            text += '\n';
            indent();
        }
    };

    void writeHeader(Writer& out, const char* format, int version, const std::string& guid){
        out.key("format");
        out.str(format);
        out.key("version");
        out.raw(std::to_string(version));
        if (!guid.empty()){
            out.key("guid");
            out.str(guid);
        }
    }

    void writeUnknown(Writer& out, const AnimatorUnknownKeys& unknownKeys){
        for (const auto& entry : unknownKeys){
            out.key(entry.first.c_str());
            out.raw(entry.second.empty() ? std::string("null") : entry.second);
        }
    }

    // the unknown members of an object written inline (", \"key\": value...")
    std::string unknownInline(const AnimatorUnknownKeys& unknownKeys){
        std::string text;
        for (const auto& entry : unknownKeys){
            text += ", " + quote(entry.first) + ": " + (entry.second.empty() ? std::string("null") : entry.second);
        }
        return text;
    }

    void writeRef(Writer& out, const AnimatorAssetRef& ref){
        if (ref.empty()){
            out.null();
            return;
        }
        out.raw("{\"path\": " + quote(ref.path));
        if (!ref.guid.empty()) out.raw(", \"guid\": " + quote(ref.guid));
        out.raw("}");
    }

    void writeArgs(Writer& out, const std::vector<AnimationEventArg>& args){
        std::string text = "[";
        for (size_t i = 0; i < args.size(); i++){
            if (i > 0) text += ", ";
            const AnimationEventArg& arg = args[i];
            if (std::holds_alternative<bool>(arg)) text += std::get<bool>(arg) ? "true" : "false";
            else if (std::holds_alternative<std::string>(arg)) text += quote(std::get<std::string>(arg));
            else text += argNumber(std::get<double>(arg));
        }
        out.raw(text + "]");
    }

    void keepUnknownTopLevel(const OJson& root, std::initializer_list<const char*> known,
                             std::vector<std::pair<std::string, std::string>>& unknownKeys, std::vector<ClipWarning>& warnings){
        std::string names;
        for (auto it = root.begin(); it != root.end(); ++it){
            bool found = false;
            for (const char* name : known){
                if (it.key() == name){
                    found = true;
                    break;
                }
            }
            if (found) continue;
            unknownKeys.push_back({it.key(), it->dump(-1, ' ', false, OJson::error_handler_t::replace)});
            names += (names.empty() ? "" : ", ") + it.key();
        }
        if (!names.empty()) warnings.push_back({"", "unknown keys kept as they are (written back on save): " + names});
    }

    void readGuid(Reader& reader, const OJson& root, std::string& out){
        std::string guid;
        if (reader.string(root, "", "guid", guid)){
            if (isClipGuid(guid)) out = guid;
            else reader.warn("guid", "expected 32 lowercase hex digits: dropped (a new one is given on save)");
        }
    }

    // ---------------------------------------------------------------- controller: read

    void readMotion(Reader& reader, const OJson& object, const std::string& path, MotionDef& out, int depth);

    void readTransition(Reader& reader, const OJson& value, const std::string& path, TransitionDef& out){
        reader.string(value, path, "to", out.to);
        if (const OJson* conditions = reader.array(value, path, "conditions")){
            for (size_t i = 0; i < conditions->size(); i++){
                const OJson& c = (*conditions)[i];
                const std::string at = Reader::index(Reader::member(path, "conditions"), i);
                if (!c.is_object()){
                    reader.warn(at, "expected an object: dropped");
                    continue;
                }
                ConditionDef condition;
                if (!reader.string(c, at, "parameter", condition.parameter) || condition.parameter.empty()){
                    reader.warn(at, "a condition needs a parameter: dropped");
                    continue;
                }
                reader.enumeration(c, at, "mode", kConditionModes, condition.mode);
                reader.number(c, at, "threshold", condition.threshold);
                reader.keep(c, at, {"parameter", "mode", "threshold"}, condition.unknownKeys);
                out.conditions.push_back(condition);
            }
        }
        reader.boolean(value, path, "has_exit_time", out.hasExitTime);
        reader.number(value, path, "exit_time", out.exitTime);
        reader.boolean(value, path, "fixed_duration", out.fixedDuration);
        reader.number(value, path, "duration", out.duration);
        reader.number(value, path, "offset", out.offset);
        reader.enumeration(value, path, "interruption_source", kSources, out.interruptionSource);
        reader.boolean(value, path, "ordered_interruption", out.orderedInterruption);
        reader.boolean(value, path, "can_transition_to_self", out.canTransitionToSelf);
        reader.boolean(value, path, "mute", out.mute);
        reader.keep(value, path, {"to", "conditions", "has_exit_time", "exit_time", "fixed_duration", "duration", "offset",
                                  "interruption_source", "ordered_interruption", "can_transition_to_self", "mute"}, out.unknownKeys);
    }

    void readTransitions(Reader& reader, const OJson& object, const std::string& path, const char* name, std::vector<TransitionDef>& out){
        const OJson* list = reader.array(object, path, name);
        if (!list) return;
        for (size_t i = 0; i < list->size(); i++){
            const std::string at = Reader::index(Reader::member(path, name), i);
            if (!(*list)[i].is_object()){
                reader.warn(at, "expected an object: dropped");
                continue;
            }
            TransitionDef transition;
            readTransition(reader, (*list)[i], at, transition);
            if (transition.to.empty()){
                reader.warn(at, "a transition needs \"to\": dropped");
                continue;
            }
            out.push_back(std::move(transition));
        }
    }

    void readTree(Reader& reader, const OJson& value, const std::string& path, BlendTreeDef& out, int depth){
        reader.enumeration(value, path, "type", kTreeTypes, out.type);
        reader.string(value, path, "parameter", out.parameter);
        reader.string(value, path, "parameter_y", out.parameterY);
        if (const OJson* children = reader.array(value, path, "children")){
            for (size_t i = 0; i < children->size(); i++){
                const std::string at = Reader::index(Reader::member(path, "children"), i);
                const OJson& c = (*children)[i];
                if (!c.is_object()){
                    reader.warn(at, "expected an object: dropped");
                    continue;
                }
                BlendChildDef child;
                readMotion(reader, c, at, child.motion, depth + 1);
                reader.number(c, at, "threshold", child.threshold);
                reader.vector2(c, at, "position", child.position);
                reader.number(c, at, "time_scale", child.timeScale);
                reader.string(c, at, "direct_parameter", child.directParameter);
                reader.boolean(c, at, "mirror", child.mirror);
                reader.keep(c, at, {"motion", "threshold", "position", "time_scale", "direct_parameter", "mirror"}, child.unknownKeys);
                out.children.push_back(std::move(child));
            }
        }
        reader.keep(value, path, {"type", "parameter", "parameter_y", "children"}, out.unknownKeys);
    }

    void readMotion(Reader& reader, const OJson& object, const std::string& path, MotionDef& out, int depth){
        auto it = object.find("motion");
        if (it == object.end() || it->is_null()) return;
        const std::string at = Reader::member(path, "motion");
        if (!it->is_object()){
            reader.warn(at, "expected null, {\"clip\"} or {\"blend_tree\"}");
            return;
        }
        if (it->contains("clip")){
            reader.assetRef(*it, at, "clip", out.clip);
        }else if (it->contains("blend_tree")){
            const OJson& tree = (*it)["blend_tree"];
            const std::string treeAt = Reader::member(at, "blend_tree");
            if (!tree.is_object()){
                reader.warn(treeAt, "expected an object");
                return;
            }
            if (depth >= 16){
                reader.warn(treeAt, "blend trees nest at most 16 deep: dropped");
                return;
            }
            out.tree.emplace_back();
            readTree(reader, tree, treeAt, out.tree.back(), depth);
        }
        reader.unknown(*it, at, {"clip", "blend_tree"});
    }

    void readState(Reader& reader, const OJson& value, const std::string& path, StateDef& out){
        reader.string(value, path, "name", out.name);
        reader.vector2(value, path, "position", out.position);
        readMotion(reader, value, path, out.motion, 0);
        reader.number(value, path, "speed", out.speed);
        reader.string(value, path, "speed_parameter", out.speedParameter);
        reader.number(value, path, "cycle_offset", out.cycleOffset);
        reader.string(value, path, "cycle_offset_parameter", out.cycleOffsetParameter);
        reader.string(value, path, "time_parameter", out.timeParameter);
        reader.enumeration(value, path, "loop", kLoops, out.loop);
        reader.string(value, path, "tag", out.tag);
        reader.enumeration(value, path, "additive_reference", kReferences, out.additiveReference);
        reader.number(value, path, "additive_time", out.additiveTime);
        reader.boolean(value, path, "mirror", out.mirror);
        reader.boolean(value, path, "foot_ik", out.footIK);
        reader.boolean(value, path, "retarget", out.retarget);
        readTransitions(reader, value, path, "transitions", out.transitions);
        reader.keep(value, path, {"name", "position", "motion", "speed", "speed_parameter", "cycle_offset", "cycle_offset_parameter",
                                  "time_parameter", "loop", "tag", "additive_reference", "additive_time", "mirror", "foot_ik", "retarget",
                                  "transitions"}, out.unknownKeys);
    }

    void readMachine(Reader& reader, const OJson& value, const std::string& path, StateMachineDef& out, int depth){
        reader.string(value, path, "name", out.name);
        reader.vector2(value, path, "position", out.position);
        reader.string(value, path, "default_state", out.defaultState);
        readTransitions(reader, value, path, "entry", out.entry);
        readTransitions(reader, value, path, "any_state", out.anyState);
        if (const OJson* states = reader.array(value, path, "states")){
            for (size_t i = 0; i < states->size(); i++){
                const std::string at = Reader::index(Reader::member(path, "states"), i);
                if (!(*states)[i].is_object()){
                    reader.warn(at, "expected an object: dropped");
                    continue;
                }
                StateDef state;
                readState(reader, (*states)[i], at, state);
                out.states.push_back(std::move(state));
            }
        }
        if (const OJson* machines = reader.array(value, path, "sub_state_machines")){
            for (size_t i = 0; i < machines->size(); i++){
                const std::string at = Reader::index(Reader::member(path, "sub_state_machines"), i);
                if (!(*machines)[i].is_object()){
                    reader.warn(at, "expected an object: dropped");
                    continue;
                }
                if (depth >= 16){
                    reader.warn(at, "state machines nest at most 16 deep: dropped");
                    continue;
                }
                StateMachineDef machine;
                readMachine(reader, (*machines)[i], at, machine, depth + 1);
                out.subMachines.push_back(std::move(machine));
            }
        }
        readTransitions(reader, value, path, "exit_transitions", out.exitTransitions);
        reader.vector2(value, path, "entry_position", out.entryPosition);
        reader.vector2(value, path, "any_state_position", out.anyStatePosition);
        reader.vector2(value, path, "exit_position", out.exitPosition);
        reader.keep(value, path, {"name", "position", "default_state", "entry", "any_state", "states", "sub_state_machines",
                                  "exit_transitions", "entry_position", "any_state_position", "exit_position"}, out.unknownKeys);
    }

    // ---------------------------------------------------------------- controller: write

    void writeMotion(Writer& out, const MotionDef& motion);

    void writeTransitionList(Writer& out, const std::vector<TransitionDef>& list){
        out.open('[');
        for (const TransitionDef& t : list){
            out.element();
            out.open('{');
            out.key("to"); out.str(t.to);
            out.key("conditions");
            out.open('[');
            for (const ConditionDef& c : t.conditions){
                out.element();
                std::string text = "{\"parameter\": " + quote(c.parameter) + ", \"mode\": " + quote(conditionModeName(c.mode));
                if (c.mode != AnimatorConditionMode::If && c.mode != AnimatorConditionMode::IfNot){
                    text += ", \"threshold\": " + number(c.threshold);
                }
                out.raw(text + unknownInline(c.unknownKeys) + "}");
            }
            out.close(']');
            out.key("has_exit_time"); out.boolean(t.hasExitTime);
            out.key("exit_time"); out.num(t.exitTime);
            out.key("fixed_duration"); out.boolean(t.fixedDuration);
            out.key("duration"); out.num(t.duration);
            out.key("offset"); out.num(t.offset);
            out.key("interruption_source"); out.str(interruptionSourceName(t.interruptionSource));
            out.key("ordered_interruption"); out.boolean(t.orderedInterruption);
            if (t.canTransitionToSelf){
                out.key("can_transition_to_self"); out.boolean(true);
            }
            if (t.mute){
                out.key("mute"); out.boolean(true);
            }
            writeUnknown(out, t.unknownKeys);
            out.close('}');
        }
        out.close(']');
    }

    void writeTree(Writer& out, const BlendTreeDef& tree){
        out.open('{');
        out.key("type"); out.str(blendTreeTypeName(tree.type));
        out.key("parameter"); out.str(tree.parameter);
        const bool twoD = tree.type == BlendTreeType::SimpleDirectional2D || tree.type == BlendTreeType::FreeformDirectional2D ||
                          tree.type == BlendTreeType::FreeformCartesian2D;
        if (twoD){
            out.key("parameter_y"); out.str(tree.parameterY);
        }
        out.key("children");
        out.open('[');
        for (const BlendChildDef& child : tree.children){
            out.element();
            out.open('{');
            out.key("motion");
            writeMotion(out, child.motion);
            if (tree.type == BlendTreeType::Simple1D){
                out.key("threshold"); out.num(child.threshold);
            }else if (twoD){
                out.key("position"); out.vec2(child.position);
            }
            if (child.timeScale != 1.0f){
                out.key("time_scale"); out.num(child.timeScale);
            }
            if (tree.type == BlendTreeType::Direct){
                out.key("direct_parameter"); out.str(child.directParameter);
            }
            if (child.mirror){ out.key("mirror"); out.boolean(true); }
            writeUnknown(out, child.unknownKeys);
            out.close('}');
        }
        out.close(']');
        writeUnknown(out, tree.unknownKeys);
        out.close('}');
    }

    void writeMotion(Writer& out, const MotionDef& motion){
        if (motion.isTree()){
            out.open('{');
            out.key("blend_tree");
            writeTree(out, motion.tree.front());
            out.close('}');
        }else if (!motion.clip.empty()){
            out.raw("{\"clip\": ");
            writeRef(out, motion.clip);
            out.raw("}");
        }else{
            out.null();
        }
    }

    void writeMachine(Writer& out, const StateMachineDef& machine, bool root){
        out.open('{');
        if (!root){
            out.key("name"); out.str(machine.name);
            out.key("position"); out.vec2(machine.position);
        }
        out.key("default_state"); out.str(machine.defaultState);
        out.key("entry"); writeTransitionList(out, machine.entry);
        out.key("any_state"); writeTransitionList(out, machine.anyState);
        out.key("states");
        out.open('[');
        for (const StateDef& state : machine.states){
            out.element();
            out.open('{');
            out.key("name"); out.str(state.name);
            out.key("position"); out.vec2(state.position);
            out.key("motion"); writeMotion(out, state.motion);
            out.key("speed"); out.num(state.speed);
            if (!state.speedParameter.empty()){ out.key("speed_parameter"); out.str(state.speedParameter); }
            if (state.cycleOffset != 0.0f){ out.key("cycle_offset"); out.num(state.cycleOffset); }
            if (!state.cycleOffsetParameter.empty()){ out.key("cycle_offset_parameter"); out.str(state.cycleOffsetParameter); }
            if (!state.timeParameter.empty()){ out.key("time_parameter"); out.str(state.timeParameter); }
            if (state.loop != AnimatorStateLoop::Inherit){ out.key("loop"); out.str(stateLoopName(state.loop)); }
            if (!state.tag.empty()){ out.key("tag"); out.str(state.tag); }
            if (state.additiveReference != AdditiveReference::FirstFrame){
                out.key("additive_reference"); out.str(additiveReferenceName(state.additiveReference));
            }
            if (state.additiveReference == AdditiveReference::Time){ out.key("additive_time"); out.num(state.additiveTime); }
            if (state.mirror){ out.key("mirror"); out.boolean(true); }
            if (state.footIK){ out.key("foot_ik"); out.boolean(true); }
            if (state.retarget){ out.key("retarget"); out.boolean(true); }
            out.key("transitions"); writeTransitionList(out, state.transitions);
            writeUnknown(out, state.unknownKeys);
            out.close('}');
        }
        out.close(']');
        out.key("sub_state_machines");
        out.open('[');
        for (const StateMachineDef& sub : machine.subMachines){
            out.element();
            writeMachine(out, sub, false);
        }
        out.close(']');
        if (!root){
            out.key("exit_transitions"); writeTransitionList(out, machine.exitTransitions);
        }
        out.key("entry_position"); out.vec2(machine.entryPosition);
        out.key("any_state_position"); out.vec2(machine.anyStatePosition);
        out.key("exit_position"); out.vec2(machine.exitPosition);
        writeUnknown(out, machine.unknownKeys);
        out.close('}');
    }

    // ---------------------------------------------------------------- normalization

    bool reservedNodeName(const std::string& name){
        return name == "Exit" || name == "Entry" || name == "Any State";
    }

    std::string uniqueName(const std::string& wanted, const std::set<std::string>& taken){
        if (!taken.count(wanted)) return wanted;
        for (int n = 1;; n++){
            std::string candidate = wanted + " " + std::to_string(n);
            if (!taken.count(candidate)) return candidate;
        }
    }

    float clamp01(float value){
        return std::min(std::max(value, 0.0f), 1.0f);
    }

    void normalizeTransitions(std::vector<TransitionDef>& list, const std::string& path, std::vector<ClipWarning>* warnings){
        for (size_t i = 0; i < list.size(); i++){
            TransitionDef& t = list[i];
            if (!(t.duration >= 0.0f) || !std::isfinite(t.duration)){
                if (warnings) warnings->push_back({path + "[" + std::to_string(i) + "].duration", "below 0: set to 0"});
                t.duration = 0.0f;
            }
            if (!(t.exitTime >= 0.0f) || !std::isfinite(t.exitTime)) t.exitTime = 0.0f;
            if (!std::isfinite(t.offset)) t.offset = 0.0f;
            t.offset = clamp01(t.offset);
        }
    }

    void normalizeTree(BlendTreeDef& tree, const std::string& path, std::vector<ClipWarning>* warnings){
        if (tree.children.size() > MAX_BLEND_CHILDREN){
            if (warnings) warnings->push_back({path + ".children", "more than " + std::to_string(MAX_BLEND_CHILDREN) + " children: the rest dropped"});
            tree.children.resize(MAX_BLEND_CHILDREN);
        }
        for (size_t i = 0; i < tree.children.size(); i++){
            BlendChildDef& child = tree.children[i];
            if (!std::isfinite(child.timeScale)) child.timeScale = 1.0f;
            if (child.motion.isTree()){
                normalizeTree(child.motion.tree.front(), path + ".children[" + std::to_string(i) + "].motion.blend_tree", warnings);
            }
        }
        // 1D: children in threshold order (stable, Unity keeps them sorted)
        if (tree.type == BlendTreeType::Simple1D){
            std::stable_sort(tree.children.begin(), tree.children.end(),
                             [](const BlendChildDef& a, const BlendChildDef& b){ return a.threshold < b.threshold; });
        }
    }

    void normalizeMachine(StateMachineDef& machine, const std::string& path, std::vector<ClipWarning>* warnings){
        std::set<std::string> taken;
        auto claim = [&](std::string& name, const std::string& at, const char* kind){
            std::string wanted = name;
            if (wanted.empty()) wanted = kind;
            for (char& c : wanted){
                if (c == '/' || c == '.') c = '_';
            }
            if (reservedNodeName(wanted)) wanted += " " + std::string(kind);
            std::string unique = uniqueName(wanted, taken);
            if (unique != name && warnings){
                warnings->push_back({at + ".name", "\"" + name + "\" is empty, reserved, has '/' or '.', or is taken: renamed \"" + unique + "\""});
            }
            name = unique;
            taken.insert(name);
        };
        for (size_t i = 0; i < machine.states.size(); i++){
            const std::string at = path + ".states[" + std::to_string(i) + "]";
            StateDef& state = machine.states[i];
            claim(state.name, at, "State");
            if (!std::isfinite(state.speed)) state.speed = 1.0f;
            if (!std::isfinite(state.cycleOffset)) state.cycleOffset = 0.0f;
            if (!(state.additiveTime >= 0.0f)) state.additiveTime = 0.0f;
            if (state.motion.isTree()) normalizeTree(state.motion.tree.front(), at + ".motion.blend_tree", warnings);
            normalizeTransitions(state.transitions, at + ".transitions", warnings);
        }
        for (size_t i = 0; i < machine.subMachines.size(); i++){
            const std::string at = path + ".sub_state_machines[" + std::to_string(i) + "]";
            claim(machine.subMachines[i].name, at, "State Machine");
            normalizeMachine(machine.subMachines[i], at, warnings);
            normalizeTransitions(machine.subMachines[i].exitTransitions, at + ".exit_transitions", warnings);
        }
        normalizeTransitions(machine.entry, path + ".entry", warnings);
        normalizeTransitions(machine.anyState, path + ".any_state", warnings);
    }

    // ---------------------------------------------------------------- top-level member scan

    // the string value of a top-level member of a JSON object text (no full parse); "" when absent
    std::string topLevelString(const std::string& text, const char* name){
        const std::string key = std::string("\"") + name + "\"";
        size_t depth = 0;
        bool inString = false;
        for (size_t i = 0; i < text.size(); i++){
            const char c = text[i];
            if (inString){
                if (c == '\\') i++;
                else if (c == '"') inString = false;
                continue;
            }
            if (c == '{' || c == '['){
                depth++;
                continue;
            }
            if (c == '}' || c == ']'){
                if (depth > 0) depth--;
                continue;
            }
            if (c != '"') continue;
            if (depth == 1 && text.compare(i, key.size(), key) == 0){
                size_t j = i + key.size();
                while (j < text.size() && (text[j] == ' ' || text[j] == '\t' || text[j] == '\r' || text[j] == '\n')) j++;
                if (j >= text.size() || text[j] != ':') {
                    inString = true;
                    continue;
                }
                j++;
                while (j < text.size() && (text[j] == ' ' || text[j] == '\t' || text[j] == '\r' || text[j] == '\n')) j++;
                if (j >= text.size() || text[j] != '"') return std::string();
                size_t end = j + 1;
                while (end < text.size() && text[end] != '"'){
                    if (text[end] == '\\') end++;
                    end++;
                }
                return end < text.size() ? text.substr(j + 1, end - j - 1) : std::string();
            }
            inString = true;
        }
        return std::string();
    }

}

// ---------------------------------------------------------------- public: names

const char* anim::parameterTypeName(AnimatorParameterType type){ return nameOf(kParameterTypes, type); }
bool anim::parameterTypeFromName(const std::string& name, AnimatorParameterType& out){ return valueOf(kParameterTypes, name, out); }
const char* anim::conditionModeName(AnimatorConditionMode mode){ return nameOf(kConditionModes, mode); }
bool anim::conditionModeFromName(const std::string& name, AnimatorConditionMode& out){ return valueOf(kConditionModes, name, out); }
const char* anim::layerBlendingName(AnimatorLayerBlending blending){ return nameOf(kBlendings, blending); }
bool anim::layerBlendingFromName(const std::string& name, AnimatorLayerBlending& out){ return valueOf(kBlendings, name, out); }
const char* anim::blendTreeTypeName(BlendTreeType type){ return nameOf(kTreeTypes, type); }
bool anim::blendTreeTypeFromName(const std::string& name, BlendTreeType& out){ return valueOf(kTreeTypes, name, out); }
const char* anim::interruptionSourceName(TransitionInterruptionSource source){ return nameOf(kSources, source); }
bool anim::interruptionSourceFromName(const std::string& name, TransitionInterruptionSource& out){ return valueOf(kSources, name, out); }
const char* anim::stateLoopName(AnimatorStateLoop loop){ return nameOf(kLoops, loop); }
bool anim::stateLoopFromName(const std::string& name, AnimatorStateLoop& out){ return valueOf(kLoops, name, out); }
const char* anim::additiveReferenceName(AdditiveReference reference){ return nameOf(kReferences, reference); }
bool anim::additiveReferenceFromName(const std::string& name, AdditiveReference& out){ return valueOf(kReferences, name, out); }
const char* anim::bodyPartName(AvatarBodyPart part){ return nameOf(kBodyParts, part); }
bool anim::bodyPartFromName(const std::string& name, AvatarBodyPart& out){ return valueOf(kBodyParts, name, out); }
const char* anim::ikTypeName(AnimatorIKType type){ return nameOf(kIKTypes, type); }
bool anim::ikTypeFromName(const std::string& name, AnimatorIKType& out){ return valueOf(kIKTypes, name, out); }

bool anim::isClipExtensionKey(const std::string& key){
    return key == "root_motion" || key == "editor" || key == "optimize";
}

std::string anim::AnimatorParseError::toString() const{
    if (line > 0) return std::to_string(line) + ":" + std::to_string(column) + ": " + message;
    return message;
}

int anim::ControllerDef::findParameter(const std::string& name) const{
    for (size_t i = 0; i < parameters.size(); i++){
        if (parameters[i].name == name) return (int)i;
    }
    return -1;
}

int anim::ControllerDef::findLayer(const std::string& name) const{
    for (size_t i = 0; i < layers.size(); i++){
        if (layers[i].name == name) return (int)i;
    }
    return -1;
}

int anim::MontageDef::findSection(const std::string& name) const{
    for (size_t i = 0; i < sections.size(); i++){
        if (sections[i].name == name) return (int)i;
    }
    return -1;
}

namespace{

    void visitMotion(MotionDef& motion, const std::string& path, const std::function<void(AnimatorAssetRef&, const std::string&)>& visit){
        if (!motion.clip.empty()) visit(motion.clip, path + ".clip");
        if (!motion.isTree()) return;
        BlendTreeDef& tree = motion.tree.front();
        for (size_t i = 0; i < tree.children.size(); i++){
            visitMotion(tree.children[i].motion, path + ".blend_tree.children[" + std::to_string(i) + "].motion", visit);
        }
    }

    void visitMachine(StateMachineDef& machine, const std::string& path, const std::function<void(AnimatorAssetRef&, const std::string&)>& visit){
        for (size_t i = 0; i < machine.states.size(); i++){
            visitMotion(machine.states[i].motion, path + ".states[" + std::to_string(i) + "].motion", visit);
        }
        for (size_t i = 0; i < machine.subMachines.size(); i++){
            visitMachine(machine.subMachines[i], path + ".sub_state_machines[" + std::to_string(i) + "]", visit);
        }
    }

}

namespace{

    void clearParameterInMotion(anim::MotionDef& motion, const std::string& name){
        if (!motion.isTree()) return;
        anim::BlendTreeDef& tree = motion.tree.front();
        if (tree.parameter == name) tree.parameter.clear();
        if (tree.parameterY == name) tree.parameterY.clear();
        for (anim::BlendChildDef& child : tree.children){
            if (child.directParameter == name) child.directParameter.clear();
            clearParameterInMotion(child.motion, name);
        }
    }

    void removeParameterInMachine(anim::StateMachineDef& machine, const std::string& name){
        auto prune = [&](std::vector<anim::TransitionDef>& list){
            for (anim::TransitionDef& t : list){
                t.conditions.erase(std::remove_if(t.conditions.begin(), t.conditions.end(),
                                                  [&](const anim::ConditionDef& c){ return c.parameter == name; }), t.conditions.end());
            }
        };
        prune(machine.entry);
        prune(machine.anyState);
        prune(machine.exitTransitions);
        for (anim::StateDef& s : machine.states){
            prune(s.transitions);
            clearParameterInMotion(s.motion, name);
            for (std::string* p : {&s.speedParameter, &s.cycleOffsetParameter, &s.timeParameter}) if (*p == name) p->clear();
        }
        for (anim::StateMachineDef& sub : machine.subMachines) removeParameterInMachine(sub, name);
    }

}

void anim::removeParameterReferences(ControllerDef& data, const std::string& name){
    for (LayerDef& layer : data.layers) removeParameterInMachine(layer.machine, name);
}

void anim::visitControllerRefs(ControllerDef& data, const std::function<void(AnimatorAssetRef& ref, const std::string& field)>& visit){
    for (size_t l = 0; l < data.layers.size(); l++){
        const std::string path = "layers[" + std::to_string(l) + "]";
        if (!data.layers[l].mask.empty()) visit(data.layers[l].mask, path + ".mask");
        visitMachine(data.layers[l].machine, path + ".state_machine", visit);
    }
}

std::string anim::animatorAssetKind(const std::string& text){
    const std::string format = topLevelString(text, "format");
    if (format == CONTROLLER_FORMAT) return "controller";
    if (format == MASK_FORMAT) return "mask";
    if (format == MONTAGE_FORMAT) return "montage";
    return std::string();
}

std::string anim::readAnimatorAssetGuid(const std::string& text){
    const std::string guid = topLevelString(text, "guid");
    return isClipGuid(guid) ? guid : std::string();
}

// ---------------------------------------------------------------- public: controller

namespace{

    // ---------------------------------------------------------------- version 1 dialects

    bool hasMember(const OJson& object, const char* name){
        return object.is_object() && object.find(name) != object.end();
    }

    // a version 1 controller is upstream's when some layer carries upstream's members (flat
    // states / transitions, "blend", a string mask...) and none carries S21's state machine
    bool isUpstreamController(const OJson& root){
        auto layers = root.find("layers");
        if (layers == root.end() || !layers->is_array()) return false;
        bool upstream = false;
        for (const OJson& layer : *layers){
            if (!layer.is_object()) continue;
            if (hasMember(layer, "state_machine") || hasMember(layer, "blending") || hasMember(layer, "slot")) return false;
            auto mask = layer.find("mask");
            if (hasMember(layer, "states") || hasMember(layer, "transitions") || hasMember(layer, "blend") || hasMember(layer, "default_state") ||
                hasMember(layer, "any_state") || hasMember(layer, "mask_guid") || (mask != layer.end() && mask->is_string())){
                upstream = true;
            }
        }
        return upstream;
    }

    // the S21 controller members of each kind of object (an unknown key of the same name, from a
    // converted file, would be written twice: it is renamed "upstream_<key>")
    const std::set<std::string> kLayerMembers = {"name", "weight", "blending", "mask", "slot", "state_machine"};
    const std::set<std::string> kMachineMembers = {"name", "position", "default_state", "entry", "any_state", "states", "sub_state_machines",
                                                   "exit_transitions", "entry_position", "any_state_position", "exit_position"};
    const std::set<std::string> kStateMembers = {"name", "position", "motion", "speed", "speed_parameter", "cycle_offset", "cycle_offset_parameter",
                                                 "time_parameter", "loop", "tag", "additive_reference", "additive_time", "mirror", "foot_ik",
                                                 "retarget", "transitions"};
    const std::set<std::string> kTransitionMembers = {"to", "conditions", "has_exit_time", "exit_time", "fixed_duration", "duration", "offset",
                                                      "interruption_source", "ordered_interruption", "can_transition_to_self", "mute"};
    const std::set<std::string> kTreeMembers = {"type", "parameter", "parameter_y", "children"};
    const std::set<std::string> kChildMembers = {"motion", "threshold", "position", "time_scale", "direct_parameter", "mirror"};
    const std::set<std::string> kConditionMembers = {"parameter", "mode", "threshold"};
    const std::set<std::string> kControllerMembers = {"format", "version", "guid", "parameters", "layers"};
    const std::set<std::string> kMaskMembers = {"format", "version", "guid", "default", "body_parts", "root_hierarchy", "paths", "preview"};

    void dedupeUnknown(AnimatorUnknownKeys& keys, const std::set<std::string>& members, const std::string& path, std::vector<ClipWarning>* warnings){
        std::set<std::string> seen;
        for (auto& entry : keys){
            std::string name = entry.first;
            while (members.count(name) || seen.count(name)) name = "upstream_" + name;
            if (name != entry.first){
                if (warnings) warnings->push_back({path, "unknown key \"" + entry.first + "\" kept as \"" + name + "\" (the name is a member)"});
                entry.first = name;
            }
            seen.insert(name);
        }
    }

    void dedupeTransitions(std::vector<TransitionDef>& list, const std::string& path, std::vector<ClipWarning>* warnings){
        for (size_t i = 0; i < list.size(); i++){
            const std::string at = path + "[" + std::to_string(i) + "]";
            dedupeUnknown(list[i].unknownKeys, kTransitionMembers, at, warnings);
            for (ConditionDef& c : list[i].conditions) dedupeUnknown(c.unknownKeys, kConditionMembers, at + ".conditions", warnings);
        }
    }

    void dedupeMotion(MotionDef& motion, const std::string& path, std::vector<ClipWarning>* warnings){
        if (!motion.isTree()) return;
        BlendTreeDef& tree = motion.tree.front();
        dedupeUnknown(tree.unknownKeys, kTreeMembers, path + ".blend_tree", warnings);
        for (size_t i = 0; i < tree.children.size(); i++){
            const std::string at = path + ".blend_tree.children[" + std::to_string(i) + "]";
            dedupeUnknown(tree.children[i].unknownKeys, kChildMembers, at, warnings);
            dedupeMotion(tree.children[i].motion, at + ".motion", warnings);
        }
    }

    void dedupeMachine(StateMachineDef& machine, const std::string& path, std::vector<ClipWarning>* warnings){
        dedupeUnknown(machine.unknownKeys, kMachineMembers, path, warnings);
        dedupeTransitions(machine.entry, path + ".entry", warnings);
        dedupeTransitions(machine.anyState, path + ".any_state", warnings);
        dedupeTransitions(machine.exitTransitions, path + ".exit_transitions", warnings);
        for (size_t i = 0; i < machine.states.size(); i++){
            const std::string at = path + ".states[" + std::to_string(i) + "]";
            dedupeUnknown(machine.states[i].unknownKeys, kStateMembers, at, warnings);
            dedupeMotion(machine.states[i].motion, at + ".motion", warnings);
            dedupeTransitions(machine.states[i].transitions, at + ".transitions", warnings);
        }
        for (size_t i = 0; i < machine.subMachines.size(); i++){
            dedupeMachine(machine.subMachines[i], path + ".sub_state_machines[" + std::to_string(i) + "]", warnings);
        }
    }

    // a version 1 mask is upstream's when it has upstream's members and not S21's, or when only
    // upstream's carry values (a file upstream's editor saved over S21's keeps S21's members as
    // they were, and writes its own defaults)
    bool upstreamMaskInformative(const OJson& root){
        auto def = root.find("default");
        if (def != root.end() && !(def->is_number() && def->get<double>() == 1.0) && !(def->is_boolean() && def->get<bool>())) return true;
        if (hasMember(root, "humanoid") || hasMember(root, "preview")) return true;
        auto transforms = root.find("transforms");
        return transforms != root.end() && !(transforms->is_array() && transforms->empty());
    }

    bool s21MaskInformative(const OJson& root){
        auto paths = root.find("paths");
        if (paths != root.end() && !(paths->is_array() && paths->empty())) return true;
        auto parts = root.find("body_parts");
        if (parts != root.end() && parts->is_object()){
            for (auto it = parts->begin(); it != parts->end(); ++it){
                if (!(it->is_boolean() && it->get<bool>())) return true;
            }
        }
        return false;
    }

    bool isUpstreamMask(const OJson& root){
        const bool s21Keys = hasMember(root, "body_parts") || hasMember(root, "paths");
        const bool upstreamKeys = hasMember(root, "default") || hasMember(root, "humanoid") || hasMember(root, "transforms") || hasMember(root, "preview");
        if (!upstreamKeys) return false;
        if (!s21Keys) return true;
        return upstreamMaskInformative(root) && !s21MaskInformative(root);
    }

    template<class T>
    void copyUpstreamError(AnimatorParseError& out, const T& error){
        out.line = error.line;
        out.column = error.column;
        out.message = error.message;
    }

}

const char* anim::animatorFileDialectName(AnimatorFileDialect dialect){
    switch (dialect){
        case AnimatorFileDialect::Current: return "current";
        case AnimatorFileDialect::S21Version1: return "s21_v1";
        case AnimatorFileDialect::UpstreamVersion1: return "upstream_v1";
    }
    return "current";
}

bool anim::isUnityYamlController(const std::string& text){
    return json::isUnityYaml(text, "AnimatorController:");
}

const char* anim::unityYamlControllerRefusal(){
    return "This is a Unity .controller (YAML). Create a JasperEngine Animator Controller (Assets > Create > Animator Controller, "
           "or MCP edit_animator_controller {path, create: true}).";
}

bool anim::isUnityYamlMask(const std::string& text){
    return json::isUnityYaml(text, "AvatarMask:");
}

const char* anim::unityYamlMaskRefusal(){
    return "This is a Unity .mask (YAML). Create a JasperEngine Avatar Mask (Assets > Create > Avatar Mask, or MCP edit_avatar_mask {path, create: true}).";
}

// ---------------------------------------------------------------- public: controller

anim::ControllerParseResult anim::parseController(const std::string& text){
    ControllerParseResult result;
    if (isUnityYamlController(text)){
        result.error.message = unityYamlControllerRefusal();
        return result;
    }
    try{
        OJson root;
        if (!parseRoot(text, CONTROLLER_FORMAT, "an Animator Controller", CONTROLLER_VERSION, root, result.version, result.error)) return result;
        if (result.version == 1 && isUpstreamController(root)){
            result.dialect = AnimatorFileDialect::UpstreamVersion1;
            upstream::ParseResult<upstream::Controller> read = upstream::parseController(text);
            result.warnings = read.warnings;
            if (!read.ok()){
                copyUpstreamError(result.error, read.error);
                return result;
            }
            result.data = upstream::toControllerDef(*read.data, &result.warnings);
            normalizeController(result.data, &result.warnings);
            result.ok = true;
            return result;
        }
        if (result.version == 1) result.dialect = AnimatorFileDialect::S21Version1;
        Reader reader(result.warnings);
        ControllerDef& data = result.data;
        readGuid(reader, root, data.guid);
        if (const OJson* parameters = reader.array(root, "", "parameters")){
            for (size_t i = 0; i < parameters->size(); i++){
                const std::string at = Reader::index("parameters", i);
                const OJson& p = (*parameters)[i];
                if (!p.is_object()){
                    reader.warn(at, "expected an object: dropped");
                    continue;
                }
                ParameterDef parameter;
                if (!reader.string(p, at, "name", parameter.name) || parameter.name.empty()){
                    reader.warn(at, "a parameter needs a name: dropped");
                    continue;
                }
                reader.enumeration(p, at, "type", kParameterTypes, parameter.type);
                auto value = p.find("default");
                if (value != p.end() && !value->is_null()){
                    if (value->is_boolean()) parameter.defaultValue = value->get<bool>() ? 1.0f : 0.0f;
                    else if (value->is_number() && std::isfinite(value->get<double>())) parameter.defaultValue = (float)value->get<double>();
                    else reader.warn(Reader::member(at, "default"), "expected a number or a bool");
                }
                reader.keep(p, at, {"name", "type", "default"}, parameter.unknownKeys);
                data.parameters.push_back(parameter);
            }
        }
        if (const OJson* layers = reader.array(root, "", "layers")){
            for (size_t i = 0; i < layers->size(); i++){
                const std::string at = Reader::index("layers", i);
                const OJson& l = (*layers)[i];
                if (!l.is_object()){
                    reader.warn(at, "expected an object: dropped");
                    continue;
                }
                LayerDef layer;
                reader.string(l, at, "name", layer.name);
                reader.number(l, at, "weight", layer.weight);
                reader.enumeration(l, at, "blending", kBlendings, layer.blending);
                reader.assetRef(l, at, "mask", layer.mask);
                reader.string(l, at, "slot", layer.slot);
                auto machine = l.find("state_machine");
                if (machine != l.end() && machine->is_object()){
                    readMachine(reader, *machine, Reader::member(at, "state_machine"), layer.machine, 0);
                }else if (machine != l.end()){
                    reader.warn(Reader::member(at, "state_machine"), "expected an object");
                }
                layer.machine.name.clear();
                reader.keep(l, at, {"name", "weight", "blending", "mask", "slot", "state_machine"}, layer.unknownKeys);
                data.layers.push_back(std::move(layer));
            }
        }
        keepUnknownTopLevel(root, {"format", "version", "guid", "parameters", "layers"}, data.unknownKeys, result.warnings);
        normalizeController(data, &result.warnings);
        result.ok = true;
    }catch (const std::exception& e){
        result.ok = false;
        result.error.message = std::string("cannot read the controller: ") + e.what();
    }
    return result;
}

void anim::normalizeController(ControllerDef& data, std::vector<ClipWarning>* warnings){
    if (!data.guid.empty() && !isClipGuid(data.guid)) data.guid.clear();
    std::set<std::string> parameterNames;
    for (size_t i = 0; i < data.parameters.size(); i++){
        ParameterDef& p = data.parameters[i];
        const std::string unique = uniqueName(p.name.empty() ? std::string("Parameter") : p.name, parameterNames);
        if (unique != p.name && warnings){
            warnings->push_back({"parameters[" + std::to_string(i) + "].name", "\"" + p.name + "\" is taken: renamed \"" + unique + "\""});
        }
        p.name = unique;
        parameterNames.insert(p.name);
        if (!std::isfinite(p.defaultValue)) p.defaultValue = 0.0f;
        if (p.type == AnimatorParameterType::Int) p.defaultValue = std::round(p.defaultValue);
        if (p.type == AnimatorParameterType::Bool || p.type == AnimatorParameterType::Trigger) p.defaultValue = p.defaultValue != 0.0f ? 1.0f : 0.0f;
    }
    std::set<std::string> layerNames;
    for (size_t i = 0; i < data.layers.size(); i++){
        LayerDef& layer = data.layers[i];
        const std::string at = "layers[" + std::to_string(i) + "]";
        const std::string unique = uniqueName(layer.name.empty() ? std::string(i == 0 ? "Base Layer" : "New Layer") : layer.name, layerNames);
        if (unique != layer.name && warnings && !layer.name.empty()){
            warnings->push_back({at + ".name", "\"" + layer.name + "\" is taken: renamed \"" + unique + "\""});
        }
        layer.name = unique;
        layerNames.insert(layer.name);
        if (!std::isfinite(layer.weight)) layer.weight = 1.0f;
        layer.weight = i == 0 ? 1.0f : clamp01(layer.weight);
        normalizeMachine(layer.machine, at + ".state_machine", warnings);
        dedupeUnknown(layer.unknownKeys, kLayerMembers, at, warnings);
        dedupeMachine(layer.machine, at + ".state_machine", warnings);
    }
    for (size_t i = 0; i < data.parameters.size(); i++){
        dedupeUnknown(data.parameters[i].unknownKeys, {"name", "type", "default"}, "parameters[" + std::to_string(i) + "]", warnings);
    }
    dedupeUnknown(data.unknownKeys, kControllerMembers, "", warnings);
}

std::string anim::writeController(const ControllerDef& data){
    Writer out;
    out.open('{');
    writeHeader(out, CONTROLLER_FORMAT, CONTROLLER_VERSION, data.guid);
    out.key("parameters");
    out.open('[');
    for (const ParameterDef& p : data.parameters){
        out.element();
        std::string text = "{\"name\": " + quote(p.name) + ", \"type\": " + quote(parameterTypeName(p.type));
        if (p.type == AnimatorParameterType::Bool) text += std::string(", \"default\": ") + (p.defaultValue != 0.0f ? "true" : "false");
        else if (p.type != AnimatorParameterType::Trigger) text += ", \"default\": " + number(p.defaultValue);
        out.raw(text + unknownInline(p.unknownKeys) + "}");
    }
    out.close(']');
    out.key("layers");
    out.open('[');
    for (const LayerDef& layer : data.layers){
        out.element();
        out.open('{');
        out.key("name"); out.str(layer.name);
        out.key("weight"); out.num(layer.weight);
        out.key("blending"); out.str(layerBlendingName(layer.blending));
        out.key("mask"); writeRef(out, layer.mask);
        out.key("slot"); out.str(layer.slot);
        out.key("state_machine");
        writeMachine(out, layer.machine, true);
        writeUnknown(out, layer.unknownKeys);
        out.close('}');
    }
    out.close(']');
    writeUnknown(out, data.unknownKeys);
    out.close('}');
    out.raw("\n");
    return out.text;
}

// ---------------------------------------------------------------- public: mask

anim::MaskParseResult anim::parseMask(const std::string& text){
    MaskParseResult result;
    if (isUnityYamlMask(text)){
        result.error.message = unityYamlMaskRefusal();
        return result;
    }
    try{
        OJson root;
        if (!parseRoot(text, MASK_FORMAT, "an Avatar Mask", MASK_VERSION, root, result.version, result.error)) return result;
        AvatarMaskDef& data = result.data;
        if (result.version == 1 && isUpstreamMask(root)){
            result.dialect = AnimatorFileDialect::UpstreamVersion1;
            upstream::ParseResult<upstream::Mask> read = upstream::parseMask(text);
            result.warnings = read.warnings;
            if (!read.ok()){
                copyUpstreamError(result.error, read.error);
                return result;
            }
            data = upstream::toMaskDef(*read.data);
            // S21's members left at their defaults by upstream's editor say nothing: dropped
            AnimatorUnknownKeys kept;
            for (const auto& entry : data.unknownKeys){
                if (entry.first == "body_parts" || entry.first == "paths") continue;
                kept.push_back(entry);
            }
            data.unknownKeys = std::move(kept);
            normalizeMask(data, &result.warnings);
            result.ok = true;
            return result;
        }
        Reader reader(result.warnings);
        readGuid(reader, root, data.guid);
        if (result.version == 1){
            // S21's version 1: every part a bool, paths exact. The version 2 form gives the bones
            // the same weights: default = the body's (what no other part claimed), the hand's part
            // also below the hand.
            result.dialect = AnimatorFileDialect::S21Version1;
            bool parts[AVATAR_BODY_PART_COUNT];
            for (bool& part : parts) part = true;
            auto bodyParts = root.find("body_parts");
            if (bodyParts != root.end() && bodyParts->is_object()){
                for (auto it = bodyParts->begin(); it != bodyParts->end(); ++it){
                    AvatarBodyPart part;
                    if (!bodyPartFromName(it.key(), part) || part == AvatarBodyPart::LeftFingers || part == AvatarBodyPart::RightFingers){
                        reader.warn("body_parts." + it.key(), "unknown body part (root, body, head, left_arm, right_arm, left_hand, right_hand, left_leg, right_leg)");
                        continue;
                    }
                    if (!it->is_boolean()){
                        reader.warn("body_parts." + it.key(), "expected true or false");
                        continue;
                    }
                    parts[(int)part] = it->get<bool>();
                }
            }else if (bodyParts != root.end() && !bodyParts->is_null()){
                reader.warn("body_parts", "expected an object");
            }
            parts[(int)AvatarBodyPart::LeftFingers] = parts[(int)AvatarBodyPart::LeftHand];
            parts[(int)AvatarBodyPart::RightFingers] = parts[(int)AvatarBodyPart::RightHand];
            for (int i = 0; i < AVATAR_BODY_PART_COUNT; i++) data.bodyParts[i] = parts[i] ? 1.0f : 0.0f;
            data.defaultWeight = parts[(int)AvatarBodyPart::Body] ? 1.0f : 0.0f;
            data.rootHierarchy = false;
            if (const OJson* paths = reader.array(root, "", "paths")){
                for (size_t i = 0; i < paths->size(); i++){
                    const std::string at = Reader::index("paths", i);
                    const OJson& p = (*paths)[i];
                    if (!p.is_object()){
                        reader.warn(at, "expected an object: dropped");
                        continue;
                    }
                    MaskPathDef entry;
                    if (!reader.string(p, at, "path", entry.path)){
                        reader.warn(at, "an entry needs a path: dropped");
                        continue;
                    }
                    reader.number(p, at, "weight", entry.weight);
                    reader.unknown(p, at, {"path", "weight"});
                    data.paths.push_back(entry);
                }
            }
            // upstream's members (upstream's editor saved over this file): their defaults say
            // nothing; values are kept under upstream_<key> (written back, not read)
            const bool informative = upstreamMaskInformative(root);
            for (auto it = root.begin(); it != root.end(); ++it){
                const std::string& key = it.key();
                if (key == "format" || key == "version" || key == "guid" || key == "body_parts" || key == "paths") continue;
                const bool upstreamKey = key == "default" || key == "humanoid" || key == "transforms" || key == "preview";
                if (upstreamKey && !informative) continue;
                data.unknownKeys.push_back({upstreamKey ? "upstream_" + key : key, it->dump(-1, ' ', false, OJson::error_handler_t::replace)});
            }
            if (informative) reader.warn("", "upstream's mask members next to S21's: S21's are read, upstream's kept as upstream_<key>");
            else if (!data.unknownKeys.empty()) {
                std::string names;
                for (const auto& entry : data.unknownKeys) names += (names.empty() ? "" : ", ") + entry.first;
                reader.warn("", "unknown keys kept as they are (written back on save): " + names);
            }
            normalizeMask(data, &result.warnings);
            result.ok = true;
            return result;
        }

        // version 2
        auto def = root.find("default");
        if (def != root.end() && !def->is_null()){
            float weight = 1.0f;
            if (json::toWeight(*def, weight)) data.defaultWeight = weight;
            else reader.warn("default", "expected a weight from 0 to 1: using 1");
        }
        auto parts = root.find("body_parts");
        if (parts != root.end() && parts->is_object()){
            for (auto it = parts->begin(); it != parts->end(); ++it){
                AvatarBodyPart part;
                if (!bodyPartFromName(it.key(), part)){
                    reader.warn("body_parts." + it.key(), "unknown body part (one of " + namesOf(kBodyParts) + ")");
                    continue;
                }
                float weight = 1.0f;
                if (!json::toWeight(*it, weight)){
                    reader.warn("body_parts." + it.key(), "expected a weight from 0 to 1 (or true / false)");
                    continue;
                }
                data.bodyParts[(int)part] = weight;
            }
        }else if (parts != root.end() && !parts->is_null()){
            reader.warn("body_parts", "expected an object");
        }
        reader.boolean(root, "", "root_hierarchy", data.rootHierarchy);
        if (const OJson* paths = reader.array(root, "", "paths")){
            for (size_t i = 0; i < paths->size(); i++){
                const std::string at = Reader::index("paths", i);
                const OJson& p = (*paths)[i];
                if (!p.is_object()){
                    reader.warn(at, "expected an object: dropped");
                    continue;
                }
                MaskPathDef entry;
                if (!reader.string(p, at, "path", entry.path)){
                    reader.warn(at, "an entry needs a path: dropped");
                    continue;
                }
                reader.number(p, at, "weight", entry.weight);
                reader.boolean(p, at, "children", entry.children);
                reader.unknown(p, at, {"path", "weight", "children"});
                data.paths.push_back(entry);
            }
        }
        auto preview = root.find("preview");
        if (preview != root.end() && preview->is_object()){
            reader.string(*preview, "preview", "model", data.previewModel);
            reader.unknown(*preview, "preview", {"model"});
        }else if (preview != root.end() && !preview->is_null()){
            reader.warn("preview", "expected {\"model\"}");
        }
        keepUnknownTopLevel(root, {"format", "version", "guid", "default", "body_parts", "root_hierarchy", "paths", "preview"}, data.unknownKeys,
                            result.warnings);
        normalizeMask(data, &result.warnings);
        result.ok = true;
    }catch (const std::exception& e){
        result.ok = false;
        result.error.message = std::string("cannot read the mask: ") + e.what();
    }
    return result;
}

void anim::normalizeMask(AvatarMaskDef& data, std::vector<ClipWarning>* warnings){
    if (!data.guid.empty() && !isClipGuid(data.guid)) data.guid.clear();
    auto clamped = [&](float value, const std::string& path){
        const float result = std::isfinite(value) ? clamp01(value) : 1.0f;
        if (result != value && warnings) warnings->push_back({path, "clamped to 0..1"});
        return result;
    };
    data.defaultWeight = clamped(data.defaultWeight, "default");
    for (int i = 0; i < AVATAR_BODY_PART_COUNT; i++){
        if (data.bodyParts[i]) data.bodyParts[i] = clamped(*data.bodyParts[i], std::string("body_parts.") + bodyPartName((AvatarBodyPart)i));
    }
    // sorted by path, one entry per path (the later one wins)
    std::map<std::string, MaskPathDef> unique;
    for (const MaskPathDef& entry : data.paths){
        MaskPathDef value = entry;
        value.weight = std::isfinite(entry.weight) ? clamp01(entry.weight) : 1.0f;
        if (value.weight != entry.weight && warnings) warnings->push_back({"paths", "the weight of \"" + entry.path + "\" clamped to 0..1"});
        if (unique.count(entry.path) && warnings) warnings->push_back({"paths", "\"" + entry.path + "\" is listed twice: the later entry is used"});
        unique[entry.path] = value;
    }
    data.paths.clear();
    for (const auto& entry : unique) data.paths.push_back(entry.second);
    dedupeUnknown(data.unknownKeys, kMaskMembers, "", warnings);
}

std::string anim::writeMask(const AvatarMaskDef& data){
    Writer out;
    out.open('{');
    writeHeader(out, MASK_FORMAT, MASK_VERSION, data.guid);
    out.key("default");
    out.num(data.defaultWeight);
    out.key("body_parts");
    out.open('{');
    for (int i = 0; i < AVATAR_BODY_PART_COUNT; i++){
        if (!data.bodyParts[i]) continue;
        out.key(bodyPartName((AvatarBodyPart)i));
        out.num(*data.bodyParts[i]);
    }
    out.close('}');
    out.key("root_hierarchy");
    out.boolean(data.rootHierarchy);
    out.key("paths");
    out.open('[');
    for (const MaskPathDef& entry : data.paths){
        out.element();
        out.raw("{\"path\": " + quote(entry.path) + ", \"weight\": " + number(entry.weight) + ", \"children\": " +
                (entry.children ? "true" : "false") + "}");
    }
    out.close(']');
    if (!data.previewModel.empty()){
        out.key("preview");
        out.raw("{\"model\": " + quote(data.previewModel) + "}");
    }
    writeUnknown(out, data.unknownKeys);
    out.close('}');
    out.raw("\n");
    return out.text;
}

// ---------------------------------------------------------------- public: montage

anim::MontageParseResult anim::parseMontage(const std::string& text){
    MontageParseResult result;
    try{
        OJson root;
        if (!parseRoot(text, MONTAGE_FORMAT, "an Animation Montage", MONTAGE_VERSION, root, result.version, result.error)) return result;
        Reader reader(result.warnings);
        MontageDef& data = result.data;
        readGuid(reader, root, data.guid);
        reader.assetRef(root, "", "clip", data.clip);
        reader.string(root, "", "slot", data.slot);
        reader.number(root, "", "rate", data.rate);
        for (const char* name : {"blend_in", "blend_out"}){
            MontageBlendDef& blend = std::strcmp(name, "blend_in") == 0 ? data.blendIn : data.blendOut;
            auto it = root.find(name);
            if (it == root.end() || it->is_null()) continue;
            if (!it->is_object()){
                reader.warn(name, "expected {\"time\", \"ease\"}");
                continue;
            }
            reader.number(*it, name, "time", blend.time);
            std::string ease;
            if (reader.string(*it, name, "ease", ease)){
                EaseType type;
                if (easeFromName(ease, type)) blend.ease = ease;
                else reader.warn(std::string(name) + ".ease", "unknown ease \"" + ease + "\": linear");
            }
            reader.unknown(*it, name, {"time", "ease"});
        }
        if (const OJson* sections = reader.array(root, "", "sections")){
            for (size_t i = 0; i < sections->size(); i++){
                const std::string at = Reader::index("sections", i);
                const OJson& s = (*sections)[i];
                if (!s.is_object()){
                    reader.warn(at, "expected an object: dropped");
                    continue;
                }
                MontageSectionDef section;
                if (!reader.string(s, at, "name", section.name) || section.name.empty()){
                    reader.warn(at, "a section needs a name: dropped");
                    continue;
                }
                reader.number(s, at, "start", section.start);
                // absent: the following section (null or "": the montage ends there)
                auto next = s.find("next");
                if (next == s.end()){
                    section.next = "\x01";
                }else if (next->is_string()){
                    section.next = next->get<std::string>();
                }else if (!next->is_null()){
                    reader.warn(Reader::member(at, "next"), "expected a section name or null");
                }
                reader.unknown(s, at, {"name", "start", "next"});
                data.sections.push_back(section);
            }
        }
        if (const OJson* events = reader.array(root, "", "events")){
            for (size_t i = 0; i < events->size(); i++){
                const std::string at = Reader::index("events", i);
                const OJson& e = (*events)[i];
                if (!e.is_object()){
                    reader.warn(at, "expected an object: dropped");
                    continue;
                }
                MontageEventDef event;
                reader.number(e, at, "time", event.time);
                if (!reader.string(e, at, "function", event.function) || !isLuaIdentifier(event.function)){
                    reader.warn(Reader::member(at, "function"), "expected a Lua identifier: dropped");
                    continue;
                }
                readEventArgs(reader, e, at, event.args);
                reader.unknown(e, at, {"time", "function", "args"});
                data.events.push_back(std::move(event));
            }
        }
        // "next" absent: the section after it in start order (resolved after the sort)
        std::vector<MontageSectionDef> sorted = data.sections;
        std::stable_sort(sorted.begin(), sorted.end(), [](const MontageSectionDef& a, const MontageSectionDef& b){ return a.start < b.start; });
        for (size_t i = 0; i < sorted.size(); i++){
            if (sorted[i].next != "\x01") continue;
            const std::string following = i + 1 < sorted.size() ? sorted[i + 1].name : std::string();
            for (MontageSectionDef& section : data.sections){
                if (section.name == sorted[i].name && section.next == "\x01") section.next = following;
            }
        }
        keepUnknownTopLevel(root, {"format", "version", "guid", "clip", "slot", "rate", "blend_in", "blend_out", "sections", "events"},
                            data.unknownKeys, result.warnings);
        normalizeMontage(data, &result.warnings);
        result.ok = true;
    }catch (const std::exception& e){
        result.ok = false;
        result.error.message = std::string("cannot read the montage: ") + e.what();
    }
    return result;
}

void anim::normalizeMontage(MontageDef& data, std::vector<ClipWarning>* warnings){
    if (!data.guid.empty() && !isClipGuid(data.guid)) data.guid.clear();
    if (data.slot.empty()) data.slot = "Default";
    if (!std::isfinite(data.rate) || data.rate == 0.0f){
        if (warnings) warnings->push_back({"rate", "must be a non-zero number: 1"});
        data.rate = 1.0f;
    }
    for (MontageBlendDef* blend : {&data.blendIn, &data.blendOut}){
        if (!(blend->time >= 0.0f) || !std::isfinite(blend->time)) blend->time = 0.0f;
        EaseType type;
        if (!easeFromName(blend->ease, type)) blend->ease = "linear";
    }
    if (data.sections.size() > MAX_MONTAGE_SECTIONS){
        if (warnings) warnings->push_back({"sections", "more than " + std::to_string(MAX_MONTAGE_SECTIONS) + " sections: the rest dropped"});
        data.sections.resize(MAX_MONTAGE_SECTIONS);
    }
    std::set<std::string> names;
    for (size_t i = 0; i < data.sections.size(); i++){
        MontageSectionDef& section = data.sections[i];
        if (!(section.start >= 0.0f) || !std::isfinite(section.start)) section.start = 0.0f;
        const std::string unique = uniqueName(section.name.empty() ? std::string("Section") : section.name, names);
        if (unique != section.name && warnings){
            warnings->push_back({"sections[" + std::to_string(i) + "].name", "\"" + section.name + "\" is taken: renamed \"" + unique + "\""});
        }
        section.name = unique;
        names.insert(unique);
    }
    std::stable_sort(data.sections.begin(), data.sections.end(),
                     [](const MontageSectionDef& a, const MontageSectionDef& b){ return a.start < b.start; });
    if (data.sections.empty()){
        data.sections.push_back({"Default", 0.0f, ""});
    }else if (data.sections.front().start != 0.0f){
        if (warnings) warnings->push_back({"sections[0].start", "the first section starts at 0"});
        data.sections.front().start = 0.0f;
    }
    for (MontageSectionDef& section : data.sections){
        if (section.next == "\x01") section.next.clear();
        if (!section.next.empty() && !names.count(section.next)){
            if (warnings) warnings->push_back({"sections", "\"" + section.name + "\".next names no section (\"" + section.next + "\"): the montage ends there"});
            section.next.clear();
        }
    }
    if (data.events.size() > MAX_MONTAGE_EVENTS){
        if (warnings) warnings->push_back({"events", "more than " + std::to_string(MAX_MONTAGE_EVENTS) + " events: the rest dropped"});
        data.events.resize(MAX_MONTAGE_EVENTS);
    }
    for (MontageEventDef& event : data.events){
        if (!(event.time >= 0.0f) || !std::isfinite(event.time)) event.time = 0.0f;
    }
    std::stable_sort(data.events.begin(), data.events.end(), [](const MontageEventDef& a, const MontageEventDef& b){
        return a.time < b.time || (a.time == b.time && a.function < b.function);
    });
}

std::string anim::writeMontage(const MontageDef& data){
    Writer out;
    out.open('{');
    writeHeader(out, MONTAGE_FORMAT, MONTAGE_VERSION, data.guid);
    out.key("clip"); writeRef(out, data.clip);
    out.key("slot"); out.str(data.slot);
    out.key("rate"); out.num(data.rate);
    out.key("blend_in"); out.raw("{\"time\": " + number(data.blendIn.time) + ", \"ease\": " + quote(data.blendIn.ease) + "}");
    out.key("blend_out"); out.raw("{\"time\": " + number(data.blendOut.time) + ", \"ease\": " + quote(data.blendOut.ease) + "}");
    out.key("sections");
    out.open('[');
    for (const MontageSectionDef& section : data.sections){
        out.element();
        out.raw("{\"name\": " + quote(section.name) + ", \"start\": " + number(section.start) + ", \"next\": " +
                (section.next.empty() ? std::string("null") : quote(section.next)) + "}");
    }
    out.close(']');
    out.key("events");
    out.open('[');
    for (const MontageEventDef& event : data.events){
        out.element();
        out.raw("{\"time\": " + number(event.time) + ", \"function\": " + quote(event.function));
        if (!event.args.empty()){
            out.raw(", \"args\": ");
            writeArgs(out, event.args);
        }
        out.raw("}");
    }
    out.close(']');
    writeUnknown(out, data.unknownKeys);
    out.close('}');
    out.raw("\n");
    return out.text;
}

// ---------------------------------------------------------------- public: clip root motion

anim::ClipRootMotion anim::readClipRootMotion(const std::vector<std::pair<std::string, std::string>>& unknownKeys,
                                              std::vector<ClipWarning>* warnings){
    ClipRootMotion out;
    for (const auto& entry : unknownKeys){
        if (entry.first != "root_motion") continue;
        OJson value = OJson::parse(entry.second, nullptr, false);
        if (value.is_discarded() || !value.is_object()){
            if (warnings) warnings->push_back({"root_motion", "expected {\"bone\", \"bake_rotation\", \"bake_position_y\", \"bake_position_xz\"}"});
            return out;
        }
        std::vector<ClipWarning> local;
        Reader reader(warnings ? *warnings : local);
        out.present = true;
        reader.string(value, "root_motion", "bone", out.bone);
        reader.boolean(value, "root_motion", "bake_rotation", out.bakeRotation);
        reader.boolean(value, "root_motion", "bake_position_y", out.bakePositionY);
        reader.boolean(value, "root_motion", "bake_position_xz", out.bakePositionXZ);
        reader.unknown(value, "root_motion", {"bone", "bake_rotation", "bake_position_y", "bake_position_xz"});
    }
    return out;
}

std::string anim::writeClipRootMotion(const ClipRootMotion& rootMotion){
    return "{\"bone\": " + quote(rootMotion.bone) + ", \"bake_rotation\": " + (rootMotion.bakeRotation ? "true" : "false") +
           ", \"bake_position_y\": " + (rootMotion.bakePositionY ? "true" : "false") +
           ", \"bake_position_xz\": " + (rootMotion.bakePositionXZ ? "true" : "false") + "}";
}

// ---------------------------------------------------------------- public: validation

bool anim::conditionModeFits(AnimatorConditionMode mode, AnimatorParameterType type){
    switch (mode){
        case AnimatorConditionMode::Greater:
        case AnimatorConditionMode::Less:
            return type == AnimatorParameterType::Float || type == AnimatorParameterType::Int;
        case AnimatorConditionMode::Equals:
        case AnimatorConditionMode::NotEqual:
            return type == AnimatorParameterType::Int;
        case AnimatorConditionMode::If:
            return type == AnimatorParameterType::Bool || type == AnimatorParameterType::Trigger;
        case AnimatorConditionMode::IfNot:
            return type == AnimatorParameterType::Bool;
    }
    return false;
}

namespace{

    struct Validator{
        const ControllerDef& def;
        const ControllerClipResolver& resolve;
        std::vector<ClipWarning>& out;
        std::vector<bool> used;

        Validator(const ControllerDef& def, const ControllerClipResolver& resolve, std::vector<ClipWarning>& out)
            : def(def), resolve(resolve), out(out), used(def.parameters.size(), false){}

        void read(const std::string& name){
            const int index = def.findParameter(name);
            if (index >= 0) used[(size_t)index] = true;
        }

        // the motion's clips load; whether every one of them loops (an empty motion never ends)
        bool motion(const MotionDef& m, const std::string& path){
            if (m.isTree()){
                const BlendTreeDef& tree = m.tree.front();
                read(tree.parameter);
                read(tree.parameterY);
                if (tree.children.empty()) out.push_back({path + ".blend_tree.children", "the blend tree has no children: the state writes nothing"});
                bool loops = true;
                for (size_t i = 0; i < tree.children.size(); i++){
                    const BlendChildDef& child = tree.children[i];
                    const std::string at = path + ".blend_tree.children[" + std::to_string(i) + "]";
                    read(child.directParameter);
                    if (child.motion.empty()){
                        out.push_back({at + ".motion", "a child without a motion"});
                        loops = false;
                        continue;
                    }
                    loops = motion(child.motion, at + ".motion") && loops;
                }
                return loops;
            }
            if (m.clip.empty()) return true;
            if (!resolve) return true;
            const ControllerClipInfo info = resolve(m.clip.path);
            if (!info.loads){
                out.push_back({path + ".clip", "\"" + m.clip.path + "\" does not load" + (info.error.empty() ? "" : ": " + info.error)});
            }
            return info.loops;
        }

        void transitions(const std::vector<TransitionDef>& list, const std::string& path, bool needsTrigger){
            for (size_t i = 0; i < list.size(); i++){
                const TransitionDef& t = list[i];
                const std::string at = path + "[" + std::to_string(i) + "]";
                for (size_t c = 0; c < t.conditions.size(); c++){
                    const ConditionDef& condition = t.conditions[c];
                    const int index = def.findParameter(condition.parameter);
                    if (index < 0) continue;    // the compiler reports unknown parameters
                    used[(size_t)index] = true;
                    const AnimatorParameterType type = def.parameters[(size_t)index].type;
                    if (!conditionModeFits(condition.mode, type)){
                        out.push_back({at + ".conditions[" + std::to_string(c) + "].mode",
                                       std::string("\"") + conditionModeName(condition.mode) + "\" does not fit the " + parameterTypeName(type) +
                                       " parameter \"" + condition.parameter + "\" (" +
                                       (type == AnimatorParameterType::Float ? "greater or less" :
                                        type == AnimatorParameterType::Int ? "greater, less, equals or not_equal" :
                                        type == AnimatorParameterType::Bool ? "if or if_not" : "if") + ")"});
                    }
                }
                if (needsTrigger && t.conditions.empty() && !t.hasExitTime && !t.mute){
                    out.push_back({at, "neither conditions nor an exit time: never taken (add a condition or has_exit_time)"});
                }
            }
        }

        bool anyStateIn(const StateMachineDef& machine){
            if (!machine.anyState.empty()) return true;
            for (const StateMachineDef& sub : machine.subMachines){
                if (anyStateIn(sub)) return true;
            }
            return false;
        }

        void machine(const StateMachineDef& m, const std::string& path, bool layerHasAny){
            transitions(m.entry, path + ".entry", false);
            transitions(m.anyState, path + ".any_state", true);
            transitions(m.exitTransitions, path + ".exit_transitions", false);
            for (size_t i = 0; i < m.states.size(); i++){
                const StateDef& s = m.states[i];
                const std::string at = path + ".states[" + std::to_string(i) + "]";
                read(s.speedParameter);
                read(s.cycleOffsetParameter);
                read(s.timeParameter);
                bool loops = motion(s.motion, at + ".motion");
                switch (s.loop){
                    case AnimatorStateLoop::Inherit: break;
                    case AnimatorStateLoop::Loop: case AnimatorStateLoop::PingPong: case AnimatorStateLoop::Repeat: loops = true; break;
                    case AnimatorStateLoop::Once: case AnimatorStateLoop::ClampForever: loops = s.motion.empty(); break;
                }
                // a time parameter drives the state: it never ends by itself
                if (!s.timeParameter.empty()) loops = true;
                transitions(s.transitions, at + ".transitions", true);
                bool exits = layerHasAny;
                for (const TransitionDef& t : s.transitions) exits = exits || !t.mute;
                if (!exits && !loops){
                    out.push_back({at, "\"" + s.name + "\" is a dead end: no transition leaves it and its motion does not loop (it holds its last frame)"});
                }
            }
            for (size_t i = 0; i < m.subMachines.size(); i++){
                machine(m.subMachines[i], path + ".sub_state_machines[" + std::to_string(i) + "]", layerHasAny);
            }
        }
    };

}

std::vector<ClipWarning> anim::validateController(const ControllerDef& data, const ControllerClipResolver& resolve){
    std::vector<ClipWarning> out;
    // the compiler's: unknown parameters, parameters of the wrong kind, unknown targets, unreachable states
    std::shared_ptr<const CompiledController> compiled = compileController(data);
    if (compiled) out = compiled->diagnostics;
    Validator validator(data, resolve, out);
    for (size_t l = 0; l < data.layers.size(); l++){
        const LayerDef& layer = data.layers[l];
        const std::string path = "layers[" + std::to_string(l) + "]";
        validator.machine(layer.machine, path + ".state_machine", validator.anyStateIn(layer.machine));
    }
    for (size_t p = 0; p < data.parameters.size(); p++){
        if (!isLuaIdentifier(data.parameters[p].name)){
            out.push_back({"parameters[" + std::to_string(p) + "].name", "\"" + data.parameters[p].name +
                           "\" is not a Lua-safe name (letters, digits and _, not starting with a digit, not a Lua keyword)"});
        }
        if (!validator.used[p]){
            out.push_back({"parameters[" + std::to_string(p) + "]", "\"" + data.parameters[p].name +
                           "\" is unused: no transition, blend tree or state reads it (an IK constraint of an Animator may)"});
        }
    }
    // one of each (path, message)
    std::vector<ClipWarning> unique;
    for (const ClipWarning& warning : out){
        bool seen = false;
        for (const ClipWarning& kept : unique) seen = seen || (kept.path == warning.path && kept.message == warning.message);
        if (!seen) unique.push_back(warning);
    }
    return unique;
}
