// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// compensateV4CharScale is ported from Pachislop src/io/migrateText.ts by SafEight, used with the
// author's permission.

#include "PachislopImport.h"

#include "PachislopMapper.h"
#include "PachislopMembers.h"
#include "PachislopProject.h"
#include "texsynth/SynthImage.h"
#include "titleart/TitleArtAssets.h"
#include "titleart/TitleArtIO.h"
#include "titleart/interchange/TitleImport.h"
#include "titleart/text/TitleTextLayout.h"
#include "util/FontFace.h"
#include "util/Sha256.h"

#include "stb_image.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::pachislop;
namespace fs = std::filesystem;

namespace {

    constexpr double PI = 3.14159265358979323846;
    constexpr double DEG = PI / 180.0;
    constexpr double EPS = 1e-3;

    bool readFile(const fs::path& path, std::string& out, size_t limit) {
        std::error_code ec;
        const uintmax_t size = fs::file_size(path, ec);
        if (ec || size > limit) return false;
        std::ifstream in(path, std::ios::binary);
        if (!in) return false;
        std::stringstream buffer;
        buffer << in.rdbuf();
        out = buffer.str();
        return true;
    }

    std::string lower(std::string s) {
        for (char& c : s) c = (char)std::tolower((unsigned char)c);
        return s;
    }

    std::string generic(const fs::path& p) {
        return p.lexically_normal().generic_string();
    }

    // a font file's extension from its first bytes
    std::string fontExtension(const std::string& bytes) {
        const std::string head = bytes.substr(0, 4);
        if (head == "OTTO") return ".otf";
        if (head == "ttcf") return ".ttc";
        if (head == "wOFF") return ".woff";
        if (head == "wOF2") return ".woff2";
        return ".ttf";
    }

    std::string withExtension(const std::string& name, const std::string& extension) {
        const fs::path p(name);
        if (lower(p.extension().string()) == extension) return name;
        return name + extension;
    }

    // the staged and existing files an import's layers name, for the v4 compensation's layout
    class ImportResolver : public titleart::AssetResolver {
    public:
        ImportResolver(fs::path assets, const std::vector<TitleOpFile>& staged) : assetsDir(std::move(assets)) {
            for (const TitleOpFile& f : staged) {
                std::error_code ec;
                const fs::path relative = fs::relative(fs::path(f.path), assetsDir, ec);
                if (!ec) files[generic(relative)] = &f.content;
            }
        }
        titleart::ResolvedTitleAsset resolve(const titleart::TitleAssetRef& ref, titleart::TitleAssetKind) const override {
            titleart::ResolvedTitleAsset r;
            r.assetPath = ref.path;
            r.path = ref.path;
            std::error_code ec;
            r.found = files.count(ref.path) || fs::exists(assetsDir / ref.path, ec);
            return r;
        }
        std::shared_ptr<FontFace> loadFont(const titleart::ResolvedTitleAsset& asset, int faceIndex, std::string* error) const override {
            auto key = std::make_pair(asset.path, faceIndex);
            if (auto it = fonts.find(key); it != fonts.end()) return it->second;
            std::shared_ptr<FontFace> face;
            if (auto staged = files.find(asset.path); staged != files.end()) {
                face = FontFace::loadMemory(reinterpret_cast<const unsigned char*>(staged->second->data()), staged->second->size(), faceIndex, true,
                                            asset.path, error, FontFaceRasterizer::Optional);
            } else {
                face = FontFace::loadFile((assetsDir / asset.path).string(), faceIndex, error, FontFaceRasterizer::Optional);
            }
            if (face) fonts[key] = face;
            return face;
        }
        bool readBytes(const titleart::ResolvedTitleAsset& asset, std::vector<uint8_t>& out, std::string* error) const override {
            if (auto staged = files.find(asset.path); staged != files.end()) {
                out.assign(staged->second->begin(), staged->second->end());
                return true;
            }
            std::string text;
            if (!readFile(assetsDir / asset.path, text, MAX_ASSET_BYTES)) {
                if (error) *error = "cannot read " + asset.path;
                return false;
            }
            out.assign(text.begin(), text.end());
            return true;
        }
        bool loadImage(const titleart::ResolvedTitleAsset&, titleart::TitleImageData&, std::string* error) const override {
            if (error) *error = "the import's layout reads no images";
            return false;
        }
        std::shared_ptr<FontFace> builtinFont() const override { return nullptr; }

    private:
        fs::path assetsDir;
        std::map<std::string, const std::string*> files;
        mutable std::map<std::pair<std::string, int>, std::shared_ptr<FontFace>> fonts;
    };

    // ---- the v4 compensation (src/io/migrateText.ts) ----

