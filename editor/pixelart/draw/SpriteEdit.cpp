// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SpriteEdit.h"
#include "SpriteEditInternal.h"

#include "DrawPrimitives.h"

#include "pixelart/PixelColor.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>

namespace jasperengine::editor::pixelart::draw {

    namespace {

        using detail::Json;

        // The value kinds a field takes (validated before any op runs; ops check meaning)
        enum class Kind : uint8_t { Int, Number, Bool, Enum, Point, NumPoint, Points, Rect, Size, Color, Colors, String, Frames, Layer, Rows, Object, IntList };

        struct Field {
            const char* name;
            Kind kind;
            double lo = 0, hi = 0;
            std::vector<std::string> values = {};
        };

        struct OpSpec {
            const char* family;
            std::vector<Field> fields;
        };

        const std::vector<std::string> kBlendModes = {"normal", "multiply", "screen", "overlay", "darken", "lighten", "color_dodge",
                                                      "color_burn", "hard_light", "soft_light", "difference", "exclusion", "hue",
                                                      "saturation", "color", "luminosity", "addition", "subtract", "divide"};

        Field fFrame() { return {"frame", Kind::Frames}; }
        Field fLayer() { return {"layer", Kind::Layer}; }
        Field fIgnore() { return {"ignore_selection", Kind::Bool}; }
        Field fColor() { return {"color", Kind::Color}; }
        Field fRect() { return {"rect", Kind::Rect}; }
        Field fTolerance() { return {"tolerance", Kind::Int, 0, 255}; }

        const std::map<std::string, OpSpec>& specs() {
            static const std::map<std::string, OpSpec> table = [] {
                std::map<std::string, OpSpec> t;
                const Field thickness{"thickness", Kind::Int, 1, 16};
                t["pixel"] = {"paint", {{"points", Kind::Points}, fColor(), fFrame(), fLayer(), fIgnore()}};
                t["line"] = {"paint", {{"points", Kind::Points}, fColor(), thickness, {"pixel_perfect", Kind::Bool}, fFrame(), fLayer(), fIgnore()}};
                t["rect"] = {"paint", {fRect(), fColor(), {"fill", Kind::Bool}, thickness, {"radius", Kind::Int, 0, 64}, fFrame(), fLayer(), fIgnore()}};
                t["ellipse"] = {"paint", {fRect(), fColor(), {"fill", Kind::Bool}, thickness, fFrame(), fLayer(), fIgnore()}};
                t["polygon"] = {"paint", {{"points", Kind::Points}, fColor(), {"fill", Kind::Bool}, fFrame(), fLayer(), fIgnore()}};
                t["paste"] = {"paint", {{"rows", Kind::Rows}, {"at", Kind::Point}, {"encoding", Kind::Enum, 0, 0, {"auto", "chars", "hex2", "legend", "hex8"}},
                                        {"legend", Kind::Object}, {"blend", Kind::Enum, 0, 0, {"replace", "over"}}, fFrame(), fLayer(), fIgnore()}};
                t["fill"] = {"paint", {{"at", Kind::Point}, fColor(), fTolerance(), {"contiguous", Kind::Bool}, {"connectivity", Kind::Int, 4, 8},
                                       {"sample", Kind::Enum, 0, 0, {"layer", "composite"}}, fFrame(), fLayer(), fIgnore()}};
                t["replace"] = {"paint", {{"from", Kind::Color}, {"to", Kind::Color}, fTolerance(), fRect(), fFrame(), fLayer(), fIgnore()}};
                t["dither"] = {"paint", {{"colors", Kind::Colors}, {"pattern", Kind::Enum, 0, 0, {"bayer2", "bayer4", "bayer8", "bayer16", "checker"}},
                                         {"level", Kind::Number, 0, 1}, {"gradient", Kind::Object}, fRect(), fFrame(), fLayer(), fIgnore()}};
                t["clear"] = {"paint", {fRect(), fFrame(), fLayer(), fIgnore()}};
                const Field shape{"shape", Kind::Enum, 0, 0, {"diamond", "circle", "square"}};
                t["outline"] = {"paint", {fColor(), {"width", Kind::Int, 1, 10}, shape, fFrame(), fLayer(), fIgnore()}};
                t["inline"] = {"paint", {fColor(), {"width", Kind::Int, 1, 10}, shape, fFrame(), fLayer(), fIgnore()}};
                t["shade"] = {"paint", {{"mode", Kind::Enum, 0, 0, {"simple", "hue_shift", "ramp"}}, {"direction", Kind::Enum, 0, 0, {"lighten", "darken"}},
                                        {"amount", Kind::Number, 0, 100}, {"hue", Kind::Number, -180, 180}, {"saturation", Kind::Number, -100, 100},
                                        {"value", Kind::Number, -100, 100}, {"ramp", Kind::IntList}, {"ramp_name", Kind::String}, {"points", Kind::Points},
                                        fRect(), {"match", Kind::Color}, {"palette_mode", Kind::Enum, 0, 0, {"snap", "add"}}, fFrame(), fLayer(), fIgnore()}};
                t["shift"] = {"transform", {{"by", Kind::Point}, {"wrap", Kind::Bool}, fRect(), fFrame(), fLayer(), fIgnore()}};
                const Field axis{"axis", Kind::Enum, 0, 0, {"x", "y"}};
                t["flip"] = {"transform", {axis, fRect(), fFrame(), fLayer(), fIgnore()}};
                t["rotate"] = {"transform", {{"angle", Kind::Number, -360, 360}, {"method", Kind::Enum, 0, 0, {"auto", "cleanedge", "rotxel", "omniscale", "nearest"}},
                                             {"pivot", Kind::NumPoint}, fRect(), fFrame(), fLayer(), fIgnore()}};
                t["scale"] = {"transform", {{"factor", Kind::Int, 2, 4}, {"method", Kind::Enum, 0, 0, {"nearest", "scale2x", "scale3x"}}, fTolerance(), fRect(),
                                            fFrame(), fLayer(), fIgnore()}};
                t["mirror"] = {"transform", {axis, {"from_side", Kind::Enum, 0, 0, {"left", "right", "top", "bottom"}}, {"at", Kind::Number, -1, 8192}, fRect(),
                                             fFrame(), fLayer(), fIgnore()}};
                t["copy"] = {"transform", {{"source", Kind::Object}, {"at", Kind::Point}, {"to_frame", Kind::Frames}, {"to_layer", Kind::Layer},
                                           {"blend", Kind::Enum, 0, 0, {"replace", "over"}}, {"cut", Kind::Bool}, fFrame(), fLayer(), fIgnore()}};
                t["select"] = {"transform", {{"shape", Kind::Enum, 0, 0, {"rect", "ellipse", "polygon", "color", "all", "none", "invert"}},
                                             {"select_mode", Kind::Enum, 0, 0, {"replace", "add", "subtract", "intersect"}}, fRect(), {"points", Kind::Points},
                                             {"at", Kind::Point}, fTolerance(), {"contiguous", Kind::Bool}, fFrame(), fLayer()}};
                t["layer"] = {"structure", {{"action", Kind::Enum, 0, 0, {"add", "remove", "rename", "move", "set"}}, {"name", Kind::Layer},
                                            {"new_name", Kind::String}, {"index", Kind::Int, 0, 255}, {"at", Kind::Int, 0, 255}, {"visible", Kind::Bool},
                                            {"opacity", Kind::Int, 0, 255}, {"blend_mode", Kind::Enum, 0, 0, kBlendModes}, {"group", Kind::String}}};
                t["frame"] = {"structure", {{"action", Kind::Enum, 0, 0, {"add", "remove", "move", "duplicate", "set"}}, {"at", Kind::Int, 1, 257},
                                            {"count", Kind::Int, 1, 256}, {"copy", Kind::Bool}, {"link", Kind::Bool}, {"duration_ms", Kind::Int, 1, 65535},
                                            {"from", Kind::Int, 1, 256}, {"to", Kind::Int, 1, 256}}};
                t["tag"] = {"structure", {{"action", Kind::Enum, 0, 0, {"set", "remove"}}, {"name", Kind::String}, {"new_name", Kind::String},
                                          {"from", Kind::Int, 1, 256}, {"to", Kind::Int, 1, 256},
                                          {"direction", Kind::Enum, 0, 0, {"forward", "reverse", "pingpong", "pingpong_reverse"}},
                                          {"repeat", Kind::Int, 0, 65535}, {"color", Kind::Color}}};
                t["slice"] = {"structure", {{"action", Kind::Enum, 0, 0, {"set", "remove"}}, {"name", Kind::String}, {"new_name", Kind::String}, fRect(),
                                            {"pivot", Kind::Point}, {"center", Kind::Rect}}};
                t["cel"] = {"structure", {{"action", Kind::Enum, 0, 0, {"link", "unlink", "clear", "set"}}, {"from", Kind::Int, 1, 256}, fFrame(), fLayer(),
                                          {"opacity", Kind::Int, 0, 255}, {"z_index", Kind::Int, -32768, 32767}}};
                t["palette"] = {"structure", {{"action", Kind::Enum, 0, 0, {"set", "add", "replace", "load"}}, {"colors", Kind::Colors}, {"index", Kind::Int, 0, 255},
                                              fColor(), {"asset", Kind::String}, {"remap", Kind::Enum, 0, 0, {"keep_indices", "nearest"}}}};
                t["canvas"] = {"structure", {{"action", Kind::Enum, 0, 0, {"resize", "crop", "convert"}}, {"size", Kind::Size},
                                             {"anchor", Kind::Enum, 0, 0, {"center", "top_left", "top", "top_right", "left", "right", "bottom_left", "bottom", "bottom_right"}},
                                             fRect(), {"mode", Kind::Enum, 0, 0, {"indexed", "rgba"}}, {"snap", Kind::Bool}}};
                return t;
            }();
            return table;
        }

