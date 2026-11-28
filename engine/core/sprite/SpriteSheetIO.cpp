// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SpriteSheetIO.h"

#include <algorithm>
#include <cstdlib>

#include <cmath>
#include <cstdio>
#include <functional>
#include <set>

namespace jasperengine{

    // ---- SpriteSheet.h

    const char* spriteTagDirectionName(SpriteTagDirection direction){
        switch (direction){
            case SpriteTagDirection::FORWARD: return "forward";
            case SpriteTagDirection::REVERSE: return "reverse";
            case SpriteTagDirection::PING_PONG: return "pingpong";
            case SpriteTagDirection::PING_PONG_REVERSE: return "pingpong_reverse";
        }
        return "forward";
    }

    bool spriteTagDirectionFromName(std::string_view name, SpriteTagDirection& out){
        for (SpriteTagDirection d : {SpriteTagDirection::FORWARD, SpriteTagDirection::REVERSE, SpriteTagDirection::PING_PONG,
                                     SpriteTagDirection::PING_PONG_REVERSE}){
            if (name == spriteTagDirectionName(d)){
                out = d;
                return true;
            }
        }
        return false;
    }

    namespace{
        // one pass of a ping-pong tag after the first: the far end is not repeated
        void appendRange(std::vector<int>& out, int first, int last){
            if (first <= last){
                for (int f = first; f <= last; f++) out.push_back(f);
            }else{
                for (int f = first; f >= last; f--) out.push_back(f);
            }
        }

        std::vector<int> tagPlayback(const SpriteTag& tag, int passes, bool loops){
            std::vector<int> out;
            const int from = std::min(tag.from, tag.to);
            const int to = std::max(tag.from, tag.to);
            if (from == to){
                out.push_back(from);
                return out;
            }
            const bool pingPong = tag.direction == SpriteTagDirection::PING_PONG || tag.direction == SpriteTagDirection::PING_PONG_REVERSE;
            const bool startsDown = tag.direction == SpriteTagDirection::REVERSE || tag.direction == SpriteTagDirection::PING_PONG_REVERSE;
            if (!pingPong){
                for (int p = 0; p < passes; p++) startsDown ? appendRange(out, to, from) : appendRange(out, from, to);
                return out;
            }
            // passes count directions (Aseprite: repeat 1 plays one direction, 2 there and back);
            // every pass after the first starts past the turning frame, and a looping clip also
            // stops short of the frame it loops back to
            for (int p = 0; p < passes; p++){
                const bool down = (p % 2 == 1) != startsDown;
                const bool last = p == passes - 1;
                int a = down ? to : from;
                int b = down ? from : to;
                if (p > 0) a += down ? -1 : 1;
                if (last && loops && p > 0) b += down ? 1 : -1;
                if ((down && a >= b) || (!down && a <= b)) appendRange(out, a, b);
            }
            return out;
        }
    }

    std::vector<int> SpriteTag::sequence() const{
        const bool pingPong = direction == SpriteTagDirection::PING_PONG || direction == SpriteTagDirection::PING_PONG_REVERSE;
        return tagPlayback(*this, pingPong ? 2 : 1, true);
    }

    std::vector<int> SpriteTag::playback() const{
        if (repeat <= 0) return sequence();
        // at most Aseprite's 16-bit count, and a playback no longer than SPRITE_TAG_MAX_PLAYBACK
        // frames (a long tag repeated many times is not unrolled into gigabytes)
        const int span = std::abs(to - from) + 1;
        const int passes = std::clamp(repeat, 1, SPRITE_TAG_MAX_REPEAT);
        const int fit = std::max(1, static_cast<int>(SPRITE_TAG_MAX_PLAYBACK / static_cast<size_t>(std::max(1, span))));
        return tagPlayback(*this, std::min(passes, fit), false);
    }

    const SpriteSliceKey* SpriteSlice::keyAt(int frame) const{
        const SpriteSliceKey* found = nullptr;
        for (const SpriteSliceKey& key : keys){
            if (key.frame <= frame) found = &key;
            else break;
        }
        if (found && found->hidden()) return nullptr;
        return found;
    }

