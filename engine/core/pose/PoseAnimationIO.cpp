// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "pose/PoseAnimationIO.h"

#include "io/Data.h"
#include "io/FileData.h"
#include "json.hpp"
#include "pose/PoseDofs.h"
#include "pose/PoseRanges.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>

namespace jasperengine::pose {

    namespace {

        using Json = nlohmann::json;

        // ---- parsing ----

        void warn(std::vector<std::string>& warnings, const std::string& message){
            warnings.push_back(message);
        }

        bool fail(std::string* error, const std::string& message){
            if (error){
                *error = message;
            }
            return false;
        }

        std::string join(const std::string& path, const char* key){
            return path.empty() ? std::string(key) : path + "." + key;
        }

        // Warns about every key of `object` that is not in `known`
        void warnUnknownKeys(const Json& object, std::initializer_list<const char*> known,
                             const std::string& path, std::vector<std::string>& warnings){
            for (auto it = object.begin(); it != object.end(); ++it){
                bool found = false;
                for (const char* name : known){
                    if (it.key() == name){
                        found = true;
                        break;
                    }
                }
                if (!found){
                    warn(warnings, "unknown key '" + it.key() + "'" + (path.empty() ? "" : " in " + path) + " ignored");
                }
            }
        }

        // The object stored at `key`, or nullptr (a value of another type warns)
        const Json* objectAt(const Json& parent, const char* key, const std::string& path,
                             std::vector<std::string>& warnings){
            auto it = parent.find(key);
            if (it == parent.end()){
                return nullptr;
            }
            if (!it->is_object()){
                warn(warnings, join(path, key) + ": expected an object, ignored");
                return nullptr;
            }
            return &*it;
        }

        bool readDouble(const Json& parent, const char* key, double& value, const std::string& path,
                        std::vector<std::string>& warnings){
            auto it = parent.find(key);
            if (it == parent.end()){
                return false;
            }
            if (!it->is_number()){
                warn(warnings, join(path, key) + ": expected a number, ignored");
                return false;
            }
            value = it->get<double>();
            return true;
        }

        void readFloat(const Json& parent, const char* key, float& value, const std::string& path,
                       std::vector<std::string>& warnings){
            double number;
            if (readDouble(parent, key, number, path, warnings)){
                value = static_cast<float>(number);
            }
        }

        void readBool(const Json& parent, const char* key, bool& value, const std::string& path,
                      std::vector<std::string>& warnings){
            auto it = parent.find(key);
            if (it == parent.end()){
                return;
            }
            if (!it->is_boolean()){
                warn(warnings, join(path, key) + ": expected true or false, ignored");
                return;
            }
            value = it->get<bool>();
        }

        void readString(const Json& parent, const char* key, std::string& value, const std::string& path,
                        std::vector<std::string>& warnings){
            auto it = parent.find(key);
            if (it == parent.end()){
                return;
            }
            if (!it->is_string()){
                warn(warnings, join(path, key) + ": expected a string, ignored");
                return;
            }
            value = it->get<std::string>();
        }

        // A whole number: rounded, saturated to [lo, hi] (the range table clamps afterwards)
        void readInteger(const Json& parent, const char* key, long long lo, long long hi, long long& value,
                         const std::string& path, std::vector<std::string>& warnings){
            double number;
            if (!readDouble(parent, key, number, path, warnings)){
                return;
            }
            const double rounded = std::round(number);
            if (rounded < static_cast<double>(lo) || rounded > static_cast<double>(hi)){
                value = rounded < static_cast<double>(lo) ? lo : hi;
                warn(warnings, join(path, key) + ": " + std::to_string(number) + " out of range, clamped to " +
                               std::to_string(value));
            }else{
                value = static_cast<long long>(rounded);
            }
            if (rounded != number){
                warn(warnings, join(path, key) + ": " + std::to_string(number) + " is not a whole number, rounded");
            }
        }