        const std::vector<std::string>& orderedNames() {
            static const std::vector<std::string> names = {"pixel", "line", "rect", "ellipse", "polygon", "paste", "fill", "replace", "dither", "clear",
                                                           "outline", "inline", "shade", "shift", "flip", "rotate", "scale", "mirror", "copy", "select",
                                                           "layer", "frame", "tag", "slice", "cel", "palette", "canvas"};
            return names;
        }

        std::string joined(const std::vector<std::string>& items) {
            std::string out;
            for (const std::string& s : items) out += (out.empty() ? "" : ", ") + s;
            return out;
        }

        std::string rangeText(double lo, double hi) {
            auto num = [](double v) {
                char buffer[32];
                std::snprintf(buffer, sizeof(buffer), "%g", v);
                return std::string(buffer);
            };
            return num(lo) + "-" + num(hi);
        }

        bool isIntPair(const Json& v) {
            return v.is_array() && v.size() == 2 && v[0].is_number_integer() && v[1].is_number_integer();
        }

        bool isColorValue(const Json& v) {
            return v.is_number_integer() || v.is_string();
        }

        // the type check of one field; "" when it fits, else what it should be
        std::string kindProblem(const Field& f, const Json& v) {
            switch (f.kind) {
                case Kind::Int:
                    if (!v.is_number_integer() || v.get<double>() < f.lo || v.get<double>() > f.hi) return "an integer " + rangeText(f.lo, f.hi);
                    if (std::string(f.name) == "connectivity" && v.get<int>() != 4 && v.get<int>() != 8) return "4 or 8";
                    return "";
                case Kind::Number:
                    if (!v.is_number() || v.get<double>() < f.lo || v.get<double>() > f.hi) return "a number " + rangeText(f.lo, f.hi);
                    return "";
                case Kind::Bool: return v.is_boolean() ? "" : "true or false";
                case Kind::Enum:
                    if (v.is_string() && std::find(f.values.begin(), f.values.end(), v.get<std::string>()) != f.values.end()) return "";
                    return "one of " + joined(f.values);
                case Kind::Point: return isIntPair(v) ? "" : "[x, y] integers";
                case Kind::NumPoint: return v.is_array() && v.size() == 2 && v[0].is_number() && v[1].is_number() ? "" : "[x, y] numbers (halves allowed)";
                case Kind::Points: {
                    if (!v.is_array() || v.empty()) return "a list of [x, y] integer points";
                    if (v.size() > static_cast<size_t>(kMaxOpPoints)) return "at most 16384 points";
                    for (const Json& p : v) {
                        if (!isIntPair(p)) return "a list of [x, y] integer points";
                    }
                    return "";
                }
                case Kind::Rect:
                    if (v.is_array() && v.size() == 4 && std::all_of(v.begin(), v.end(), [](const Json& e) { return e.is_number_integer(); })) return "";
                    return "[x, y, w, h] integers";
                case Kind::Size:
                    if (isIntPair(v) && v[0].get<int>() >= 1 && v[1].get<int>() >= 1 && v[0].get<int>() <= kMaxEditSide && v[1].get<int>() <= kMaxEditSide) return "";
                    return "[w, h], each 1-4096";
                case Kind::Color: return isColorValue(v) ? "" : "a palette index, \"#rrggbb\", \"#rrggbbaa\" or \"transparent\"";
                case Kind::Colors:
                    if (v.is_array() && !v.empty() && std::all_of(v.begin(), v.end(), isColorValue)) return "";
                    return "a list of colours (palette indices, \"#rrggbb\", \"#rrggbbaa\", \"transparent\")";
                case Kind::String: return v.is_string() ? "" : "a string";
                case Kind::Frames:
                    if (v.is_number_integer() || v.is_string()) return "";
                    if (v.is_array() && !v.empty() && std::all_of(v.begin(), v.end(), [](const Json& e) { return e.is_number_integer(); })) return "";
                    return "a frame from 1, a list of frames, a tag name or \"all\"";
                case Kind::Layer: return v.is_number_integer() || v.is_string() ? "" : "a layer name, group path or index from 0";
                case Kind::Rows:
                    if (v.is_array() && std::all_of(v.begin(), v.end(), [](const Json& e) { return e.is_string(); })) return "";
                    return "a list of row strings, top first";
                case Kind::Object: return v.is_object() ? "" : "an object";
                case Kind::IntList:
                    if (v.is_array() && v.size() >= 2 && std::all_of(v.begin(), v.end(), [](const Json& e) { return e.is_number_integer(); })) return "";
                    return "a list of at least 2 palette indices";
            }
            return "";
        }