    bool isScaled(const Json& a) {
        return a.is_object() && a.contains("scale") && a["scale"].is_number() && a["scale"].get<double>() > 0 && a["scale"].get<double>() != 1;
    }

    double pct(double v) { return std::round(v * 1e4) / 1e4; }

    struct Centre {
        double x = 0, y = 0;
    };

    std::map<uint32_t, Centre> centres(titleart::TitleTextLayout& layout, const Json& layer) {
        std::map<uint32_t, Centre> out;
        for (const titleart::LaidOutCluster& c : layout.layout(layer).clusters) {
            out[c.index] = {c.emCell.x + c.emCell.width / 2, c.emCell.y + c.emCell.height / 2};
        }
        return out;
    }

    bool compensateLayer(Json& l, const titleart::AssetResolver* assets) {
        if (!l.contains("chars") || !l["chars"].is_object()) return false;
        bool any = false;
        for (auto it = l["chars"].begin(); it != l["chars"].end(); ++it) any = any || isScaled(*it);
        if (!any) return false;
        Json neutral = l;
        for (auto it = neutral["chars"].begin(); it != neutral["chars"].end(); ++it) (*it)["scale"] = 1;
        titleart::TitleTextLayout layout(assets);
        const std::map<uint32_t, Centre> v4 = centres(layout, neutral);
        const std::map<uint32_t, Centre> v5 = centres(layout, l);
        struct Move { uint32_t index; double x, y; };
        std::vector<Move> moves;
        for (const auto& [index, c] : v5) {
            auto old = v4.find(index);
            const Centre from = old == v4.end() ? c : old->second;
            moves.push_back({index, from.x - c.x, from.y - c.y});
        }
        if (moves.empty()) return false;
        const auto scaledAt = [&](uint32_t index) {
            auto it = l["chars"].find(std::to_string(index));
            return it != l["chars"].end() && isScaled(*it);
        };
        std::vector<Move> plain;
        for (const Move& m : moves) if (!scaledAt(m.index)) plain.push_back(m);
        const std::vector<Move>& ref = plain.empty() ? moves : plain;
        bool sameX = true, sameY = true;
        for (const Move& m : ref) {
            sameX = sameX && std::abs(m.x - ref[0].x) < EPS;
            sameY = sameY && std::abs(m.y - ref[0].y) < EPS;
        }
        const double sx = sameX && std::abs(ref[0].x) >= EPS ? ref[0].x : 0;
        const double sy = sameY && std::abs(ref[0].y) >= EPS ? ref[0].y : 0;
        const bool moved = sx != 0 || sy != 0;
        const double size = l.value("size", 200.0);
        const double k = std::tan(l.value("shear", 0.0) * DEG);
        bool offset = false;
        for (const Move& m : moves) {
            const double x = m.x - sx, y = m.y - sy;
            if (std::abs(x) < EPS && std::abs(y) < EPS) continue;
            Json& a = l["chars"][std::to_string(m.index)];
            if (!a.is_object()) a = Json::object();
            const Json off = a.value("offset", Json::array({0, 0}));
            a["offset"] = Json::array({pct(off[0].get<double>() + ((x - k * y) / size) * 100), pct(off[1].get<double>() + (y / size) * 100)});
            offset = true;
        }
        if (!offset && !moved) return false;
        if (moved) {
            Json& t = l["transform"];
            const double scale = t.value("scale", 1.0);
            const double c = -t.value("rotation", 0.0) * DEG;
            const Json p = t.value("position", Json::array({0, 0}));
            double dx, dy, dz = 0;
            if (l.value("dimension", std::string("2d")) == "3d") {
                const double a = t.value("rotation_x", 0.0) * DEG, b = t.value("rotation_y", 0.0) * DEG;
                const double y1 = sy * std::cos(a), z1 = sy * std::sin(a);
                const double x2 = sx * std::cos(b) + z1 * std::sin(b), z2 = -sx * std::sin(b) + z1 * std::cos(b);
                dx = scale * (x2 * std::cos(c) - y1 * std::sin(c));
                dy = scale * (x2 * std::sin(c) + y1 * std::cos(c));
                dz = scale * z2;
                if (t.contains("z")) t["z"] = t["z"].get<double>() + dz;
            } else {
                const double ax = scale * std::cos(c), ay = scale * std::sin(c);
                dx = ax * sx - ay * sy;
                dy = ay * sx + ax * sy;
            }
            t["position"] = Json::array({p[0].get<double>() + dx, p[1].get<double>() + dy});
        }
        return true;
    }

    bool compensateList(Json& layers, const titleart::AssetResolver* assets) {
        bool changed = false;
        if (!layers.is_array()) return false;
        for (Json& l : layers) {
            if (titleart::layerTypeOf(l) == titleart::LayerType::Text) changed = compensateLayer(l, assets) || changed;
            if (l.contains("children")) changed = compensateList(l["children"], assets) || changed;
        }
        return changed;
    }