    int SpriteSheetData::findFrame(std::string_view name) const{
        for (size_t i = 0; i < frames.size(); i++){
            if (frames[i].name == name) return static_cast<int>(i);
        }
        return -1;
    }

    int SpriteSheetData::findTag(std::string_view name) const{
        for (size_t i = 0; i < tags.size(); i++){
            if (tags[i].name == name) return static_cast<int>(i);
        }
        return -1;
    }

    int SpriteSheetData::findSlice(std::string_view name) const{
        for (size_t i = 0; i < slices.size(); i++){
            if (slices[i].name == name) return static_cast<int>(i);
        }
        return -1;
    }

    // ---- colours

    std::string spriteColorText(const Vector4& color){
        auto byte = [](float v){ return static_cast<int>(std::lround(std::fmin(1.0f, std::fmax(0.0f, v)) * 255.0f)); };
        char buffer[16];
        std::snprintf(buffer, sizeof(buffer), "#%02x%02x%02x%02x", byte(color.x), byte(color.y), byte(color.z), byte(color.w));
        return buffer;
    }

    bool spriteColorFromText(const std::string& text, Vector4& out){
        if ((text.size() != 9 && text.size() != 7) || text[0] != '#') return false;
        unsigned int v[4] = {0, 0, 0, 255};
        for (size_t i = 0; i < (text.size() - 1) / 2; i++){
            unsigned int b = 0;
            for (size_t k = 0; k < 2; k++){
                const char c = text[1 + i * 2 + k];
                b <<= 4;
                if (c >= '0' && c <= '9') b |= static_cast<unsigned int>(c - '0');
                else if (c >= 'a' && c <= 'f') b |= static_cast<unsigned int>(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') b |= static_cast<unsigned int>(c - 'A' + 10);
                else return false;
            }
            v[i] = b;
        }
        out = Vector4(v[0] / 255.0f, v[1] / 255.0f, v[2] / 255.0f, v[3] / 255.0f);
        return true;
    }

    // ---- parse

    namespace{

        struct Parser{
            std::vector<std::string>& warnings;

            void warn(const std::string& path, const std::string& what){ warnings.push_back(path + ": " + what); }

            bool integer(const nlohmann::json& v, const std::string& path, int& out){
                if (!v.is_number()){
                    warn(path, "not a number");
                    return false;
                }
                const double d = v.get<double>();
                if (!std::isfinite(d) || std::fabs(d) > 1e9){
                    warn(path, "out of range");
                    return false;
                }
                out = static_cast<int>(std::lround(d));
                return true;
            }

            // an array of `n` integers
            bool ints(const nlohmann::json& v, const std::string& path, size_t n, int* out){
                if (!v.is_array() || v.size() != n){
                    warn(path, "expected " + std::to_string(n) + " integers");
                    return false;
                }
                for (size_t i = 0; i < n; i++){
                    if (!integer(v[i], path + "[" + std::to_string(i) + "]", out[i])) return false;
                }
                return true;
            }

            nlohmann::json userData(const nlohmann::json& parent, const std::string& path){
                if (!parent.contains("user_data")) return nullptr;
                const nlohmann::json& v = parent["user_data"];
                if (v.is_null()) return nullptr;
                if (!v.is_object()){
                    warn(path + ".user_data", "not an object (dropped)");
                    return nullptr;
                }
                return v.empty() ? nlohmann::json() : v;
            }
        };

    }

    SpriteSheetParseResult parseSheet(const std::string& text){
        SpriteSheetParseResult result;
        const nlohmann::json root = nlohmann::json::parse(text, nullptr, false);
        if (root.is_discarded()){
            result.error = "not JSON";
            return result;
        }
        if (!root.is_object()){
            result.error = "the root is not an object";
            return result;
        }
        if (!root.contains("format") || !root["format"].is_string() || root["format"].get<std::string>() != SPRITESHEET_FORMAT){
            result.error = std::string("not a sprite sheet (\"format\" must be \"") + SPRITESHEET_FORMAT + "\")";
            return result;
        }
        if (!root.contains("version") || !root["version"].is_number_integer()){
            result.error = "no \"version\"";
            return result;
        }
        const int version = root["version"].get<int>();
        if (version > SPRITESHEET_VERSION || version < 1){
            result.error = "sprite sheet version " + std::to_string(version) + " is newer than this engine reads (" +
                           std::to_string(SPRITESHEET_VERSION) + ")";
            return result;
        }

        Parser p{result.warnings};
        SpriteSheetData sheet;
        if (root.contains("source") && root["source"].is_string()) sheet.source = root["source"].get<std::string>();
        if (root.contains("source_guid") && root["source_guid"].is_string()){
            const std::string g = root["source_guid"].get<std::string>();
            // "" is what writeSheet writes for a source without a GUID (S45's texture sheets)
            if (Guid::isValidText(g)) sheet.sourceGuid = Guid::parse(g);
            else if (!g.empty()) p.warn("source_guid", "not a GUID (dropped)");
        }
        if (root.contains("pixels_per_unit")){
            int ppu = 1;
            if (p.integer(root["pixels_per_unit"], "pixels_per_unit", ppu) && ppu > 0) sheet.pixelsPerUnit = static_cast<float>(ppu);
        }
        if (root.contains("pixel_ratio")){
            int r[2] = {1, 1};
            if (p.ints(root["pixel_ratio"], "pixel_ratio", 2, r) && r[0] > 0 && r[1] > 0) sheet.pixelRatio = Vector2(static_cast<float>(r[0]), static_cast<float>(r[1]));
        }

        if (root.contains("pages") && root["pages"].is_array()){
            const nlohmann::json& pages = root["pages"];
            for (size_t i = 0; i < pages.size(); i++){
                const std::string path = "pages[" + std::to_string(i) + "]";
                const nlohmann::json& pj = pages[i];
                SpritePage page;
                int size[2] = {0, 0};
                if (!pj.is_object() || !pj.contains("texture") || !pj["texture"].is_string()){
                    p.warn(path, "no texture (the page is empty)");
                }else{
                    page.texture = pj["texture"].get<std::string>();
                }
                if (pj.is_object() && pj.contains("size") && p.ints(pj["size"], path + ".size", 2, size)){
                    page.width = std::max(0, size[0]);
                    page.height = std::max(0, size[1]);
                }
                // pages keep their index even when broken: frames refer to them by number
                sheet.pages.push_back(page);
            }
        }

        std::set<std::string> names;
        if (root.contains("frames") && root["frames"].is_array()){
            const nlohmann::json& frames = root["frames"];
            for (size_t i = 0; i < frames.size(); i++){
                const std::string path = "frames[" + std::to_string(i) + "]";
                const nlohmann::json& fj = frames[i];
                if (!fj.is_object() || !fj.contains("name") || !fj["name"].is_string()){
                    p.warn(path, "no name (the frame is dropped)");
                    continue;
                }
                SpriteFrame frame;
                frame.name = fj["name"].get<std::string>();
                if (fj.contains("key") && fj["key"].is_string()) frame.key = fj["key"].get<std::string>();
                int page = 0;
                if (fj.contains("page") && (!p.integer(fj["page"], path + ".page", page) || page < 0 || page >= static_cast<int>(sheet.pages.size()))){
                    p.warn(path + ".page", "no such page (the frame is dropped)");
                    continue;
                }
                frame.page = static_cast<uint16_t>(page);
                int rect[4];
                if (!fj.contains("rect") || !p.ints(fj["rect"], path + ".rect", 4, rect) || rect[2] < 0 || rect[3] < 0){
                    p.warn(path + ".rect", "no valid rect (the frame is dropped)");
                    continue;
                }
                frame.rect = Rect(static_cast<float>(rect[0]), static_cast<float>(rect[1]), static_cast<float>(rect[2]), static_cast<float>(rect[3]));
                int size[2] = {rect[2], rect[3]};
                if (fj.contains("source_size")) p.ints(fj["source_size"], path + ".source_size", 2, size);
                frame.sourceSize = Vector2(static_cast<float>(std::max(0, size[0])), static_cast<float>(std::max(0, size[1])));
                int offset[2] = {0, 0};
                if (fj.contains("offset")) p.ints(fj["offset"], path + ".offset", 2, offset);
                frame.offset = Vector2(static_cast<float>(offset[0]), static_cast<float>(offset[1]));
                int pivot[2] = {size[0] / 2, size[1] / 2};
                if (fj.contains("pivot")) p.ints(fj["pivot"], path + ".pivot", 2, pivot);
                frame.pivot = Vector2(static_cast<float>(pivot[0]), static_cast<float>(pivot[1]));
                int border[4] = {0, 0, 0, 0};
                if (fj.contains("border")) p.ints(fj["border"], path + ".border", 4, border);
                frame.border = Vector4(static_cast<float>(std::max(0, border[0])), static_cast<float>(std::max(0, border[1])),
                                       static_cast<float>(std::max(0, border[2])), static_cast<float>(std::max(0, border[3])));
                int duration = 100;
                if (fj.contains("duration_ms") && p.integer(fj["duration_ms"], path + ".duration_ms", duration) && duration < 1){
                    p.warn(path + ".duration_ms", "below 1 ms (set to 1)");
                    duration = 1;
                }
                frame.durationMs = static_cast<uint32_t>(std::max(1, duration));
                if (!names.insert(frame.name).second){
                    std::string unique;
                    for (int k = 2;; k++){
                        unique = frame.name + " " + std::to_string(k);
                        if (!names.count(unique)) break;
                    }
                    p.warn(path + ".name", "'" + frame.name + "' is already a frame (renamed '" + unique + "')");
                    frame.name = unique;
                    names.insert(unique);
                }
                sheet.frames.push_back(std::move(frame));
            }
        }

        const int frameCount = static_cast<int>(sheet.frames.size());
        if (root.contains("tags") && root["tags"].is_array()){
            const nlohmann::json& tags = root["tags"];
            for (size_t i = 0; i < tags.size(); i++){
                const std::string path = "tags[" + std::to_string(i) + "]";
                const nlohmann::json& tj = tags[i];
                if (!tj.is_object() || !tj.contains("name") || !tj["name"].is_string()){
                    p.warn(path, "no name (the tag is dropped)");
                    continue;
                }
                SpriteTag tag;
                tag.name = tj["name"].get<std::string>();
                if (!tj.contains("from") || !tj.contains("to") || !p.integer(tj["from"], path + ".from", tag.from) ||
                    !p.integer(tj["to"], path + ".to", tag.to) || tag.from < 0 || tag.to < tag.from || tag.to >= frameCount){
                    p.warn(path, "frames out of range (the tag is dropped)");
                    continue;
                }
                if (tj.contains("direction") && (!tj["direction"].is_string() || !spriteTagDirectionFromName(tj["direction"].get<std::string>(), tag.direction))){
                    p.warn(path + ".direction", "unknown direction (forward)");
                }
                if (tj.contains("repeat") && p.integer(tj["repeat"], path + ".repeat", tag.repeat) && tag.repeat < 0) tag.repeat = 0;
                if (tag.repeat > SPRITE_TAG_MAX_REPEAT){
                    // Aseprite stores the count in 16 bits; more would unroll into a huge playback
                    p.warn(path + ".repeat", std::to_string(tag.repeat) + " is more than " + std::to_string(SPRITE_TAG_MAX_REPEAT) + " (kept at that)");
                    tag.repeat = SPRITE_TAG_MAX_REPEAT;
                }
                if (tj.contains("color") && (!tj["color"].is_string() || !spriteColorFromText(tj["color"].get<std::string>(), tag.color))){
                    p.warn(path + ".color", "not #rrggbbaa (black)");
                }
                tag.userData = p.userData(tj, path);
                sheet.tags.push_back(std::move(tag));
            }
        }

        if (root.contains("slices") && root["slices"].is_array()){
            const nlohmann::json& slices = root["slices"];
            for (size_t i = 0; i < slices.size(); i++){
                const std::string path = "slices[" + std::to_string(i) + "]";
                const nlohmann::json& sj = slices[i];
                if (!sj.is_object() || !sj.contains("name") || !sj["name"].is_string()){
                    p.warn(path, "no name (the slice is dropped)");
                    continue;
                }
                SpriteSlice slice;
                slice.name = sj["name"].get<std::string>();
                if (sj.contains("keys") && sj["keys"].is_array()){
                    const nlohmann::json& keys = sj["keys"];
                    for (size_t k = 0; k < keys.size(); k++){
                        const std::string kpath = path + ".keys[" + std::to_string(k) + "]";
                        const nlohmann::json& kj = keys[k];
                        SpriteSliceKey key;
                        int bounds[4];
                        if (!kj.is_object() || !kj.contains("frame") || !p.integer(kj["frame"], kpath + ".frame", key.frame) || key.frame < 0 ||
                            !kj.contains("bounds") || !p.ints(kj["bounds"], kpath + ".bounds", 4, bounds)){
                            p.warn(kpath, "no frame or bounds (the key is dropped)");
                            continue;
                        }
                        if (!slice.keys.empty() && key.frame <= slice.keys.back().frame){
                            p.warn(kpath + ".frame", "keys must ascend (the key is dropped)");
                            continue;
                        }
                        key.bounds = Rect(static_cast<float>(bounds[0]), static_cast<float>(bounds[1]), static_cast<float>(std::max(0, bounds[2])),
                                          static_cast<float>(std::max(0, bounds[3])));
                        int center[4];
                        if (kj.contains("center") && p.ints(kj["center"], kpath + ".center", 4, center)){
                            key.hasCenter = true;
                            key.center = Rect(static_cast<float>(center[0]), static_cast<float>(center[1]), static_cast<float>(center[2]), static_cast<float>(center[3]));
                        }
                        int pivot[2];
                        if (kj.contains("pivot") && p.ints(kj["pivot"], kpath + ".pivot", 2, pivot)){
                            key.hasPivot = true;
                            key.pivot = Vector2(static_cast<float>(pivot[0]), static_cast<float>(pivot[1]));
                        }
                        slice.keys.push_back(key);
                    }
                }
                slice.userData = p.userData(sj, path);
                sheet.slices.push_back(std::move(slice));
            }
        }
        if (root.contains("skins") && root["skins"].is_array()){
            const nlohmann::json& skins = root["skins"];
            for (size_t i = 0; i < skins.size(); i++){
                const std::string path = "skins[" + std::to_string(i) + "]";
                int frame = -1;
                if (!skins[i].is_object() || !skins[i].contains("frame") || !p.integer(skins[i]["frame"], path + ".frame", frame) ||
                    frame < 0 || frame >= frameCount){
                    p.warn(path, "no such frame (the skin is dropped)");
                    continue;
                }
                std::string why;
                std::shared_ptr<SpriteSkinGeometry> skin = parseSpriteSkin(skins[i], why);
                if (!skin){
                    p.warn(path, why + " (the skin is dropped)");
                    continue;
                }
                sheet.frames[static_cast<size_t>(frame)].skin = std::move(skin);
            }
        }
        sheet.userData = p.userData(root, "");
        result.sheet = std::move(sheet);
        return result;
    }

    std::shared_ptr<SpriteSkinGeometry> parseSpriteSkin(const nlohmann::json& j, std::string& error){
        auto skin = std::make_shared<SpriteSkinGeometry>();
        auto number = [](const nlohmann::json& v, double& out){
            if (!v.is_number()) return false;
            out = v.get<double>();
            return std::isfinite(out) && std::fabs(out) <= 1e9;
        };
        auto point = [&](const nlohmann::json& v, Vector2& out){
            double x = 0, y = 0;
            if (!v.is_array() || v.size() != 2 || !number(v[0], x) || !number(v[1], y)) return false;
            out = Vector2(static_cast<float>(x), static_cast<float>(y));
            return true;
        };
        if (j.contains("bones")){
            if (!j["bones"].is_array() || j["bones"].size() > static_cast<size_t>(SPRITE_SKIN_MAX_BONES)){
                error = "bones is not a list of at most " + std::to_string(SPRITE_SKIN_MAX_BONES);
                return nullptr;
            }
            for (size_t b = 0; b < j["bones"].size(); b++){
                const nlohmann::json& bj = j["bones"][b];
                SpriteBone bone;
                double parent = -1;
                if (!bj.is_object() || !bj.contains("name") || !bj["name"].is_string() || !bj.contains("head") || !point(bj["head"], bone.head) ||
                    !bj.contains("tail") || !point(bj["tail"], bone.tail)){
                    error = "bones[" + std::to_string(b) + "] needs a name, a head and a tail";
                    return nullptr;
                }
                bone.name = bj["name"].get<std::string>();
                if (bj.contains("parent") && (!number(bj["parent"], parent) || parent < -1 || parent >= static_cast<double>(b))){
                    // parents come first: a bone's world pose needs its parent's
                    error = "bones[" + std::to_string(b) + "].parent must be -1 or an earlier bone";
                    return nullptr;
                }
                bone.parent = static_cast<int>(parent);
                skin->bones.push_back(std::move(bone));
            }
        }
        if (j.contains("vertices")){
            if (!j["vertices"].is_array() || j["vertices"].size() > SPRITE_SKIN_MAX_VERTICES){
                error = "vertices is not a list of at most " + std::to_string(SPRITE_SKIN_MAX_VERTICES) + " points";
                return nullptr;
            }
            for (size_t v = 0; v < j["vertices"].size(); v++){
                Vector2 at;
                if (!point(j["vertices"][v], at)){
                    error = "vertices[" + std::to_string(v) + "] is not [x, y]";
                    return nullptr;
                }
                skin->vertices.push_back(at);
            }
        }
        if (j.contains("indices")){
            if (!j["indices"].is_array() || j["indices"].size() % 3 != 0){
                error = "indices is not a list of triangles (a multiple of 3)";
                return nullptr;
            }
            for (const nlohmann::json& iv : j["indices"]){
                double index = 0;
                if (!number(iv, index) || index < 0 || index >= static_cast<double>(skin->vertices.size())){
                    error = "indices names a vertex that is not there";
                    return nullptr;
                }
                skin->indices.push_back(static_cast<uint16_t>(index));
            }
        }
        if (j.contains("weights") && j["weights"].is_array() && !j["weights"].empty()){
            const nlohmann::json& wj = j["weights"];
            if (wj.size() != skin->vertices.size()){
                error = "weights has " + std::to_string(wj.size()) + " entries for " + std::to_string(skin->vertices.size()) + " vertices";
                return nullptr;
            }
            for (size_t v = 0; v < wj.size(); v++){
                SpriteVertexWeights w;
                if (!wj[v].is_array() || wj[v].size() > static_cast<size_t>(SPRITE_SKIN_MAX_INFLUENCES)){
                    error = "weights[" + std::to_string(v) + "] is not a list of at most 4 [bone, weight]";
                    return nullptr;
                }
                for (size_t k = 0; k < wj[v].size(); k++){
                    double bone = 0, weight = 0;
                    const nlohmann::json& pair = wj[v][k];
                    if (!pair.is_array() || pair.size() != 2 || !number(pair[0], bone) || !number(pair[1], weight) || bone < 0 ||
                        bone >= static_cast<double>(skin->bones.size()) || weight < 0){
                        error = "weights[" + std::to_string(v) + "][" + std::to_string(k) + "] is not [bone, weight >= 0] of a bone that is there";
                        return nullptr;
                    }
                    w.bones[k] = static_cast<uint8_t>(bone);
                    w.weights[k] = static_cast<float>(weight);
                }
                skin->weights.push_back(w);
            }
            normalizeSpriteWeights(*skin);
        }
        return skin;
    }

    void normalizeSpriteWeights(SpriteSkinGeometry& skin){
        for (SpriteVertexWeights& w : skin.weights){
            float sum = 0.0f;
            for (float x : w.weights) sum += x;
            if (sum <= 0.0f){
                // an unweighted vertex follows the first bone (a rigid part stays rigid)
                w = SpriteVertexWeights();
                w.weights[0] = skin.bones.empty() ? 0.0f : 1.0f;
                continue;
            }
            for (float& x : w.weights) x /= sum;
        }
    }

    // ---- write

    namespace{

        std::string quoted(const std::string& s){ return nlohmann::json(s).dump(); }

        std::string num(float v){ return std::to_string(static_cast<long long>(std::llround(v))); }

        std::string pair(float a, float b){ return "[" + num(a) + ", " + num(b) + "]"; }

        std::string rect(const Rect& r){ return "[" + num(r.getX()) + ", " + num(r.getY()) + ", " + num(r.getWidth()) + ", " + num(r.getHeight()) + "]"; }

        std::string userData(const nlohmann::json& ud){ return ud.is_object() && !ud.empty() ? ud.dump() : std::string(); }

        // a weight as 6 significant digits: the text a float parsed from it prints again
        std::string weightText(float w){
            char buffer[32];
            std::snprintf(buffer, sizeof(buffer), "%.6g", static_cast<double>(w));
            return buffer;
        }

    }

    std::string writeSpriteSkin(const SpriteSkinGeometry& skin){
        std::string s = "\"bones\": [";
        for (size_t b = 0; b < skin.bones.size(); b++){
            const SpriteBone& bone = skin.bones[b];
            s += (b ? ", " : "") + std::string("{\"name\": ") + quoted(bone.name) + ", \"parent\": " + std::to_string(bone.parent) +
                 ", \"head\": " + pair(bone.head.x, bone.head.y) + ", \"tail\": " + pair(bone.tail.x, bone.tail.y) + "}";
        }
        s += "], \"vertices\": [";
        for (size_t v = 0; v < skin.vertices.size(); v++){
            s += (v ? ", " : "") + pair(skin.vertices[v].x, skin.vertices[v].y);
        }
        s += "], \"indices\": [";
        for (size_t i = 0; i < skin.indices.size(); i++){
            s += (i ? ", " : "") + std::to_string(skin.indices[i]);
        }
        s += "], \"weights\": [";
        for (size_t v = 0; v < skin.weights.size(); v++){
            const SpriteVertexWeights& w = skin.weights[v];
            s += v ? ", [" : "[";
            bool first = true;
            for (int k = 0; k < SPRITE_SKIN_MAX_INFLUENCES; k++){
                if (w.weights[k] <= 0.0f) continue;
                s += (first ? "" : ", ") + std::string("[") + std::to_string(w.bones[k]) + ", " + weightText(w.weights[k]) + "]";
                first = false;
            }
            s += "]";
        }
        s += "]";
        return s;
    }

    std::string writeSheet(const SpriteSheetData& sheet){
        std::string out;
        out += "{\n";
        out += "  \"format\": " + quoted(SPRITESHEET_FORMAT) + ",\n";
        out += "  \"version\": " + std::to_string(SPRITESHEET_VERSION) + ",\n";
        out += "  \"source\": " + quoted(sheet.source) + ",\n";
        out += "  \"source_guid\": " + quoted(sheet.sourceGuid.isNull() ? std::string() : sheet.sourceGuid.str()) + ",\n";
        out += "  \"pixels_per_unit\": " + num(std::fmax(1.0f, sheet.pixelsPerUnit)) + ",\n";
        out += "  \"pixel_ratio\": " + pair(sheet.pixelRatio.x, sheet.pixelRatio.y) + ",\n";

        auto list = [&](const char* name, size_t count, const std::function<std::string(size_t)>& item, bool last){
            out += std::string("  \"") + name + "\": [";
            if (count == 0){
                out += "]";
            }else{
                out += "\n";
                for (size_t i = 0; i < count; i++){
                    out += "    " + item(i) + (i + 1 < count ? ",\n" : "\n");
                }
                out += "  ]";
            }
            out += last ? "\n" : ",\n";
        };

        list("pages", sheet.pages.size(), [&](size_t i){
            const SpritePage& p = sheet.pages[i];
            return "{\"texture\": " + quoted(p.texture) + ", \"size\": [" + std::to_string(p.width) + ", " + std::to_string(p.height) + "]}";
        }, false);
        list("frames", sheet.frames.size(), [&](size_t i){
            const SpriteFrame& f = sheet.frames[i];
            std::string s = "{\"name\": " + quoted(f.name);
            if (!f.key.empty()) s += ", \"key\": " + quoted(f.key);
            s += ", \"page\": " + std::to_string(f.page);
            s += ", \"rect\": " + rect(f.rect);
            s += ", \"source_size\": " + pair(f.sourceSize.x, f.sourceSize.y);
            s += ", \"offset\": " + pair(f.offset.x, f.offset.y);
            s += ", \"pivot\": " + pair(f.pivot.x, f.pivot.y);
            s += ", \"border\": [" + num(f.border.x) + ", " + num(f.border.y) + ", " + num(f.border.z) + ", " + num(f.border.w) + "]";
            s += ", \"duration_ms\": " + std::to_string(f.durationMs) + "}";
            return s;
        }, false);
        list("tags", sheet.tags.size(), [&](size_t i){
            const SpriteTag& t = sheet.tags[i];
            std::string s = "{\"name\": " + quoted(t.name) + ", \"from\": " + std::to_string(t.from) + ", \"to\": " + std::to_string(t.to) +
                            ", \"direction\": " + quoted(spriteTagDirectionName(t.direction)) + ", \"repeat\": " + std::to_string(t.repeat) +
                            ", \"color\": " + quoted(spriteColorText(t.color));
            const std::string ud = userData(t.userData);
            if (!ud.empty()) s += ", \"user_data\": " + ud;
            return s + "}";
        }, false);
        list("slices", sheet.slices.size(), [&](size_t i){
            const SpriteSlice& sl = sheet.slices[i];
            std::string s = "{\"name\": " + quoted(sl.name) + ", \"keys\": [";
            for (size_t k = 0; k < sl.keys.size(); k++){
                const SpriteSliceKey& key = sl.keys[k];
                s += (k ? ", " : "") + std::string("{\"frame\": ") + std::to_string(key.frame) + ", \"bounds\": " + rect(key.bounds);
                if (key.hasCenter) s += ", \"center\": " + rect(key.center);
                if (key.hasPivot) s += ", \"pivot\": " + pair(key.pivot.x, key.pivot.y);
                s += "}";
            }
            s += "]";
            const std::string ud = userData(sl.userData);
            if (!ud.empty()) s += ", \"user_data\": " + ud;
            return s + "}";
        }, false);
        // F49: only a sheet with skins has the list, so every text written before them stays canonical
        std::vector<size_t> skinned;
        for (size_t i = 0; i < sheet.frames.size(); i++){
            if (sheet.frames[i].skin) skinned.push_back(i);
        }
        if (!skinned.empty()){
            list("skins", skinned.size(), [&](size_t i){
                return "{\"frame\": " + std::to_string(skinned[i]) + ", " + writeSpriteSkin(*sheet.frames[skinned[i]].skin) + "}";
            }, false);
        }
        const std::string ud = userData(sheet.userData);
        out += "  \"user_data\": " + (ud.empty() ? std::string("{}") : ud) + "\n";
        out += "}\n";
        return out;
    }

}