        // the op (other than `self`) that takes a field, for a misplaced key's message
        std::string ownerOf(const std::string& field, const std::string& self) {
            for (const std::string& name : orderedNames()) {
                if (name == self) continue;
                for (const Field& f : specs().at(name).fields) {
                    if (field == f.name) return name;
                }
            }
            return "";
        }

    }

    const std::vector<std::string>& opNames() { return orderedNames(); }

    const char* opFamily(const std::string& op) {
        auto it = specs().find(op);
        return it == specs().end() ? "" : it->second.family;
    }

    const std::vector<std::string>& opFields(const std::string& op) {
        static std::map<std::string, std::vector<std::string>> cache = [] {
            std::map<std::string, std::vector<std::string>> c;
            for (const auto& [name, spec] : specs()) {
                for (const Field& f : spec.fields) c[name].push_back(f.name);
            }
            return c;
        }();
        static const std::vector<std::string> none;
        auto it = cache.find(op);
        return it == cache.end() ? none : it->second;
    }

    bool validateOps(const nlohmann::json& ops, PixelProblem& problem, int& opIndex) {
        opIndex = -1;
        if (!ops.is_array() || ops.empty()) {
            problem = {"bad_op_field", "ops", "ops is a list of 1-256 op objects", "send ops: [{\"op\": \"pixel\", \"points\": [[0, 0]], \"color\": 1}]"};
            return false;
        }
        if (ops.size() > static_cast<size_t>(kMaxOps)) {
            problem = {"too_many_ops", "ops", "the batch has " + std::to_string(ops.size()) + " ops, above 256", "send at most 256 ops per call; split the batch"};
            return false;
        }
        for (size_t i = 0; i < ops.size(); i++) {
            opIndex = static_cast<int>(i);
            const Json& op = ops[i];
            const std::string at = "ops[" + std::to_string(i) + "]";
            if (!op.is_object() || !op.contains("op") || !op["op"].is_string() || !specs().count(op["op"].get<std::string>())) {
                problem = {"bad_op_field", at + ".op", at + " has no known op", "op is one of " + joined(orderedNames())};
                return false;
            }
            const std::string name = op["op"].get<std::string>();
            const OpSpec& spec = specs().at(name);
            const std::string fields = joined(opFields(name));
            for (auto it = op.begin(); it != op.end(); ++it) {
                if (it.key() == "op") continue;
                const Field* field = nullptr;
                for (const Field& f : spec.fields) {
                    if (it.key() == f.name) field = &f;
                }
                if (!field) {
                    const std::string owner = ownerOf(it.key(), name);
                    problem = {"bad_op_field", at + "." + it.key(),
                               name + " has no field \"" + it.key() + "\"" + (owner.empty() ? "" : " (" + owner + " takes it)"),
                               name + " takes: " + fields};
                    return false;
                }
                const std::string expected = kindProblem(*field, it.value());
                if (!expected.empty()) {
                    problem = {"bad_op_field", at + "." + it.key(), name + "." + it.key() + " must be " + expected, name + " takes: " + fields};
                    return false;
                }
            }
        }
        opIndex = -1;
        return true;
    }