        void readTransition(const Json& object, Transition& transition, const std::string& path,
                            std::vector<std::string>& warnings){
            warnUnknownKeys(object, {"curve", "duration", "overshoot", "anticipation"}, path, warnings);
            auto curve = object.find("curve");
            if (curve != object.end()){
                if (!curve->is_string() || !curveFromName(curve->get<std::string>(), transition.curve)){
                    warn(warnings, path + ".curve: expected spring, linear or smooth, ignored");
                }
            }
            readFloat(object, "duration", transition.duration, path, warnings);
            readFloat(object, "overshoot", transition.overshoot, path, warnings);
            readFloat(object, "anticipation", transition.anticipation, path, warnings);
        }

        void readFeel(const Json& feel, PoseAnimationSettings& settings, std::vector<std::string>& warnings){
            warnUnknownKeys(feel, {"transition", "step_rate", "overlap", "looseness", "weights"}, "feel", warnings);
            if (const Json* transition = objectAt(feel, "transition", "feel", warnings)){
                readTransition(*transition, settings.transition, "feel.transition", warnings);
            }
            readFloat(feel, "step_rate", settings.stepRate, "feel", warnings);
            readFloat(feel, "overlap", settings.overlap, "feel", warnings);
            readFloat(feel, "looseness", settings.looseness, "feel", warnings);
            if (const Json* weights = objectAt(feel, "weights", "feel", warnings)){
                warnUnknownKeys(*weights, {"spine", "head", "arms", "legs"}, "feel.weights", warnings);
                for (int group = 0; group < static_cast<int>(BodyGroup::Count); group++){
                    readFloat(*weights, bodyGroupName(static_cast<BodyGroup>(group)), settings.weights[group],
                              "feel.weights", warnings);
                }
            }
        }

        void readHumanize(const Json& object, HumanizeSettings& humanize, std::vector<std::string>& warnings){
            warnUnknownKeys(object, {"enabled", "joint_limits", "moving_holds", "inbetweens", "life", "seed",
                                     "head_follow", "torso_follow", "springiness"}, "humanize", warnings);
            readBool(object, "enabled", humanize.enabled, "humanize", warnings);
            readBool(object, "joint_limits", humanize.jointLimits, "humanize", warnings);
            readFloat(object, "moving_holds", humanize.movingHolds, "humanize", warnings);
            readFloat(object, "inbetweens", humanize.inbetweens, "humanize", warnings);
            readFloat(object, "life", humanize.life, "humanize", warnings);
            long long seed = humanize.seed;
            readInteger(object, "seed", 0, 0xFFFFFFFFLL, seed, "humanize", warnings);
            humanize.seed = static_cast<uint32_t>(seed);
            readFloat(object, "head_follow", humanize.headFollow, "humanize", warnings);
            readFloat(object, "torso_follow", humanize.torsoFollow, "humanize", warnings);
            readFloat(object, "springiness", humanize.springiness, "humanize", warnings);
        }

        void readPose(const Json& object, PoseKey& key, const std::string& path, std::vector<std::string>& warnings){
            warnUnknownKeys(object, {"name", "hold", "grounded", "lift", "transition", "offset", "angles"}, path, warnings);
            readString(object, "name", key.name, path, warnings);
            readFloat(object, "hold", key.hold, path, warnings);
            readBool(object, "grounded", key.grounded, path, warnings);
            readFloat(object, "lift", key.lift, path, warnings);

            auto transition = object.find("transition");
            if (transition != object.end() && !transition->is_null()){
                if (transition->is_object()){
                    key.customTransition = true;
                    readTransition(*transition, key.transition, path + ".transition", warnings);
                }else{
                    warn(warnings, path + ".transition: expected null or an object, ignored");
                }
            }

            if (const Json* offset = objectAt(object, "offset", path, warnings)){
                warnUnknownKeys(*offset, {"left", "forward"}, path + ".offset", warnings);
                readFloat(*offset, "left", key.body.offset.x, path + ".offset", warnings);
                readFloat(*offset, "forward", key.body.offset.z, path + ".offset", warnings);
            }

            if (const Json* angles = objectAt(object, "angles", path, warnings)){
                for (auto it = angles->begin(); it != angles->end(); ++it){
                    int dof;
                    if (!dofFromName(it.key(), dof)){
                        warn(warnings, path + ".angles: unknown DOF '" + it.key() + "' ignored");
                    }else if (!it->is_number()){
                        warn(warnings, path + ".angles." + it.key() + ": expected a number, ignored");
                    }else{
                        key.body.angles[dof] = static_cast<float>(it->get<double>());
                    }
                }
            }
        }