    // ---- the reimport op ----

    bool reimport3D(TitleOpContext& ctx, const Json& op) {
        if (!mapperHas3D()) {
            return ctx.fail("not_available", "re-mapping a Pachislop 3D style needs the 3D module (JASPERENGINE_TITLE_ART_3D)", "op");
        }
        std::vector<Json*> targets;
        if (op.contains("layer")) {
            Json* layer = ctx.layer(op);
            if (!layer) return false;
            if (!layer->contains("pachislop_style3d")) {
                return ctx.fail("bad_layer", "'" + layer->value("name", std::string()) + "' has no kept Pachislop 3D style (pachislop_style3d)", "layer");
            }
            targets.push_back(layer);
        } else {
            std::function<void(Json&)> walk = [&](Json& list) {
                for (Json& l : list) {
                    if (l.contains("pachislop_style3d")) targets.push_back(&l);
                    if (l.contains("children")) walk(l["children"]);
                }
            };
            walk(ctx.doc.root["layers"]);
        }
        for (Json* layer : targets) {
            const Json before = *layer;
            remapStyle3D(*layer);
            TitleOpChange c;
            c.layer = layer->value("id", std::string());
            c.path = "style_3d";
            c.from = before.value("pachislop_style3d", Json());
            c.to = (*layer)["style_3d"];
            c.note = "re-mapped the kept Pachislop 3D style";
            ctx.change(std::move(c));
        }
        ctx.undoName = "Reimport Pachislop 3D";
        return true;
    }

}

bool pachislop::compensateV4CharScale(titleart::TitleArtDoc& doc, const titleart::AssetResolver* assets) {
    return compensateList(doc.root["layers"], assets);
}