    std::string summarizeOps(const std::vector<std::string>& opKinds) {
        std::vector<std::pair<std::string, int>> counts;
        for (const std::string& k : opKinds) {
            auto it = std::find_if(counts.begin(), counts.end(), [&](const auto& c) { return c.first == k; });
            if (it == counts.end()) counts.push_back({k, 1});
            else it->second++;
        }
        std::string out;
        for (const auto& [k, n] : counts) {
            out += (out.empty() ? "" : ", ") + k + (n > 1 ? " \xC3\x97" + std::to_string(n) : "");
        }
        return out;
    }

    // ------------------------------------------------------------------ the working state

    namespace detail {

        void fail(std::string kind, std::string field, std::string message, std::string fix) {
            throw OpError{{std::move(kind), std::move(field), std::move(message), std::move(fix)}};
        }

        int intField(const Json& op, const char* name, int def, int lo, int hi) {
            if (!op.contains(name)) return def;
            const Json& v = op[name];
            if (!v.is_number_integer() || v.get<long long>() < lo || v.get<long long>() > hi) {
                fail("bad_op_field", name, std::string(name) + " must be an integer " + std::to_string(lo) + "-" + std::to_string(hi),
                     std::string(name) + " is " + std::to_string(lo) + "-" + std::to_string(hi));
            }
            return v.get<int>();
        }

        double numberField(const Json& op, const char* name, double def, double lo, double hi) {
            if (!op.contains(name)) return def;
            const Json& v = op[name];
            if (!v.is_number() || v.get<double>() < lo || v.get<double>() > hi) {
                fail("bad_op_field", name, std::string(name) + " must be a number " + rangeText(lo, hi), std::string(name) + " is " + rangeText(lo, hi));
            }
            return v.get<double>();
        }

        bool boolField(const Json& op, const char* name, bool def) {
            if (!op.contains(name)) return def;
            if (!op[name].is_boolean()) fail("bad_op_field", name, std::string(name) + " must be true or false", "");
            return op[name].get<bool>();
        }

        std::string enumField(const Json& op, const char* name, const std::string& def, const std::vector<std::string>& values) {
            if (!op.contains(name)) return def;
            const Json& v = op[name];
            if (!v.is_string() || std::find(values.begin(), values.end(), v.get<std::string>()) == values.end()) {
                fail("bad_op_field", name, std::string(name) + " must be one of " + joined(values), std::string(name) + " is one of " + joined(values));
            }
            return v.get<std::string>();
        }

        IntPoint pointValue(const Json& value, const std::string& field) {
            if (!isIntPair(value)) fail("bad_op_field", field, field + " must be [x, y] integers", "");
            return {value[0].get<int>(), value[1].get<int>()};
        }

        std::vector<IntPoint> pointsField(const Json& op, const char* name, size_t minimum) {
            if (!op.contains(name) || !op[name].is_array() || op[name].size() < minimum) {
                fail("bad_op_field", name, std::string(op.value("op", "")) + " needs " + name + ": at least " + std::to_string(minimum) + " [x, y] points",
                     std::string("send ") + name + ": [[x, y], ...]");
            }
            if (op[name].size() > static_cast<size_t>(kMaxOpPoints)) {
                fail("bad_op_field", name, std::string(name) + " holds " + std::to_string(op[name].size()) + " points, above 16384",
                     "send at most 16384 points per op");
            }
            std::vector<IntPoint> out;
            for (size_t i = 0; i < op[name].size(); i++) out.push_back(pointValue(op[name][i], std::string(name) + "[" + std::to_string(i) + "]"));
            return out;
        }

        IntRect rectValue(const Json& value, const std::string& field) {
            if (!value.is_array() || value.size() != 4 || !std::all_of(value.begin(), value.end(), [](const Json& e) { return e.is_number_integer(); })) {
                fail("bad_op_field", field, field + " must be [x, y, w, h] integers", "");
            }
            const IntRect r{value[0].get<int>(), value[1].get<int>(), value[2].get<int>(), value[3].get<int>()};
            if (r.w <= 0 || r.h <= 0) fail("bad_op_field", field, field + " has w or h below 1", field + " is [x, y, w, h] with w and h at least 1");
            return r;
        }

        std::optional<IntRect> rectField(const Json& op, const char* name) {
            if (!op.contains(name)) return std::nullopt;
            return rectValue(op[name], name);
        }

        EditState::EditState(SpriteDoc& s, const EditRequest& req, const EditEnv& e, EditResult& r)
            : sprite(s), doc(s.doc), env(e), result(r), request(req) {}

        std::string EditState::frameRange() const {
            std::string tags;
            for (size_t i = 0; i < doc.tags.size() && i < 6; i++) tags += (tags.empty() ? "" : ", ") + doc.tags[i].name;
            std::string out = "frame is 1-" + std::to_string(doc.frameCount());
            if (!tags.empty()) out += ", a tag (" + tags + (doc.tags.size() > 6 ? ", ..." : "") + ")";
            return out + " or \"all\" (frames from 1, as Aseprite shows them)";
        }

        std::string EditState::layerList() const {
            std::string out;
            for (int i = static_cast<int>(doc.layers.size()) - 1; i >= 0 && out.size() < 300; i--) {
                out += (out.empty() ? "" : ", ") + doc.layerPath(i) + " (" + std::to_string(i) + (doc.layers[static_cast<size_t>(i)].isGroup() ? ", group" : "") + ")";
            }
            return "layers, top first: " + out + " (index 0 is the bottom)";
        }

        int EditState::parseFrame(const Json& value, const std::string& field) {
            if (!value.is_number_integer() || value.get<int>() < 1 || value.get<int>() > doc.frameCount()) {
                fail("no_frame", field, field + " " + value.dump() + " is not a frame of this sprite", frameRange());
            }
            return value.get<int>() - 1;
        }