        void readTemplate(const Json& object, PoseAnimationTemplateInfo& info, std::vector<std::string>& warnings){
            warnUnknownKeys(object, {"name", "description", "category"}, "template", warnings);
            readString(object, "name", info.name, "template", warnings);
            readString(object, "description", info.description, "template", warnings);
            readString(object, "category", info.category, "template", warnings);
        }

        // ---- writing ----

        // A number rounded to 4 decimals without trailing zeros ("0.12", "25", "0")
        std::string number(double value){
            if (!std::isfinite(value)){
                return "0";
            }
            char buffer[64];
            std::snprintf(buffer, sizeof(buffer), "%.4f", value);
            std::string text = buffer;
            size_t end = text.size();
            while (end > 0 && text[end - 1] == '0'){
                end--;
            }
            if (end > 0 && text[end - 1] == '.'){
                end--;
            }
            text.resize(end);
            if (text == "-0" || text.empty()){
                text = "0";
            }
            return text;
        }

        std::string quote(const std::string& text){
            return Json(text).dump(-1, ' ', false, Json::error_handler_t::replace);
        }

        std::string boolean(bool value){
            return value ? "true" : "false";
        }

        std::string transitionText(const Transition& transition){
            return std::string("{\"curve\": ") + quote(curveName(transition.curve)) +
                   ", \"duration\": " + number(transition.duration) +
                   ", \"overshoot\": " + number(transition.overshoot) +
                   ", \"anticipation\": " + number(transition.anticipation) + "}";
        }

        // The angles object, entries in DOF order, wrapped to lines of about 100 columns
        std::string anglesText(const PoseKey& key, const std::string& indent){
            std::vector<std::string> entries;
            for (int dof = 0; dof < DOF_COUNT; dof++){
                const float angle = key.body.angles[dof];
                if (std::fabs(angle) >= 0.0005f){
                    entries.push_back(quote(dofName(dof)) + ": " + number(angle));
                }
            }
            if (entries.empty()){
                return "{}";
            }
            std::string text = "{";
            size_t column = indent.size() + 11;   // after `"angles": {`
            for (size_t i = 0; i < entries.size(); i++){
                const bool last = (i + 1 == entries.size());
                const size_t width = entries[i].size() + 2;
                if (i > 0 && column + width > 100){
                    text += "\n" + indent + "           ";
                    column = indent.size() + 11;
                }else if (i > 0){
                    text += " ";
                    column++;
                }
                text += entries[i];
                text += last ? "}" : ",";
                column += width - 1;
            }
            return text;
        }

        std::string keyText(const PoseKey& key){
            const std::string indent = "     ";
            std::string text = "    {\"name\": " + quote(key.name) + ", \"hold\": " + number(key.hold) +
                               ", \"grounded\": " + boolean(key.grounded) + ", \"lift\": " + number(key.lift) + ",\n";
            text += indent + "\"transition\": " + (key.customTransition ? transitionText(key.transition) : "null") + ",\n";
            text += indent + "\"offset\": {\"left\": " + number(key.body.offset.x) +
                    ", \"forward\": " + number(key.body.offset.z) + "},\n";
            text += indent + "\"angles\": " + anglesText(key, indent) + "}";
            return text;
        }

    }

