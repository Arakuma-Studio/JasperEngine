// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "PsdImport.h"

#include "PsColor.h"
#include "PsdAdjustments.h"
#include "PsdEngineData.h"
#include "PsdReader.h"
#include "texsynth/SynthImage.h"
#include "titleart/TitleArtIO.h"
#include "titleart/TitleEffectRegistry.h"
#include "titleart/fonts/FontTables.h"
#include "titleart/fonts/SystemFontList.h"
#include "titleart/interchange/TitleImport.h"
#include "util/Sha256.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::psd;
namespace fs = std::filesystem;

namespace {

    constexpr size_t MAX_FILE = 2048ull << 20;

    bool readFile(const fs::path& path, std::vector<uint8_t>& out, std::string& error) {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            error = "cannot read '" + path.string() + "'";
            return false;
        }
        in.seekg(0, std::ios::end);
        const std::streamoff size = in.tellg();
        if (size < 0 || (uint64_t)size > MAX_FILE) {
            error = "'" + path.filename().string() + "' is larger than 2 GiB";
            return false;
        }
        in.seekg(0);
        out.resize((size_t)size);
        in.read(reinterpret_cast<char*>(out.data()), size);
        return (bool)in;
    }

    std::string lower(std::string s) {
        for (char& c : s) c = (char)std::tolower((unsigned char)c);
        return s;
    }

    // letters and digits only, lowercase ("DelaGothicOne-Regular" -> "delagothiconeregular")
    std::string squash(const std::string& s) {
        std::string out;
        for (unsigned char c : s) if (std::isalnum(c)) out += (char)std::tolower(c);
        return out;
    }

    struct Notes {
        std::vector<TitleImportNote> list;
        std::set<std::string> seen;
        void add(const std::string& layer, const std::string& field, const std::string& what, const std::string& action, const std::string& code = {}) {
            TitleImportNote n{layer, field, what, action, code};
            if (seen.insert(n.toLine()).second) list.push_back(std::move(n));
        }
    };

    Json assetRef(const std::string& path) {
        return Json{{"path", path}, {"guid", ""}};
    }

    // ---- fonts ----

    struct FontMatch {
        std::string assetPath;          // assets-relative ("" none)
        std::string family;
        int faceIndex = 0;
    };

    class FontFinder {
    public:
        FontFinder(const fs::path& assets) : assetsDir(assets) {}

        // the project font of a PostScript name, else a system font of its family (copied in)
        FontMatch find(const std::string& postScript, TitleImportAssets& files, std::string& how) {
            if (postScript.empty()) return {};
            scan();
            const std::string wanted = squash(postScript);
            for (const Entry& e : project) {
                if (squash(e.postScript) == wanted) {
                    how = "project";
                    return {e.assetPath, e.family, e.face};
                }
            }
            // the family part of "Family-Style"
            const std::string family = squash(postScript.substr(0, postScript.find('-')));
            for (const Entry& e : project) {
                if (squash(e.family) == family) {
                    how = "project family";
                    return {e.assetPath, e.family, e.face};
                }
            }
            for (const titlefonts::SystemFontFace& f : titlefonts::SystemFontList::instance().faces()) {
                if (squash(f.family) != family) continue;
                std::vector<uint8_t> bytes;
                std::string error;
                if (!readFile(fs::path(f.path), bytes, error)) continue;
                how = "system";
                const std::string path = files.add(fs::path(f.path).filename().string(), std::string(bytes.begin(), bytes.end()), "fonts");
                return {path, f.family, 0};
            }
            return {};
        }

    private:
        struct Entry {
            std::string assetPath, postScript, family;
            int face = 0;
        };
        fs::path assetsDir;
        std::vector<Entry> project;
        bool scanned = false;

        void scan() {
            if (scanned) return;
            scanned = true;
            std::error_code ec;
            size_t seen = 0;
            for (fs::recursive_directory_iterator it(assetsDir, ec), end; !ec && it != end && seen < 4000; it.increment(ec)) {
                if (!it->is_regular_file(ec)) continue;
                const std::string ext = lower(it->path().extension().string());
                if (ext != ".ttf" && ext != ".otf" && ext != ".ttc") continue;
                seen++;
                std::vector<uint8_t> bytes;
                std::string error;
                if (!readFile(it->path(), bytes, error)) continue;
                const std::vector<uint32_t> faces = titlefonts::sfntFaceOffsets(bytes.data(), bytes.size());
                for (size_t face = 0; face < std::max<size_t>(1, faces.size()) && face < 32; face++) {
                    const titlefonts::FontNames names = titlefonts::fontNames(bytes.data(), bytes.size(), (int)face);
                    std::error_code rel;
                    const std::string path = fs::relative(it->path(), assetsDir, rel).generic_string();
                    project.push_back({path, names.postScriptName, names.family, (int)face});
                }
            }
        }
    };

    // ---- pixels ----

    texsynth::Image layerImage(const PsdFile& file, const PsdLayerRecord& l, PsdRect& trimmed) {
        const int w = l.bounds.width(), h = l.bounds.height();
        const int bps = file.bytesPerSample();
        const bool gray = file.mode == (uint16_t)PsdColorMode::Grayscale;
        const auto sample = [&](const PsdPlane& p, size_t i, uint16_t fallback) -> uint16_t {
            if (!p.present()) return fallback;
            if (bps == 2) {
                uint16_t v;
                std::memcpy(&v, p.samples.data() + i * 2, 2);
                return v;
            }
            return p.samples[i];
        };
        const uint16_t opaque = bps == 2 ? 65535 : 255;
        int x0 = w, y0 = h, x1 = -1, y1 = -1;
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                if (sample(l.alpha, (size_t)y * w + x, opaque) == 0) continue;
                x0 = std::min(x0, x);
                y0 = std::min(y0, y);
                x1 = std::max(x1, x);
                y1 = std::max(y1, y);
            }
        }
        if (x1 < x0) {
            trimmed = PsdRect();
            return texsynth::Image();
        }
        trimmed = {l.bounds.top + y0, l.bounds.left + x0, l.bounds.top + y1 + 1, l.bounds.left + x1 + 1};
        texsynth::Image img = texsynth::Image::make(x1 - x0 + 1, y1 - y0 + 1, 4, bps * 8);
        for (int y = 0; y < img.height; y++) {
            for (int x = 0; x < img.width; x++) {
                const size_t i = (size_t)(y + y0) * w + (x + x0);
                const uint16_t a = sample(l.alpha, i, opaque);
                const uint16_t r = sample(l.planes[0], i, 0);
                const uint16_t v[4] = {r, gray ? r : sample(l.planes[1], i, 0), gray ? r : sample(l.planes[2], i, 0), a};
                uint8_t* px = img.pixel(x, y);
                for (int c = 0; c < 4; c++) {
                    if (bps == 2) std::memcpy(px + c * 2, &v[c], 2);
                    else px[c] = (uint8_t)v[c];
                }
            }
        }
        return img;
    }

    std::string pngOf(const texsynth::Image& img) {
        std::vector<uint8_t> png;
        if (!texsynth::encodePng(img, png)) return std::string();
        return std::string(png.begin(), png.end());
    }

    // a grey image of the whole canvas: the user mask pasted on its default value
    texsynth::Image canvasMask(const PsdFile& file, const PsdLayerRecord& l) {
        texsynth::Image img = texsynth::Image::make(file.width, file.height, 1, 8);
        std::fill(img.data.begin(), img.data.end(), l.maskDefault);
        const int w = l.maskBounds.width(), h = l.maskBounds.height();
        if (!l.maskPlane.present()) return img;
        const int bps = file.bytesPerSample();
        for (int y = 0; y < h; y++) {
            const int cy = l.maskBounds.top + y;
            if (cy < 0 || cy >= file.height) continue;
            for (int x = 0; x < w; x++) {
                const int cx = l.maskBounds.left + x;
                if (cx < 0 || cx >= file.width) continue;
                const size_t i = (size_t)y * w + x;
                uint8_t v;
                if (bps == 2) {
                    uint16_t s;
                    std::memcpy(&s, l.maskPlane.samples.data() + i * 2, 2);
                    v = (uint8_t)((s * 255u + 32767u) / 65535u);
                } else {
                    v = l.maskPlane.samples[i];
                }
                img.pixel(cx, cy)[0] = v;
            }
        }
        return img;
    }

    // the vector mask's paths filled (even-odd) on the canvas, 4 x 4 samples a pixel
    texsynth::Image vectorMask(const PsdFile& file, const PsdLayerRecord& l) {
        struct P {
            double x, y;
        };
        std::vector<std::vector<P>> polygons;
        ByteReader r(l.vectorMask.data(), l.vectorMask.size());
        bool initialFill = false;
        while (r.remaining() >= 26 && r.ok()) {
            ByteReader rec = r.sub(26);
            const uint16_t selector = rec.u16();
            if (selector == 8) {
                initialFill = rec.u16() != 0;
                continue;
            }
            if (selector != 0 && selector != 3) continue;
            const uint16_t count = rec.u16();
            const bool closed = selector == 0;
            struct Knot {
                P in, anchor, out;
            };
            std::vector<Knot> knots;
            for (uint16_t k = 0; k < count && r.remaining() >= 26; k++) {
                ByteReader kr = r.sub(26);
                kr.u16();
                Knot kn;
                P* points[3] = {&kn.in, &kn.anchor, &kn.out};
                for (P* p : points) {
                    const double y = kr.i32() / 16777216.0, x = kr.i32() / 16777216.0;
                    *p = {x * file.width, y * file.height};
                }
                knots.push_back(kn);
            }
            if (knots.size() < 2) continue;
            std::vector<P> poly;
            const size_t segments = closed ? knots.size() : knots.size() - 1;
            for (size_t s = 0; s < segments; s++) {
                const Knot& a = knots[s];
                const Knot& b = knots[(s + 1) % knots.size()];
                for (int t = 0; t < 16; t++) {
                    const double u = t / 16.0, v = 1 - u;
                    poly.push_back({v * v * v * a.anchor.x + 3 * v * v * u * a.out.x + 3 * v * u * u * b.in.x + u * u * u * b.anchor.x,
                                    v * v * v * a.anchor.y + 3 * v * v * u * a.out.y + 3 * v * u * u * b.in.y + u * u * u * b.anchor.y});
                }
            }
            polygons.push_back(std::move(poly));
        }
        texsynth::Image img = texsynth::Image::make(file.width, file.height, 1, 8);
        std::vector<uint16_t> coverage((size_t)file.width * file.height, 0);
        std::vector<double> xs;
        for (int y = 0; y < file.height; y++) {
            for (int sy = 0; sy < 4; sy++) {
                const double py = y + (sy + 0.5) / 4;
                xs.clear();
                for (const auto& poly : polygons) {
                    for (size_t i = 0; i < poly.size(); i++) {
                        const P& a = poly[i];
                        const P& b = poly[(i + 1) % poly.size()];
                        if ((a.y <= py) != (b.y <= py)) xs.push_back(a.x + (py - a.y) / (b.y - a.y) * (b.x - a.x));
                    }
                }
                std::sort(xs.begin(), xs.end());
                for (size_t i = 0; i + 1 < xs.size(); i += 2) {
                    for (int sx = 0; sx < file.width * 4; sx++) {
                        const double px = (sx + 0.5) / 4;
                        if (px >= xs[i] && px < xs[i + 1]) coverage[(size_t)y * file.width + sx / 4]++;
                    }
                }
            }
        }
        for (size_t i = 0; i < coverage.size(); i++) {
            uint8_t v = (uint8_t)std::min(255, coverage[i] * 255 / 16);
            if (initialFill) v = (uint8_t)(255 - v);
            if (l.vectorMaskInvert) v = (uint8_t)(255 - v);
            img.data[i] = v;
        }
        return img;
    }

    // ---- the document ----

    struct Builder {
        const PsdFile& file;
        const TitleCreateRequest& request;
        TitleImportAssets assets;
        FontFinder fonts;
        Notes notes;
        std::vector<PsPattern> patterns;

        Builder(const PsdFile& f, const TitleCreateRequest& r)
            : file(f), request(r), assets(fs::path(r.documentPath), fs::path(r.assetsPath)), fonts(fs::path(r.assetsPath)) {}

        Json position(const PsdRect& b) const {
            return Json::array({(b.left + b.right) / 2.0 - file.width / 2.0, file.height / 2.0 - (b.top + b.bottom) / 2.0});
        }

        std::string savePattern(const PsPattern& p) {
            texsynth::Image img = texsynth::Image::make(p.width, p.height, 4, 8);
            img.data = p.rgba;
            return assets.add(p.name + ".png", pngOf(img), "patterns");
        }

        void styles(Json& layer, const PsdLayerRecord& l, const std::string& path, double refSize) {
            if (!psEffectsEnabled(l.effects)) {
                if (l.fillOpacity != 255) layer["style"]["face"]["opacity"] = l.fillOpacity / 255.0;
                if (l.knockout) layer["style"]["knockout"] = true;
                return;
            }
            PsStyleSource src;
            src.name = l.name;
            src.effects = l.effects;
            src.ppi = file.dpi;
            src.globalAngle = file.globalAngle;
            src.globalAltitude = file.globalAltitude;
            src.fillOpacity = l.fillOpacity / 255.0;
            src.knockout = l.knockout;
            std::string face = "#ffffff";
            if (auto st = layer.find("style"); st != layer.end() && st->is_object() && st->contains("face") && (*st)["face"].contains("fill")) {
                face = (*st)["face"]["fill"].value("color", face);
            }
            src.baseColor = face;
            PsConvertOptions options;
            options.savePattern = [this](const PsPattern& p) { return savePattern(p); };
            ConvertedPsStyle converted;
            std::string error;
            if (!convertPsStyle(src, std::max(1.0, refSize), patterns, options, converted, error)) {
                notes.add(path, "effects", error, "dropped");
                return;
            }
            Json style = converted.style;
            // an image layer's face shows its own pixels unless an overlay fills it
            const Json& faceFill = style["face"]["fill"];
            if (titleart::layerTypeOf(layer) == titleart::LayerType::Image && faceFill.value("kind", std::string()) == "solid" &&
                faceFill.value("color", std::string()) == face) {
                style["face"]["fill"] = Json{{"kind", "source"}};
            }
            layer["style"] = style;
            layer["effects"] = converted.effects;
            for (const std::string& n : converted.notes) notes.add(path, "effects", n, "approximated");
        }

        void common(Json& layer, const PsdLayerRecord& l, const std::string& path) {
            layer["name"] = l.name.empty() ? layer.value("name", std::string("Layer")) : l.name;
            layer["visible"] = l.visible;
            layer["opacity"] = l.opacity / 255.0;
            layer["clip"] = l.clipping;
            const std::string blend = titleBlendOfPsKey(l.blendKey);
            if (l.section == 1 || l.section == 2) {
                // a group's blend is its section's (pass-through included), set by list()
            } else if (blend.empty()) {
                layer["blend"] = "normal";
                notes.add(path, "blend", "blend mode '" + l.blendKey + "' has no Title Art equivalent: drawn Normal", "approximated");
            } else {
                layer["blend"] = blend;
            }
            Json masks = Json::array();
            if (l.hasMask) {
                const texsynth::Image m = canvasMask(file, l);
                const std::string name = assets.add(l.name + "_mask.png", pngOf(m));
                masks.push_back(Json{{"type", "image"}, {"enabled", !l.maskDisabled}, {"invert", l.maskInvert}, {"linked", false},
                                     {"image", assetRef(name)}, {"channel", "luminance"}, {"position", Json::array({0, 0})},
                                     {"size", std::max(file.width, file.height)}});
            }
            if (!l.vectorMask.empty()) {
                const texsynth::Image m = vectorMask(file, l);
                const std::string name = assets.add(l.name + "_vector_mask.png", pngOf(m));
                masks.push_back(Json{{"type", "image"}, {"enabled", !l.vectorMaskDisabled}, {"linked", false}, {"image", assetRef(name)},
                                     {"channel", "luminance"}, {"position", Json::array({0, 0})}, {"size", std::max(file.width, file.height)}});
                notes.add(path, "masks", "vector mask rasterised to an image mask", "approximated");
            }
            if (!masks.empty()) layer["masks"] = masks;
        }

        bool textLayer(Json& layer, const PsdLayerRecord& l, const std::string& path, double& refSize) {
            const std::vector<uint8_t> raw = descRaw(l.textDescriptor, "EngineData");
            Json engine;
            std::string error;
            PsTextStyle style;
            if (raw.empty() || !parseEngineData(raw, engine, error) || !firstTextStyle(engine, style)) {
                const std::string text = descText(l.textDescriptor, "Txt ");
                if (text.empty()) return false;
                style.text = text;
                notes.add(path, "text", "the text's EngineData is unreadable" + (error.empty() ? std::string() : " (" + error + ")") +
                                        ": its font, size and colour are defaults", "approximated");
            }
            const double k = std::hypot(l.transform[2], l.transform[3]) > 0 ? std::hypot(l.transform[2], l.transform[3]) : 1;
            const double sizePx = style.fontSize > 0 ? style.fontSize * k * file.dpi / 72 : std::max(1, l.bounds.height());
            refSize = sizePx;
            layer = Json{{"type", "text"}, {"text", style.text}, {"size", sizePx},
                         {"tracking", style.tracking}, {"align", style.justification == 1 ? "end" : style.justification == 2 ? "center" : "start"}};
            if (style.leading > 0 && style.fontSize > 0) layer["line_height"] = style.leading / style.fontSize;
            if (style.hasColor) {
                layer["style"] = Json{{"face", Json{{"fill", Json{{"kind", "solid"}, {"color", rgbHex(style.color[0], style.color[1], style.color[2])}}}}}};
            }
            if (!style.font.empty()) {
                std::string how;
                const FontMatch match = fonts.find(style.font, assets, how);
                if (!match.assetPath.empty()) {
                    layer["font"] = assetRef(match.assetPath);
                    layer["face_index"] = match.faceIndex;
                    if (how == "system") notes.add(path, "font", "font '" + style.font + "' copied in from the system fonts (" + match.family + ")", "note");
                } else {
                    notes.add(path, "font", "font '" + style.font + "' is not in the project or the system: the built-in face is used (find it with the font picker)",
                              "approximated", "font_unavailable");
                }
            }
            if (style.runs > 1) notes.add(path, "text", std::to_string(style.runs) + " style runs: the first run's font, size and colour are used", "approximated");
            if (l.warp) notes.add(path, "warp", "warped text: the warp is not imported", "dropped");
            layer["transform"] = Json{{"position", position(l.bounds)}};
            return true;
        }

        bool adjustmentLayer(Json& layer, const PsdLayerRecord& l, const std::string& path) {
            Json adjustment;
            std::string error;
            if (!psdAdjustmentToTitle(l.adjustmentKey, l.adjustmentData, adjustment, error)) {
                notes.add(path, "adjustment", error, "dropped");
                return false;
            }
            layer = Json{{"type", "adjustment"}, {"adjustment", adjustment}};
            return true;
        }

        bool fillLayer(Json& layer, const PsdLayerRecord& l, const std::string& path) {
            Json params;
            if (l.fillKey == "SoCo") {
                const Json* c = descObject(l.fillDescriptor, "Clr ");
                params = Json{{"kind", "solid"}, {"color", c ? psColorHex(*c, "#202020") : "#202020"}};
            } else if (l.fillKey == "GdFl") {
                // the gradient fill layer as a style gradient overlay, read through the style converter
                PsStyleSource src;
                Json g = l.fillDescriptor;
                g["enab"] = true;
                src.effects = Json{{"_class", "null"}, {"GrFl", g}};
                ConvertedPsStyle converted;
                std::string error;
                if (!convertPsStyle(src, 100, {}, {}, converted, error)) return false;
                // a damaged gradient may convert to no fill at all: untrusted input, nothing is assumed
                const Json face = converted.style.value("face", Json::object());
                Json fill = face.is_object() ? face.value("fill", Json::object()) : Json::object();
                if (!fill.is_object()) fill = Json::object();
                if (fill.value("kind", std::string("linear")) == "solid" || !fill.contains("gradient")) {
                    params = Json{{"kind", "solid"}, {"color", fill.value("color", std::string("#202020"))}};
                } else {
                    params = Json{{"kind", fill.value("kind", std::string("linear"))}, {"gradient", fill["gradient"]},
                                  {"angle", fill.value("angle", 90.0)}, {"scale", std::clamp(fill.value("scale", 1.0), 0.1, 10.0)},
                                  {"offset", fill.value("offset", 0.0)}};
                }
                for (const std::string& n : converted.notes) notes.add(path, "fill", n, "approximated");
            } else {
                notes.add(path, "fill", "pattern fill layer: no Title Art generator draws a bitmap pattern over the canvas", "dropped");
                return false;
            }
            layer = Json{{"type", "generator"}, {"generator", Json{{"type", "fill"}, {"params", params}}}};
            return true;
        }

        bool pixelLayer(Json& layer, const PsdLayerRecord& l, const std::string& path) {
            PsdRect trimmed;
            const texsynth::Image img = layerImage(file, l, trimmed);
            if (img.empty()) {
                notes.add(path, "", "an empty layer", "dropped");
                return false;
            }
            const std::string png = assets.add((l.name.empty() ? std::string("layer") : l.name) + ".png", pngOf(img));
            layer = Json{{"type", "image"}, {"image", assetRef(png)}, {"size", std::max(img.width, img.height)},
                         {"transform", Json{{"position", position(trimmed)}}}};
            if (l.smartObject) notes.add(path, "", "smart object imported as its pixels", "approximated");
            return true;
        }

        Json list(const std::vector<PsdNode>& nodes, const std::string& parent) {
            Json out = Json::array();
            std::map<std::string, int> counts;
            for (const PsdNode& node : nodes) {
                const PsdLayerRecord& l = file.layers[(size_t)node.layer];
                const int n = counts[l.name]++;
                const std::string path = parent + l.name + (n ? "#" + std::to_string(n) : "");
                Json layer;
                double refSize = std::max(1, l.bounds.height());
                bool made = false;
                if (l.section == 1 || l.section == 2) {
                    layer = Json{{"type", "group"}, {"collapsed", l.section == 2}, {"children", list(node.children, path + "/")}};
                    made = true;
                } else if (l.text) {
                    made = textLayer(layer, l, path, refSize);
                } else if (!l.adjustmentKey.empty()) {
                    made = adjustmentLayer(layer, l, path);
                } else if (!l.fillKey.empty()) {
                    made = fillLayer(layer, l, path);
                } else {
                    made = pixelLayer(layer, l, path);
                }
                if (!made) continue;
                common(layer, l, path);
                if (l.section == 1 || l.section == 2) {
                    if (l.sectionBlend == "pass") {
                        layer["blend"] = "normal";
                        notes.add(path, "blend", "pass-through group drawn as Normal", "approximated");
                    } else if (!l.sectionBlend.empty()) {
                        const std::string blend = titleBlendOfPsKey(l.sectionBlend);
                        layer["blend"] = blend.empty() ? "normal" : blend;
                    }
                    if (psEffectsEnabled(l.effects)) notes.add(path, "effects", "a group's layer style is not imported", "dropped");
                } else if (titleart::layerTypeHasStyle(titleart::layerTypeOf(layer))) {
                    styles(layer, l, path, refSize);
                } else if (psEffectsEnabled(l.effects)) {
                    notes.add(path, "effects", "the layer style of a " + std::string(titleart::layerTypeName(titleart::layerTypeOf(layer))) +
                                               " layer is not imported", "dropped");
                }
                out.push_back(std::move(layer));
            }
            return out;
        }
    };

    std::string headOf(const std::string& head, size_t at, size_t n) {
        return head.size() >= at + n ? head.substr(at, n) : std::string();
    }

    fs::path sourcePath(const TitleOpContext& ctx, const std::string& file) {
        const fs::path p(file);
        if (p.is_absolute() || !ctx.options || ctx.options->projectPath.empty()) return p;
        return (fs::path(ctx.options->projectPath) / p).lexically_normal();
    }

    // reads `file`'s styles; false after ctx.fail
    bool loadStyles(TitleOpContext& ctx, const Json& op, PsStyleFile& styles, fs::path& path) {
        std::string file;
        if (!ctx.requireString(op, "file", file)) return false;
        path = sourcePath(ctx, file);
        std::vector<uint8_t> bytes;
        std::string error;
        if (!readFile(path, bytes, error)) return ctx.fail("file_not_found", error, "file");
        if (!readPsStyles(bytes.data(), bytes.size(), styles, error)) return ctx.fail("ps_styles_unreadable", error, "file");
        return true;
    }

    // `style` / `styles` entries: a name or an index
    int styleIndex(const PsStyleFile& styles, const Json& ref) {
        if (ref.is_number_integer()) {
            const int i = ref.get<int>();
            return i >= 0 && (size_t)i < styles.styles.size() ? i : -1;
        }
        if (ref.is_string()) {
            for (size_t i = 0; i < styles.styles.size(); i++) if (styles.styles[i].name == ref.get<std::string>()) return (int)i;
        }
        return -1;
    }

    std::vector<std::string> styleNames(const PsStyleFile& styles) {
        std::vector<std::string> out;
        for (const PsStyleSource& s : styles.styles) out.push_back(s.name);
        return out;
    }

    // px params of an effect entry scaled by k (S84's rule: a look's px are for its ref_size)
    void scaleEffect(Json& entry, double k) {
        const titleart::EffectDef* def = titleart::findEffect(entry.value("type", std::string()));
        if (!def || !entry.contains("params")) return;
        for (const titleart::ParamDef& p : def->params) {
            if (p.unit != titleart::ParamUnit::Px || !entry["params"].contains(p.key) || !entry["params"][p.key].is_number()) continue;
            entry["params"][p.key] = entry["params"][p.key].get<double>() * k;
        }
    }

    Json report(const ConvertedPsStyle& c) {
        return Json{{"summary", c.summary}, {"notes", c.notes}, {"ref_size", c.refSize}};
    }

    bool applyPsStyle(TitleOpContext& ctx, const Json& op) {
        Json* layer = ctx.layer(op);
        if (!layer) return false;
        if (!titleart::layerTypeHasStyle(titleart::layerTypeOf(*layer))) {
            return ctx.fail("bad_layer", "'" + layer->value("name", std::string()) + "' has no style (text, shape, vector and image layers do)", "layer");
        }
        PsStyleFile styles;
        fs::path path;
        if (!loadStyles(ctx, op, styles, path)) return false;
        const int index = styleIndex(styles, op.value("style", Json(0)));
        if (index < 0) return ctx.fail("bad_value", "the file has no style " + op.value("style", Json(0)).dump(), "style", styleNames(styles));
        const PsStyleSource& src = styles.styles[(size_t)index];
        double ref = psReferenceSize(src);
        if (op.contains("reference_px")) {
            if (!op["reference_px"].is_number() || op["reference_px"].get<double>() <= 0) return ctx.fail("bad_value", "`reference_px` is a size above 0", "reference_px");
            ref = op["reference_px"].get<double>();
        }
        TitleImportAssets assets(fs::path(ctx.options ? ctx.options->documentPath : std::string()), fs::path(ctx.options ? ctx.options->assetsPath : std::string()));
        PsConvertOptions options;
        options.savePattern = [&](const PsPattern& p) {
            if (!ctx.options || ctx.options->documentPath.empty()) return std::string();
            texsynth::Image img = texsynth::Image::make(p.width, p.height, 4, 8);
            img.data = p.rgba;
            return assets.add(p.name + ".png", pngOf(img), "patterns");
        };
        ConvertedPsStyle converted;
        std::string error;
        if (!convertPsStyle(src, ref, styles.patterns, options, converted, error)) return ctx.fail("bad_value", error, "reference_px");
        if (!ctx.set(layer, "style", converted.style)) return false;
        const double k = titleart::layerSourceSize(*layer) / ref;
        Json effects = converted.effects;
        for (Json& e : effects) scaleEffect(e, k);
        if (!ctx.set(layer, "effects", effects)) return false;
        for (TitleOpFile& f : assets.take()) ctx.result.files.push_back(std::move(f));
        Json r = report(converted);
        r["style"] = src.name;
        r["size_note"] = src.sizeNote;
        ctx.result.reports[std::to_string(ctx.opIndex)] = r;
        ctx.undoName = "Apply Photoshop Style " + src.name;
        return true;
    }

    bool importPsStyles(TitleOpContext& ctx, const Json& op) {
        PsStyleFile styles;
        fs::path path;
        if (!loadStyles(ctx, op, styles, path)) return false;
        const std::string scope = op.value("scope", std::string("project"));
        if (scope != "project") {
            return ctx.fail("bad_value", "scope '" + scope + "': Photoshop styles import into the project's library", "scope", {"project"});
        }
        if (!ctx.options || ctx.options->projectPath.empty()) return ctx.fail("no_project", "importing looks needs a project", "file");
        std::vector<int> picked;
        if (op.contains("styles")) {
            if (!op["styles"].is_array()) return ctx.fail("bad_op", "`styles` is a list of style names or indexes", "styles");
            for (const Json& ref : op["styles"]) {
                const int i = styleIndex(styles, ref);
                if (i < 0) return ctx.fail("bad_value", "the file has no style " + ref.dump(), "styles", styleNames(styles));
                picked.push_back(i);
            }
        } else {
            for (size_t i = 0; i < styles.styles.size(); i++) picked.push_back((int)i);
        }
        const fs::path looks = fs::path(ctx.options->projectPath) / "EditorSettings" / "TitleLibrary" / "Looks";
        const bool overwrite = op.value("overwrite", false);
        const std::string stem = psLookSlug(path.stem().string());
        Json list = Json::array();
        std::set<std::string> slugs;
        for (int i : picked) {
            const PsStyleSource& src = styles.styles[(size_t)i];
            double ref = op.contains("reference_px") && op["reference_px"].is_number() ? op["reference_px"].get<double>() : psReferenceSize(src);
            std::string slug = psLookSlug(src.name);
            for (int n = 2; slugs.count(slug); n++) slug = psLookSlug(src.name) + "-" + std::to_string(n);
            slugs.insert(slug);
            const fs::path target = looks / (slug + ".titlelook");
            std::error_code ec;
            if (fs::exists(target, ec) && !overwrite) {
                return ctx.fail("library_item_exists", "the project look 'project/" + slug + "' exists: pass overwrite true to replace it", "styles");
            }
            Json assetList = Json::array();
            std::vector<TitleOpFile> files;
            PsConvertOptions options;
            options.savePattern = [&](const PsPattern& p) {
                texsynth::Image img = texsynth::Image::make(p.width, p.height, 4, 8);
                img.data = p.rgba;
                const std::string png = pngOf(img);
                const std::string key = "pattern-" + std::to_string(assetList.size() + 1);
                const std::string name = titleSafeFileName(p.name) + ".png";
                files.push_back({(looks / (slug + ".files") / name).string(), png});
                assetList.push_back(Json{{"key", key}, {"file", slug + ".files/" + name}, {"sha256", sha256Hex(png)}});
                return std::string("asset:") + key;
            };
            ConvertedPsStyle converted;
            std::string error;
            if (!convertPsStyle(src, ref, styles.patterns, options, converted, error)) return ctx.fail("bad_value", error, "reference_px");
            const Json look = psLookJson(slug, src, converted, stem, assetList);
            files.push_back({target.string(), titleart::writeTitleArtJson(look) + "\n"});
            for (TitleOpFile& f : files) ctx.result.files.push_back(std::move(f));
            Json r = report(converted);
            r["id"] = "project/" + slug;
            r["style"] = src.name;
            list.push_back(r);
        }
        ctx.result.reports[std::to_string(ctx.opIndex)] = Json{{"looks", list}, {"skipped", styles.skipped}};
        ctx.undoName = "Import Photoshop Styles (" + path.filename().string() + ")";
        return true;
    }

}