        std::vector<int> EditState::parseFrames(const Json& value, const std::string& field) {
            std::vector<int> out;
            if (value.is_number_integer()) {
                out.push_back(parseFrame(value, field));
            } else if (value.is_array()) {
                if (value.empty()) fail("no_frame", field, field + " is an empty list", frameRange());
                for (const Json& v : value) {
                    const int f = parseFrame(v, field);
                    if (std::find(out.begin(), out.end(), f) == out.end()) out.push_back(f);
                }
            } else if (value.is_string()) {
                const std::string name = value.get<std::string>();
                if (name == "all") {
                    for (int f = 0; f < doc.frameCount(); f++) out.push_back(f);
                } else {
                    const ase::Tag* tag = nullptr;
                    for (const ase::Tag& t : doc.tags) {
                        if (t.name == name) tag = &t;
                    }
                    if (!tag) fail("no_tag", field, "the sprite has no tag \"" + name + "\"", frameRange());
                    for (int f = tag->from; f <= tag->to && f < doc.frameCount(); f++) out.push_back(f);
                }
            } else {
                fail("no_frame", field, field + " must be a frame, a list, a tag name or \"all\"", frameRange());
            }
            return out;
        }

        std::vector<int> EditState::frames(const Json& op, const char* field) {
            if (op.contains(field)) return parseFrames(op[field], field);
            if (!request.frame.is_null()) return parseFrames(request.frame, "frame");
            return {0};
        }

        int EditState::parseLayer(const Json& value, const std::string& field, bool pixels) {
            int index = -1;
            if (value.is_number_integer()) {
                index = value.get<int>();
                if (index < 0 || index >= static_cast<int>(doc.layers.size())) index = -1;
            } else if (value.is_string()) {
                index = doc.findLayer(value.get<std::string>());
            }
            if (index < 0) fail("no_layer", field, "the sprite has no layer " + value.dump(), layerList());
            if (pixels) {
                const ase::Layer& l = doc.layers[static_cast<size_t>(index)];
                if (l.type == ase::LayerType::Tilemap) {
                    fail("unsupported_layer", field, doc.layerPath(index) + " is a tilemap layer, which edit_sprite does not paint",
                         "paint a pixel layer; " + layerList());
                }
                if (l.isGroup()) fail("no_layer", field, doc.layerPath(index) + " is a group, not a pixel layer", "name one of its pixel layers; " + layerList());
            }
            return index;
        }

        int EditState::pixelLayer(const Json& op, const char* field) {
            if (op.contains(field)) return parseLayer(op[field], field, true);
            if (!request.layer.is_null()) return parseLayer(request.layer, "layer", true);
            for (int i = static_cast<int>(doc.layers.size()) - 1; i >= 0; i--) {
                const ase::Layer& l = doc.layers[static_cast<size_t>(i)];
                if (l.type == ase::LayerType::Image && doc.visibleHierarchy(i)) return i;
            }
            for (int i = static_cast<int>(doc.layers.size()) - 1; i >= 0; i--) {
                if (doc.layers[static_cast<size_t>(i)].type == ase::LayerType::Image) return i;
            }
            fail("no_layer", field, "the sprite has no pixel layer", "add one first: {\"op\": \"layer\", \"action\": \"add\", \"name\": \"Layer 1\"}");
        }

        std::vector<uint8_t> EditState::paletteRgba() const { return doc.paletteRgba(0); }

        Ink EditState::inkOfIndex(int index) const {
            Ink ink;
            ink.index = static_cast<uint8_t>(index);
            if (index >= 0 && index < static_cast<int>(doc.palette.size())) ink.rgba = doc.palette[static_cast<size_t>(index)].color;
            return ink;
        }

        Ink EditState::clearInk(int layer) const {
            Ink ink;
            if (indexed()) {
                ink.index = doc.transparentIndex;
                const bool background = layer >= 0 && layer < static_cast<int>(doc.layers.size()) && doc.layers[static_cast<size_t>(layer)].background();
                if (background) ink.rgba = inkOfIndex(doc.transparentIndex).rgba;
            }
            return ink;
        }

        Ink EditState::inkOfRgba(const PixelRgba& in, const std::string& field) {
            PixelRgba rgba = in;
            if (grayscale() && rgba[3] > 0) {
                // Aseprite's rgba_luma, as its grayscale conversion
                const uint8_t v = static_cast<uint8_t>((rgba[0] * 2126 + rgba[1] * 7152 + rgba[2] * 722) / 10000);
                rgba = {v, v, v, rgba[3]};
            }
            Ink ink;
            if (!indexed()) {
                ink.rgba = rgba[3] == 0 ? PixelRgba{0, 0, 0, 0} : rgba;
                return ink;
            }
            if (rgba[3] == 0) return inkOfIndex(doc.transparentIndex);
            for (size_t i = 0; i < doc.palette.size(); i++) {
                if (doc.palette[i].color == rgba && i != doc.transparentIndex) return inkOfIndex(static_cast<int>(i));
            }
            std::vector<jasperengine::pixelart::Rgba8> colors;
            for (const ase::PaletteEntry& e : doc.palette) colors.push_back({e.color[0], e.color[1], e.color[2], e.color[3]});
            if (!colors.empty()) colors[std::min<size_t>(doc.transparentIndex, colors.size() - 1)].a = 0;
            const std::string hex = hexColor(rgba);
            std::string nearest;
            if (!colors.empty()) {
                jasperengine::pixelart::PaletteMatcher matcher(colors, jasperengine::pixelart::ColorMetric::OkLab);
                const int n = matcher.nearestUncached({rgba[0], rgba[1], rgba[2], rgba[3]}).first;
                if (n >= 0) nearest = "nearest is index " + std::to_string(n) + " (" + hexColor(doc.palette[static_cast<size_t>(n)].color) + "); ";
            }
            fail("color_not_in_palette", field, hex + " is not in the sprite's palette (indices 0-" + std::to_string(static_cast<int>(doc.palette.size()) - 1) + ")",
                 nearest + "or add it first: {\"op\": \"palette\", \"action\": \"add\", \"colors\": [\"" + hex + "\"]}");
        }