bool pachislop::importProjectFile(const TitleCreateRequest& request, TitleCreateResult& out, std::string& code, std::string& error) {
    std::string text;
    std::error_code ec;
    const uintmax_t size = fs::file_size(fs::path(request.sourcePath), ec);
    if (ec) {
        code = "file_not_found";
        error = "cannot read '" + request.sourcePath + "'";
        return false;
    }
    if (size > MAX_FILE_BYTES) {
        code = "pachislop_too_large";
        error = "the project file is " + std::to_string(size >> 20) + " MiB; Title Art imports Pachislop files of up to 256 MiB";
        return false;
    }
    if (!readFile(fs::path(request.sourcePath), text, MAX_FILE_BYTES)) {
        code = "file_not_found";
        error = "cannot read '" + request.sourcePath + "'";
        return false;
    }
    Bundle bundle;
    if (!parseProjectFile(text, bundle, code, error)) return false;

    const fs::path document(request.documentPath);
    const fs::path assetsDir(request.assetsPath);
    TitleImportAssets files(document, assetsDir);
    std::vector<TitleOpFile> packFiles;
    std::vector<TitleImportNote> notes;
    const auto note = [&](const std::string& layer, const std::string& field, const std::string& what, const std::string& action,
                          const std::string& codeName) {
        TitleImportNote n;
        n.layer = layer;
        n.field = field;
        n.what = what;
        n.action = action;
        n.code = codeName;
        notes.push_back(std::move(n));
    };
    for (const std::string& n : bundle.notes) note("", "", n, "note", "");

    std::map<std::string, const Blob*> assetById, fontById;
    for (const Blob& b : bundle.assets) assetById[b.id] = &b;
    for (const Blob& b : bundle.fonts) fontById[b.id] = &b;
    std::map<std::string, Json> assetRefs, fontRefs;

    MapperAssets sink;
    sink.asset = [&](const std::string& id, const std::string& layerPath, const std::string& field) -> Json {
        if (auto done = assetRefs.find(id); done != assetRefs.end()) return done->second;
        auto it = assetById.find(id);
        Json ref;
        if (it == assetById.end()) {
            note(layerPath, field, "asset '" + id + "' is not embedded in the project file", "dropped", "asset_missing");
            return ref;
        }
        const Blob& b = *it->second;
        const std::string mime = lower(b.mime);
        const std::string name = titleSafeFileName(b.name);
        const bool webp = b.data.size() >= 12 && b.data.compare(0, 4, "RIFF") == 0 && b.data.compare(8, 4, "WEBP") == 0;
        if (mime == "image/webp" || webp) {
            note(layerPath, field, "'" + b.name + "' is a WebP image, which Title Art cannot decode: re-save it as PNG and set it on the layer",
                 "dropped", "image_format_unsupported");
            assetRefs[id] = ref;
            return ref;
        }
        std::string bytes = b.data, fileName = name;
        if (mime == "image/gif" || b.data.rfind("GIF8", 0) == 0) {
            int w = 0, h = 0, channels = 0;
            unsigned char* pixels = stbi_load_from_memory(reinterpret_cast<const unsigned char*>(b.data.data()), (int)b.data.size(), &w, &h, &channels, 4);
            if (!pixels) {
                note(layerPath, field, "'" + b.name + "' is a GIF that does not decode", "dropped", "image_format_unsupported");
                assetRefs[id] = ref;
                return ref;
            }
            texsynth::Image image = texsynth::Image::make(w, h, 4, 8);
            std::copy(pixels, pixels + (size_t)w * h * 4, image.data.begin());
            stbi_image_free(pixels);
            std::vector<uint8_t> png;
            texsynth::encodePng(image, png);
            bytes.assign(png.begin(), png.end());
            fileName = fs::path(name).stem().string() + ".png";
            note(layerPath, field, "the GIF '" + b.name + "' is its first frame as a PNG", "approximated", "");
        } else if (mime == "image/png") {
            fileName = withExtension(name, ".png");
        } else if (mime == "image/jpeg") {
            if (lower(fs::path(name).extension().string()) != ".jpeg") fileName = withExtension(name, ".jpg");
        } else if (mime == "image/svg+xml") {
            fileName = withExtension(name, ".svg");
        } else if (mime == "model/gltf-binary") {
            fileName = withExtension(name, ".glb");
        }
        ref = titleart::encodeAssetRef(titleart::TitleAssetRef{files.add(fileName, bytes), ""});
        assetRefs[id] = ref;
        return ref;
    };
    sink.font = [&](const std::string& fontId, const std::string& layerPath) -> Json {
        if (auto done = fontRefs.find(fontId); done != fontRefs.end()) return done->second;
        Json ref;
        if (auto it = fontById.find(fontId); it != fontById.end()) {
            const Blob& b = *it->second;
            ref = titleart::encodeAssetRef(titleart::TitleAssetRef{files.add(withExtension(titleSafeFileName(b.name), fontExtension(b.data)), b.data), ""});
        } else if (const std::string pack = packFontId(fontId); !pack.empty()) {
            const fs::path packDir = titleArtLibraryDir() / "fonts" / pack;
            const std::string file = builtinFontFile(fontId);
            std::string bytes;
            std::error_code missing;
            if (!file.empty() && readFile(packDir / file, bytes, MAX_ASSET_BYTES)) {
                const fs::path target = assetsDir / "TitleArt" / "Fonts" / pack / file;
                if (!fs::exists(target, missing)) {
                    packFiles.push_back({target.string(), bytes});
                    std::string licence;
                    if (readFile(packDir / "OFL.txt", licence, MAX_ASSET_BYTES)) packFiles.push_back({(target.parent_path() / "OFL.txt").string(), licence});
                }
                ref = titleart::encodeAssetRef(titleart::TitleAssetRef{"TitleArt/Fonts/" + pack + "/" + file, ""});
            } else {
                note(layerPath, "font", "font '" + builtinFontFamily(fontId) + "' is not in this editor's font pack: the built-in face stands in (the font picker finds it on Google Fonts)",
                     "approximated", "font_unavailable");
            }
        } else if (!fontId.empty()) {
            note(layerPath, "font", "font '" + fontId + "' is not embedded in the project file: the built-in face stands in", "approximated",
                 "font_unavailable");
        }
        fontRefs[fontId] = ref;
        return ref;
    };

    MapResult mapped = mapProject(bundle.project, sink);
    std::vector<TitleOpFile> staged = files.take();
    if (bundle.version == 4) {
        ImportResolver resolver(assetsDir, staged);
        if (compensateV4CharScale(mapped.doc, &resolver)) {
            note("", "", "Per-character scale now reflows the line, so character offsets and layer positions were adjusted to keep the v4 glyph positions",
                 "note", "");
        }
    }
    for (TitleImportNote& n : mapped.notes) notes.push_back(std::move(n));
    Json lines = Json::array();
    out.notes = Json::array();
    for (const TitleImportNote& n : notes) {
        lines.push_back(n.toLine());
        out.notes.push_back(n.toJson());
    }
    mapped.doc.root["import_notes"] = lines;
    titleart::normalizeTitleArt(mapped.doc, nullptr);
    out.doc = std::move(mapped.doc);
    out.kind = "pachislop";
    out.files = std::move(staged);
    for (TitleOpFile& f : packFiles) out.files.push_back(std::move(f));
    return true;
}

void editor::registerPachislopImport() {
    TitleArtOps::registerCreateImporter(
        "pachislop", [](const std::string& head, const std::string&) { return looksLikeProjectFile(head); }, &importProjectFile);
    TitleArtOps::registerOp("reimport_pachislop_3d", reimport3D);
}