std::string psd::psLookSlug(const std::string& name) {
    std::string out;
    bool gap = false;
    for (unsigned char c : name) {
        if (c < 128 && std::isalnum(c)) {
            if (gap && !out.empty()) out += '-';
            out += (char)std::tolower(c);
            gap = false;
        } else {
            gap = true;
        }
        if (out.size() >= 48) break;
    }
    return out.empty() ? "style" : out;
}

titleart::Json psd::psLookJson(const std::string& slug, const PsStyleSource& source, const ConvertedPsStyle& converted, const std::string& fileStem,
                               const Json& assets) {
    Json effects = Json::array();
    for (const Json& e : converted.effects) {
        Json entry = Json{{"type", e["type"]}};
        if (!e.value("enabled", true)) entry["enabled"] = false;
        entry["params"] = e["params"];
        effects.push_back(entry);
    }
    Json look = Json{{"format", "jasperengine.titlelook"}, {"version", 1}, {"id", "project/" + slug}, {"revision", 1},
                     {"name", Json{{"en", source.name}}}, {"description", Json{{"en", "Imported from a Photoshop style"}}},
                     {"family", "custom"}, {"tags", Json::array({"photoshop", fileStem})}, {"synonyms", Json{{"en", Json::array()}, {"ja", Json::array()}}},
                     {"dimension", "2d"}, {"applies", "style"}, {"ref_size", converted.refSize}, {"style", converted.style},
                     {"effects", effects}, {"part_effects", Json::object()}, {"generators", Json::array()}, {"group_effects", Json::array()},
                     {"links", Json::object()}, {"lint_allow", Json::array()}, {"check_allow", Json::array()}};
    if (!assets.empty()) look["assets"] = assets;
    look["provenance"] = Json{{"source", "user"}, {"item", source.name}, {"license_note", "imported from a Photoshop " + source.origin}};
    return look;
}