        Ink EditState::color(const Json& value, const std::string& field) {
            if (value.is_number_integer()) {
                const int i = value.get<int>();
                const int size = static_cast<int>(doc.palette.size());
                if (i < 0 || i >= size) {
                    fail("bad_color", field, field + " " + std::to_string(i) + " is not a palette index",
                         size == 0 ? std::string("the palette is empty; add colours with {\"op\": \"palette\", \"action\": \"add\"}")
                                   : "palette has indices 0-" + std::to_string(size - 1) + "; add colours with {\"op\": \"palette\", \"action\": \"add\"}");
                }
                if (indexed()) return inkOfIndex(i);
                return inkOfRgba(doc.palette[static_cast<size_t>(i)].color, field);
            }
            if (value.is_string()) {
                const std::string text = value.get<std::string>();
                if (text == "transparent") return indexed() ? inkOfIndex(doc.transparentIndex) : Ink{};
                jasperengine::pixelart::Rgba8 c;
                bool hasAlpha = false;
                if (!text.empty() && text[0] == '#' && (text.size() == 7 || text.size() == 9) && jasperengine::pixelart::parseHex(text, c, &hasAlpha)) {
                    return inkOfRgba({c.r, c.g, c.b, c.a}, field);
                }
            }
            fail("bad_color", field, field + " " + value.dump() + " is not a colour",
                 "a colour is a palette index, \"#rrggbb\", \"#rrggbbaa\" or \"transparent\"");
        }

        Ink EditState::inkAt(const PixelImage& image, int x, int y) const {
            Ink ink;
            ink.rgba = image.at(x, y);
            if (image.hasIndices()) ink.index = image.indexAt(x, y);
            return ink;
        }

        bool EditState::clearAt(const PixelImage& image, int x, int y) const {
            if (image.hasIndices()) return image.at(x, y)[3] == 0;
            return image.at(x, y)[3] == 0;
        }

        bool EditState::sameInk(const Ink& a, const Ink& b) const {
            if (indexed()) return a.index == b.index;
            return a.rgba == b.rgba || (a.rgba[3] == 0 && b.rgba[3] == 0);
        }

        bool EditState::similar(const Ink& a, const Ink& b, int tolerance) const {
            if (indexed() && tolerance == 0) return a.index == b.index;
            if (tolerance == 0) return sameInk(a, b);
            int d = 0;
            for (int k = 0; k < 4; k++) d = std::max(d, std::abs(int(a.rgba[k]) - int(b.rgba[k])));
            return d <= tolerance;
        }

        Canvas& EditState::canvas(int layer, int frame) {
            const ase::Cel* cel = doc.cel(layer, frame);
            const int src = cel ? cel->frame : frame;
            auto key = std::make_pair(layer, src);
            auto it = canvases.find(key);
            if (it == canvases.end()) {
                Canvas c;
                c.layer = layer;
                c.srcFrame = src;
                c.original = celCanvas(doc, layer, frame);
                c.current = c.original;
                it = canvases.emplace(key, std::move(c)).first;
            }
            it->second.frames.insert(frame);
            return it->second;
        }

        bool EditState::selected(int x, int y) const {
            if (!hasSelection) return true;
            if (x < 0 || y < 0 || x >= doc.width || y >= doc.height) return false;
            return selection[static_cast<size_t>(y) * doc.width + x] != 0;
        }

        IntRect EditState::selectionBounds() const {
            if (!hasSelection) return {0, 0, doc.width, doc.height};
            int x0 = doc.width, y0 = doc.height, x1 = -1, y1 = -1;
            for (int y = 0; y < doc.height; y++) {
                for (int x = 0; x < doc.width; x++) {
                    if (!selection[static_cast<size_t>(y) * doc.width + x]) continue;
                    x0 = std::min(x0, x);
                    y0 = std::min(y0, y);
                    x1 = std::max(x1, x);
                    y1 = std::max(y1, y);
                }
            }
            if (x1 < 0) return {0, 0, 0, 0};
            return {x0, y0, x1 - x0 + 1, y1 - y0 + 1};
        }

        bool EditState::writable(int x, int y) const {
            if (x < 0 || y < 0 || x >= doc.width || y >= doc.height) return false;
            return ignoreSelection || selected(x, y);
        }

        void EditState::putRaw(Canvas& c, int x, int y, const Ink& ink) {
            if (x < 0 || y < 0 || x >= doc.width || y >= doc.height) return;
            const Ink old = inkAt(c.current, x, y);
            if (indexed()) {
                if (old.index == ink.index) return;
                c.current.setIndex(x, y, ink.index, ink.rgba);
            } else {
                if (old.rgba == ink.rgba) return;
                c.current.set(x, y, ink.rgba);
            }
            opChanges++;
        }

        void EditState::put(Canvas& c, int x, int y, const Ink& ink) {
            if (!writable(x, y)) return;
            putRaw(c, x, y, ink);
        }

        std::vector<IntPoint> EditState::mirrorsOf(IntPoint p) const {
            auto mx = [&](IntPoint q) { return IntPoint{axisX2 - q.x, q.y}; };
            auto my = [&](IntPoint q) { return IntPoint{q.x, axisY2 - q.y}; };
            // Tools.gd calculate_mirror_diagonal: reflected about the canvas centre through pixel centres
            auto md = [&](IntPoint q, bool flipped) {
                const double cx = doc.width / 2.0, cy = doc.height / 2.0;
                const double lx = q.x + 0.5 - cx, ly = q.y + 0.5 - cy;
                const double rx = flipped ? ly : -ly, ry = flipped ? lx : -lx;
                auto roundAway = [](double v) { return static_cast<int>(v < 0 ? -std::floor(-v + 0.5) : std::floor(v + 0.5)); };
                return IntPoint{roundAway(rx + cx - 0.5), roundAway(ry + cy - 0.5)};
            };
            std::vector<IntPoint> out;
            if (mirrorX) {
                const IntPoint a = mx(p);
                out.push_back(a);
                if (mirrorY) out.push_back(my(a));
                else {
                    if (diagXY) out.push_back(md(a, false));
                    if (diagXMinusY) out.push_back(md(a, true));
                }
            }
            if (mirrorY) {
                const IntPoint b = my(p);
                out.push_back(b);
                if (diagXY) out.push_back(md(b, false));
                if (diagXMinusY) out.push_back(md(b, true));
            }
            if (diagXY) {
                const IntPoint d = md(p, false);
                out.push_back(d);
                if (!mirrorX && !mirrorY && diagXMinusY) out.push_back(md(d, true));
            }
            if (diagXMinusY) out.push_back(md(p, true));
            return out;
        }