    bool parsePoseAnimation(const std::string& text, PoseAnimationData& out,
                            std::vector<std::string>& warnings, std::string* error,
                            std::optional<PoseAnimationTemplateInfo>* templateInfo){
        out = PoseAnimationData();
        if (templateInfo){
            templateInfo->reset();
        }

        Json root = Json::parse(text, nullptr, false);
        if (root.is_discarded()){
            return fail(error, "not valid JSON");
        }
        if (!root.is_object()){
            return fail(error, "not a pose animation: the top level must be a JSON object");
        }

        auto format = root.find("format");
        if (format == root.end() || !format->is_string() || format->get<std::string>() != POSEANIM_FORMAT){
            return fail(error, std::string("not a pose animation: \"format\" must be \"") + POSEANIM_FORMAT + "\"");
        }
        auto version = root.find("version");
        if (version == root.end()){
            warn(warnings, "version: missing, assuming 1");
        }else{
            if (!version->is_number()){
                return fail(error, "\"version\" must be a number");
            }
            const double value = version->get<double>();
            if (value != std::floor(value) || value < 1.0){
                return fail(error, "invalid \"version\": " + number(value));
            }
            if (value > POSEANIM_VERSION){
                return fail(error, "unsupported pose animation version " + number(value) +
                                   " (this engine reads versions up to " + std::to_string(POSEANIM_VERSION) + ")");
            }
        }

        warnUnknownKeys(root, {"format", "version", "template", "preview_model", "frame_rate", "loop", "feel",
                               "humanize", "poses"}, "", warnings);

        if (const Json* info = objectAt(root, "template", "", warnings)){
            PoseAnimationTemplateInfo parsed;
            readTemplate(*info, parsed, warnings);
            if (templateInfo){
                *templateInfo = parsed;
            }
        }

        PoseAnimationSettings& settings = out.settings;
        readString(root, "preview_model", out.previewModel, "", warnings);
        long long frameRate = settings.frameRate;
        readInteger(root, "frame_rate", -1000000, 1000000, frameRate, "", warnings);
        settings.frameRate = static_cast<int>(frameRate);
        readBool(root, "loop", settings.loop, "", warnings);
        if (const Json* feel = objectAt(root, "feel", "", warnings)){
            readFeel(*feel, settings, warnings);
        }
        if (const Json* humanize = objectAt(root, "humanize", "", warnings)){
            readHumanize(*humanize, settings.humanize, warnings);
        }

        auto poses = root.find("poses");
        if (poses != root.end()){
            if (!poses->is_array()){
                warn(warnings, "poses: expected an array, ignored");
            }else{
                for (size_t i = 0; i < poses->size(); i++){
                    const std::string path = "poses[" + std::to_string(i) + "]";
                    PoseKey key;
                    if (!(*poses)[i].is_object()){
                        warn(warnings, path + ": expected an object, replaced by a default pose");
                    }else{
                        readPose((*poses)[i], key, path, warnings);
                    }
                    out.poses.push_back(std::move(key));
                }
            }
        }

        std::vector<ClampReport> reports;
        clampData(out, reports);
        for (const ClampReport& report : reports){
            warn(warnings, "out of range, clamped: " + describeClamp(report));
        }
        return true;
    }