bool psd::importPsdDocument(const TitleCreateRequest& request, TitleCreateResult& out, std::string& code, std::string& error) {
    std::vector<uint8_t> bytes;
    if (!readFile(fs::path(request.sourcePath), bytes, error)) {
        code = "file_not_found";
        return false;
    }
    PsdFile file;
    PsdReadOptions options;
    options.mergedImage = false;
    if (!readPsd(bytes.data(), bytes.size(), file, error, options)) {
        code = "psd_unreadable";
        return false;
    }
    if (!file.rgbOrGray() || (file.depth != 8 && file.depth != 16)) {
        code = "psd_mode_unsupported";
        error = "the PSD is " + std::to_string(file.depth) + "-bit " + psdColorModeName(file.mode) +
                ": layers import from RGB or Grayscale 8- and 16-bit files; its styles can still be imported (the import_ps_styles op)";
        return false;
    }
    Builder b(file, request);
    readPatterns(file.patterns.data(), file.patterns.size(), b.patterns, nullptr);
    titleart::TitleArtDoc doc = titleart::makeNewTitleArt();
    doc.root["name"] = fs::path(request.sourcePath).stem().string();
    doc.root["canvas"]["width"] = std::clamp(file.width, titleart::TITLE_MIN_CANVAS_SIDE, titleart::TITLE_MAX_CANVAS_SIDE);
    doc.root["canvas"]["height"] = std::clamp(file.height, titleart::TITLE_MIN_CANVAS_SIDE, titleart::TITLE_MAX_CANVAS_SIDE);
    if (file.width > titleart::TITLE_MAX_CANVAS_SIDE || file.height > titleart::TITLE_MAX_CANVAS_SIDE) {
        b.notes.add("", "canvas", "the PSD is " + std::to_string(file.width) + " x " + std::to_string(file.height) + " px: the canvas is clamped to 8,192",
                    "approximated");
    }
    doc.root["dpi"] = std::clamp(file.dpi, 1.0, 2400.0);
    doc.root["global_light"] = Json{{"angle", file.globalAngle}, {"altitude", std::clamp(file.globalAltitude, 0, 90)}};
    doc.root["exposed"] = Json::array();
    if (!file.icc.empty()) {
        const std::string description = iccDescription(file.icc);
        if (lower(description).find("srgb") == std::string::npos) {
            b.notes.add("", "color_profile", "the embedded profile '" + description + "' is ignored: the values are read as sRGB", "dropped",
                        "color_profile_ignored");
        }
    }
    doc.root["layers"] = b.list(psdTree(file), "");
    Json lines = Json::array();
    for (const TitleImportNote& n : b.notes.list) {
        out.notes.push_back(n.toJson());
        lines.push_back(n.toLine());
    }
    doc.root["import_notes"] = lines;
    titleart::normalizeTitleArt(doc, nullptr);
    out.doc = std::move(doc);
    out.kind = "psd";
    out.files = b.assets.take();
    return true;
}

void psd::registerPsdInterchange() {
    TitleArtOps::registerCreateImporter(
        "psd", [](const std::string& head, const std::string&) { return headOf(head, 0, 4) == "8BPS"; }, importPsdDocument);
    TitleArtOps::registerCreateImporter(
        "ps_styles", [](const std::string& head, const std::string& ext) { return headOf(head, 2, 4) == "8BSL" || ext == ".asl"; },
        [](const TitleCreateRequest& request, TitleCreateResult&, std::string& code, std::string& error) {
            code = "ps_styles_not_a_document";
            error = "'" + fs::path(request.sourcePath).filename().string() +
                    "' is a Photoshop style library, not a document: import its styles as Looks with the import_ps_styles op, or apply one "
                    "to a layer with apply_ps_style";
            return false;
        });
    TitleArtOps::registerOp("apply_ps_style", applyPsStyle);
    TitleArtOps::registerOp("import_ps_styles", importPsStyles);
}