        std::vector<IntPoint> EditState::withMirrors(const std::vector<IntPoint>& pixels) const {
            std::vector<IntPoint> out = pixels;
            if (!symmetric()) return out;
            for (const IntPoint& p : pixels) {
                for (const IntPoint& m : mirrorsOf(p)) out.push_back(m);
            }
            dedupe(out);
            return out;
        }

        void EditState::writeBack(Canvas& c) {
            // the bounds of what differs from the cel as it was
            const PixelImage& a = c.original;
            const PixelImage& b = c.current;
            int x0 = doc.width, y0 = doc.height, x1 = -1, y1 = -1;
            const bool idx = a.hasIndices() && b.hasIndices();
            for (int y = 0; y < doc.height; y++) {
                const uint8_t* ra = a.row(y);
                const uint8_t* rb = b.row(y);
                for (int x = 0; x < doc.width; x++) {
                    const bool differs = idx ? a.indexAt(x, y) != b.indexAt(x, y) : std::memcmp(ra + x * 4, rb + x * 4, 4) != 0;
                    if (!differs) continue;
                    x0 = std::min(x0, x);
                    y0 = std::min(y0, y);
                    x1 = std::max(x1, x);
                    y1 = std::max(y1, y);
                }
            }
            // nothing differs and nothing was written: the cel keeps its image and its provenance
            if (x1 < 0 && !c.written) return;
            ase::Cel* rec = doc.celRecord(c.layer, c.srcFrame);
            if (rec && rec->linkedFrame >= 0) rec = nullptr;   // never: srcFrame holds the image
            IntRect bounds = x1 < 0 ? IntRect{} : IntRect{x0, y0, x1 - x0 + 1, y1 - y0 + 1};
            if (rec && rec->image) bounds = bounds.unite({rec->x, rec->y, rec->image->width(), rec->image->height()});
            if (bounds.empty()) return;
            PixelImage image;
            if (indexed()) {
                const bool background = doc.layers[static_cast<size_t>(c.layer)].background();
                image = PixelImage::fromIndices(bounds.w, bounds.h, std::vector<uint8_t>(static_cast<size_t>(bounds.w) * bounds.h, doc.transparentIndex),
                                                paletteRgba(), background ? -1 : doc.transparentIndex);
            } else {
                image = PixelImage(bounds.w, bounds.h);
            }
            if (rec && rec->image) image.blit(*rec->image, rec->x - bounds.x, rec->y - bounds.y);
            // the canvas inside the canvas rect (cels may reach past it: those parts stay)
            const IntRect inside = bounds.intersect({0, 0, doc.width, doc.height});
            if (!inside.empty()) image.blit(b.crop(inside), inside.x - bounds.x, inside.y - bounds.y);
            if (!rec) {
                ase::Cel cel;
                cel.layer = c.layer;
                cel.frame = c.srcFrame;
                doc.cels.push_back(std::move(cel));
                doc.sortCels();
                rec = doc.celRecord(c.layer, c.srcFrame);
            }
            rec->x = bounds.x;
            rec->y = bounds.y;
            rec->image = std::make_shared<const PixelImage>(std::move(image));
            rec->tilemap.reset();
            c.written = true;
        }

        void EditState::record(Canvas& c) {
            const PixelImage& a = c.original;
            const PixelImage& b = c.current;
            const bool idx = a.hasIndices() && b.hasIndices();
            int x0 = doc.width, y0 = doc.height, x1 = -1, y1 = -1, count = 0;
            for (int y = 0; y < doc.height; y++) {
                for (int x = 0; x < doc.width; x++) {
                    const bool differs = idx ? a.indexAt(x, y) != b.indexAt(x, y) : std::memcmp(a.row(y) + x * 4, b.row(y) + x * 4, 4) != 0;
                    if (!differs) continue;
                    count++;
                    x0 = std::min(x0, x);
                    y0 = std::min(y0, y);
                    x1 = std::max(x1, x);
                    y1 = std::max(y1, y);
                }
            }
            if (count == 0) return;
            for (int f : c.frames) recorded.push_back({f + 1, c.layer, {x0, y0, x1 - x0 + 1, y1 - y0 + 1}, count});
            result.pixels += count;
        }

        void EditState::flush(bool drop) {
            for (auto& [key, c] : canvases) writeBack(c);
            if (!drop) return;
            for (auto& [key, c] : canvases) record(c);
            canvases.clear();
        }

        PixelImage EditState::composite(int frame, bool keepIndices) {
            flush(false);
            ase::RenderOptions options;
            options.keepIndices = keepIndices;
            return ase::renderFrame(doc, frame, options);
        }

        void EditState::remapRecorded(const std::function<bool(DirtyRect&)>& remap) {
            std::vector<DirtyRect> kept;
            for (DirtyRect& d : recorded) {
                if (remap(d)) kept.push_back(d);
            }
            recorded.swap(kept);
        }

        void EditState::warn(std::string kind, std::string message) {
            result.warnings.push_back({std::move(kind), std::move(message), opIndex});
        }