    std::string writePoseAnimation(const PoseAnimationData& data, const PoseAnimationTemplateInfo* templateInfo){
        const PoseAnimationSettings& settings = data.settings;
        const HumanizeSettings& humanize = settings.humanize;

        std::string text = "{\n";
        text += std::string("  \"format\": ") + quote(POSEANIM_FORMAT) + ",\n";
        text += "  \"version\": " + std::to_string(POSEANIM_VERSION) + ",\n";
        if (templateInfo){
            text += "  \"template\": {\"name\": " + quote(templateInfo->name) +
                    ", \"description\": " + quote(templateInfo->description) +
                    ", \"category\": " + quote(templateInfo->category) + "},\n";
        }
        text += "  \"preview_model\": " + quote(data.previewModel) + ",\n";
        text += "  \"frame_rate\": " + std::to_string(settings.frameRate) + ",\n";
        text += "  \"loop\": " + boolean(settings.loop) + ",\n";

        text += "  \"feel\": {\n";
        text += "    \"transition\": " + transitionText(settings.transition) + ",\n";
        text += "    \"step_rate\": " + number(settings.stepRate) + ", \"overlap\": " + number(settings.overlap) +
                ", \"looseness\": " + number(settings.looseness) + ",\n";
        text += "    \"weights\": {";
        for (int group = 0; group < static_cast<int>(BodyGroup::Count); group++){
            text += std::string(group > 0 ? ", " : "") + quote(bodyGroupName(static_cast<BodyGroup>(group))) + ": " +
                    number(settings.weights[group]);
        }
        text += "}\n  },\n";

        text += "  \"humanize\": {\"enabled\": " + boolean(humanize.enabled) +
                ", \"joint_limits\": " + boolean(humanize.jointLimits) +
                ", \"moving_holds\": " + number(humanize.movingHolds) +
                ", \"inbetweens\": " + number(humanize.inbetweens) + ",\n";
        text += "               \"life\": " + number(humanize.life) + ", \"seed\": " + std::to_string(humanize.seed) +
                ", \"head_follow\": " + number(humanize.headFollow) +
                ", \"torso_follow\": " + number(humanize.torsoFollow) +
                ", \"springiness\": " + number(humanize.springiness) + "},\n";

        if (data.poses.empty()){
            text += "  \"poses\": []\n";
        }else{
            text += "  \"poses\": [\n";
            for (size_t i = 0; i < data.poses.size(); i++){
                text += keyText(data.poses[i]);
                text += (i + 1 < data.poses.size()) ? ",\n" : "\n";
            }
            text += "  ]\n";
        }
        text += "}\n";
        return text;
    }

    bool readTextFile(const std::string& path, std::string& text, std::string* error){
        Data data;
        if (data.open(path.c_str()) != FileErrors::FILEDATA_OK){
            return fail(error, "cannot read '" + path + "'");
        }
        text.assign(reinterpret_cast<const char*>(data.getMemPtr()), data.length());
        return true;
    }

    bool writeTextFile(const std::string& path, const std::string& text, std::string* error){
        namespace fs = std::filesystem;
        const std::string systemPath = FileData::getSystemPath(path);
        const std::string temporary = systemPath + ".tmp";
        {
            std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
            if (!file){
                return fail(error, "cannot write '" + path + "'");
            }
            file.write(text.data(), static_cast<std::streamsize>(text.size()));
            file.close();
            if (!file){
                std::error_code ignored;
                fs::remove(temporary, ignored);
                return fail(error, "failed writing '" + path + "'");
            }
        }
        std::error_code code;
        fs::rename(temporary, systemPath, code);
        if (code){
            fs::remove(temporary, code);
            return fail(error, "cannot replace '" + path + "'");
        }
        return true;
    }

    bool loadPoseAnimationFile(const std::string& path, PoseAnimationData& out,
                               std::vector<std::string>& warnings, std::string* error,
                               std::optional<PoseAnimationTemplateInfo>* templateInfo){
        std::string text;
        if (!readTextFile(path, text, error)){
            return false;
        }
        std::string parseError;
        if (!parsePoseAnimation(text, out, warnings, &parseError, templateInfo)){
            return fail(error, path + ": " + parseError);
        }
        return true;
    }

    bool savePoseAnimationFile(const std::string& path, const PoseAnimationData& data, std::string* error,
                               const PoseAnimationTemplateInfo* templateInfo){
        return writeTextFile(path, writePoseAnimation(data, templateInfo), error);
    }

}