        void EditState::finish() {
            flush(true);
            // one rect per (frame, layer)
            std::map<std::pair<int, int>, DirtyRect> merged;
            for (const DirtyRect& d : recorded) {
                auto key = std::make_pair(d.frame, d.layer);
                auto it = merged.find(key);
                if (it == merged.end()) merged[key] = d;
                else {
                    it->second.rect = it->second.rect.unite(d.rect);
                    it->second.pixels += d.pixels;
                }
            }
            result.changed.clear();
            for (const auto& [key, d] : merged) result.changed.push_back(d);
        }

    }

    namespace {

        struct AddressingContext {
            EditRequest request;
            EditEnv env;
            EditResult result;
            detail::EditState state;
            explicit AddressingContext(SpriteDoc& sprite) : state(sprite, request, env, result) {}
        };

    }

    bool resolveFrames(SpriteDoc& sprite, const nlohmann::json& value, const std::string& field, std::vector<int>& out, PixelProblem& problem) {
        AddressingContext context(sprite);
        try {
            out = context.state.parseFrames(value, field);
            return true;
        } catch (const detail::OpError& e) {
            problem = e.problem;
            return false;
        }
    }

    bool resolveLayer(SpriteDoc& sprite, const nlohmann::json& value, const std::string& field, bool pixels, int& out, PixelProblem& problem) {
        AddressingContext context(sprite);
        try {
            out = context.state.parseLayer(value, field, pixels);
            return true;
        } catch (const detail::OpError& e) {
            problem = e.problem;
            return false;
        }
    }

    bool applyOps(SpriteDoc& working, const EditRequest& request, const EditEnv& env, EditResult& result, PixelProblem& problem, int& opIndex) {
        using namespace detail;
        result = EditResult();
        if (!validateOps(request.ops, problem, opIndex)) return false;
        opIndex = -1;
        if (working.doc.width > kMaxEditSide || working.doc.height > kMaxEditSide) {
            problem = {"canvas_too_large", "sprite", "the sprite is " + std::to_string(working.doc.width) + "x" + std::to_string(working.doc.height) +
                       ", above the 4096x4096 edit_sprite edits", "read and render it; crop or split it in Aseprite to edit it here (canvas is at most 4096x4096)"};
            return false;
        }
        if (PixelProblem refusal = editRefusal(working); !refusal.empty()) {
            problem = refusal;
            return false;
        }
        EditState state(working, request, env, result);
        try {
            // R15: the batch's symmetry
            const Json& sym = request.symmetry;
            if (!sym.is_null()) {
                if (!sym.is_object()) fail("bad_op_field", "symmetry", "symmetry must be an object {x, y, diagonal}", "symmetry: {\"x\": true}");
                for (auto it = sym.begin(); it != sym.end(); ++it) {
                    if (it.key() != "x" && it.key() != "y" && it.key() != "diagonal") {
                        fail("bad_op_field", "symmetry." + it.key(), "symmetry has no field \"" + it.key() + "\"", "symmetry takes: x, y, diagonal");
                    }
                }
                auto axis = [&](const char* name, int side, bool& on, int& twice) {
                    if (!sym.contains(name)) return;
                    const Json& v = sym[name];
                    if (v.is_boolean()) {
                        on = v.get<bool>();
                        twice = side - 1;
                    } else if (v.is_number()) {
                        on = true;
                        const double a = v.get<double>();
                        if (a < 0 || a > side - 1) {
                            fail("bad_op_field", std::string("symmetry.") + name, std::string("symmetry.") + name + " must be true or an axis 0-" + std::to_string(side - 1),
                                 "the axis is in pixels from 0, halves allowed (15.5 is between pixels 15 and 16)");
                        }
                        twice = static_cast<int>(std::floor(a * 2 + 0.5));
                    } else {
                        fail("bad_op_field", std::string("symmetry.") + name, std::string("symmetry.") + name + " must be true or a number", "");
                    }
                };
                axis("x", working.doc.width, state.mirrorX, state.axisX2);
                axis("y", working.doc.height, state.mirrorY, state.axisY2);
                if (sym.contains("diagonal")) {
                    const std::string d = enumField(sym, "diagonal", "", {"xy", "x_minus_y", "both"});
                    state.diagXY = d == "xy" || d == "both";
                    state.diagXMinusY = d == "x_minus_y" || d == "both";
                }
            }
            for (size_t i = 0; i < request.ops.size(); i++) {
                opIndex = static_cast<int>(i);
                state.opIndex = opIndex;
                state.opChanges = 0;
                const Json& op = request.ops[i];
                const std::string name = op["op"].get<std::string>();
                const std::string family = opFamily(name);
                state.ignoreSelection = op.value("ignore_selection", false);
                const int structureBefore = state.structureOps;
                if (family == "structure") {
                    if (working.isPng() && name != "palette" && name != "canvas") {
                        fail("png_structure", "op", "a plain PNG has one frame and one layer: " + name + " ops need an .aseprite",
                             "create_sprite {from: <the png>} makes an .aseprite");
                    }
                    state.flush(true);
                    runStructureOp(state, op, name);
                } else if (family == "transform") {
                    runTransformOp(state, op, name);
                } else {
                    runPaintOp(state, op, name);
                }
                result.opKinds.push_back(name);
                result.applied++;
                if (state.opChanges == 0 && state.structureOps == structureBefore && name != "select") {
                    state.warn("nothing_drawn", "ops[" + std::to_string(i) + "] (" + name + ") changed nothing");
                }
            }
            opIndex = -1;
            state.finish();
        } catch (const OpError& e) {
            problem = e.problem;
            if (opIndex >= 0 && problem.field.rfind("ops[", 0) != 0 && problem.field != "symmetry" && problem.field.rfind("symmetry.", 0) != 0) {
                problem.field = "ops[" + std::to_string(opIndex) + "]" + (problem.field.empty() ? "" : "." + problem.field);
            }
            return false;
        }
        return true;
    }

}
