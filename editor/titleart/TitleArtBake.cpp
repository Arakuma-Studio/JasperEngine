// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleArtBake.h"

#include "asset/AssetFileWrite.h"
#include "texsynth/SynthesisRecipe.h"
#include "titleart/TitleArtIO.h"
#include "titleart/TitleArtRefs.h"
#include "titleart/TitleEffectRegistry.h"
#include "util/Sha256.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <mutex>

using namespace jasperengine;
using namespace jasperengine::editor;
using TJson = titleart::Json;
using titleart::TitleRectI;
namespace fs = std::filesystem;

namespace {

    TJson rectArray(const TitleRectI& r) {
        return TJson::array({r.x, r.y, r.width, r.height});
    }
    TJson rectObject(const TitleRectI& r) {
        TJson o = TJson::object();
        o["x"] = r.x;
        o["y"] = r.y;
        o["w"] = r.width;
        o["h"] = r.height;
        return o;
    }
    bool readRect(const TJson& value, TitleRectI& out) {
        if (value.is_array() && value.size() == 4) {
            for (const TJson& v : value) if (!v.is_number()) return false;
            out = {value[0].get<int>(), value[1].get<int>(), value[2].get<int>(), value[3].get<int>()};
            return true;
        }
        if (value.is_object() && value.contains("x") && value.contains("y") && value.contains("w") && value.contains("h")) {
            for (const char* k : {"x", "y", "w", "h"}) if (!value[k].is_number()) return false;
            out = {value["x"].get<int>(), value["y"].get<int>(), value["w"].get<int>(), value["h"].get<int>()};
            return true;
        }
        return false;
    }

    std::string str(const TJson& object, const char* key) {
        auto it = object.find(key);
        return (it != object.end() && it->is_string()) ? it->get<std::string>() : std::string();
    }

    TJson assetsJson(const std::vector<TitleBakeAsset>& assets) {
        TJson list = TJson::array();
        for (const TitleBakeAsset& a : assets) {
            TJson item = TJson::object();
            item["guid"] = a.guid;
            item["path"] = a.path;
            item["sha256"] = a.sha256;
            list.push_back(std::move(item));
        }
        return list;
    }

    std::vector<TitleBakeAsset> assetsFrom(const TJson& list) {
        std::vector<TitleBakeAsset> out;
        if (!list.is_array()) return out;
        for (const TJson& item : list) {
            if (!item.is_object()) continue;
            out.push_back({str(item, "guid"), str(item, "path"), str(item, "sha256")});
        }
        return out;
    }

    // the key an asset is matched by across bakes: the GUID (renames keep it), else the path
    std::string assetKey(const TitleBakeAsset& asset) {
        return asset.guid.empty() ? "path:" + asset.path : asset.guid;
    }

    bool pngSize(const fs::path& path, int& width, int& height) {
        return texsynth::probeImage(path, width, height);
    }

}

// ==================================================================== names

std::string editor::titleArtOutputFileName(const std::string& documentFileName, const TJson& output) {
    const std::string file = output.is_object() ? str(output, "file") : std::string();
    if (!file.empty()) return file;
    const std::string id = output.is_object() ? str(output, "id") : std::string("main");
    if (id.empty() || id == "main") return documentFileName + ".png";
    return documentFileName + "." + id + ".png";
}

fs::path editor::titleArtOutputPath(const fs::path& document, const TJson& output) {
    return (document.parent_path() / fs::path(titleArtOutputFileName(document.filename().string(), output))).lexically_normal();
}

fs::path editor::titleArtSidecarPath(const fs::path& document) {
    return document.parent_path() / (document.filename().string() + TITLE_ART_SIDECAR_SUFFIX);
}

std::vector<fs::path> editor::titleArtDerivedFiles(const fs::path& document, const titleart::TitleArtDoc* doc) {
    titleart::TitleArtDoc fromDisk;
    if (!doc) {
        std::string text;
        if (assetio::readFile(document, text)) {
            titleart::TitleArtParseResult parsed = titleart::parseTitleArt(text);
            if (parsed.ok) fromDisk = std::move(parsed.doc);
        }
        doc = &fromDisk;
    }
    std::vector<fs::path> out;
    std::error_code ec;
    auto add = [&](const fs::path& path) {
        if (fs::is_regular_file(path, ec) && std::find(out.begin(), out.end(), path) == out.end()) out.push_back(path);
    };
    const TJson& outputs = doc->root.contains("outputs") ? doc->root["outputs"] : TJson::array();
    if (outputs.is_array() && !outputs.empty()) {
        for (const TJson& output : outputs) {
            const fs::path png = titleArtOutputPath(document, output);
            add(png);
            add(fs::path(png.string() + ".meta"));
        }
    } else if (!doc->root.contains("outputs")) {
        // an unreadable document: its default output still moves with it
        const fs::path png = titleArtOutputPath(document, TJson::object({{"id", "main"}}));
        add(png);
        add(fs::path(png.string() + ".meta"));
    }
    add(titleArtSidecarPath(document));
    // S85: the animation data and the flipbook pages (<file>.<animation id>.flip<N>.png)
    add(fs::path(document.string() + ".anim.json"));
    const std::string prefix = document.filename().string() + ".";
    for (const fs::directory_entry& e : fs::directory_iterator(document.parent_path(), ec)) {
        const std::string name = e.path().filename().string();
        if (name.size() > prefix.size() + 9 && name.compare(0, prefix.size(), prefix) == 0 && name.find(".flip", prefix.size()) != std::string::npos &&
            (name.size() > 4 && (name.compare(name.size() - 4, 4, ".png") == 0 || name.compare(name.size() - 9, 9, ".png.meta") == 0))) {
            add(e.path());
        }
    }
    return out;
}

// ==================================================================== pixels

void editor::bleedAlpha(texsynth::Image& image, int reachPx) {
    if (image.empty() || image.channels != 4 || reachPx <= 0) return;
    const int w = image.width, h = image.height;
    const bool wide = image.bitDepth == 16;
    auto alphaOf = [&](int x, int y) -> uint32_t {
        const uint8_t* p = image.pixel(x, y);
        return wide ? reinterpret_cast<const uint16_t*>(p)[3] : p[3];
    };
    // Danielsson's vector propagation: each pixel keeps the offset to its nearest source (alpha > 0)
    constexpr int far = std::numeric_limits<int>::max() / 4;
    std::vector<int> dx(static_cast<size_t>(w) * h, far), dy(static_cast<size_t>(w) * h, far);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            if (alphaOf(x, y) > 0) dx[static_cast<size_t>(y) * w + x] = dy[static_cast<size_t>(y) * w + x] = 0;
        }
    }
    auto dist2 = [&](size_t i) -> int64_t {
        if (dx[i] >= far) return std::numeric_limits<int64_t>::max();
        return static_cast<int64_t>(dx[i]) * dx[i] + static_cast<int64_t>(dy[i]) * dy[i];
    };
    auto relax = [&](int x, int y, int nx, int ny) {
        if (nx < 0 || ny < 0 || nx >= w || ny >= h) return;
        const size_t i = static_cast<size_t>(y) * w + x, n = static_cast<size_t>(ny) * w + nx;
        if (dx[n] >= far) return;
        const int cx = dx[n] + (x - nx), cy = dy[n] + (y - ny);
        const int64_t d = static_cast<int64_t>(cx) * cx + static_cast<int64_t>(cy) * cy;
        if (d < dist2(i)) {
            dx[i] = cx;
            dy[i] = cy;
        }
    };
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) { relax(x, y, x - 1, y); relax(x, y, x, y - 1); relax(x, y, x - 1, y - 1); relax(x, y, x + 1, y - 1); }
        for (int x = w - 1; x >= 0; x--) relax(x, y, x + 1, y);
    }
    for (int y = h - 1; y >= 0; y--) {
        for (int x = w - 1; x >= 0; x--) { relax(x, y, x + 1, y); relax(x, y, x, y + 1); relax(x, y, x + 1, y + 1); relax(x, y, x - 1, y + 1); }
        for (int x = 0; x < w; x++) relax(x, y, x - 1, y);
    }
    const int64_t reach2 = static_cast<int64_t>(reachPx) * reachPx;
    const size_t pixelBytes = image.bytesPerPixel();
    const size_t colourBytes = pixelBytes / 4 * 3;
    std::vector<uint8_t> source = image.data;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            const size_t i = static_cast<size_t>(y) * w + x;
            if (alphaOf(x, y) > 0) continue;
            uint8_t* p = image.pixel(x, y);
            if (dist2(i) <= reach2) {
                const int sx = x - dx[i], sy = y - dy[i];
                const uint8_t* s = source.data() + (static_cast<size_t>(sy) * w + sx) * pixelBytes;
                std::copy(s, s + colourBytes, p);
            } else {
                std::fill(p, p + colourBytes, uint8_t(0));
            }
        }
    }
}

TitleRectI editor::titleArtBakeFrame(int canvasWidth, int canvasHeight, const std::vector<TitleFrameInput>& outputs) {
    const TitleRectI canvas{0, 0, canvasWidth, canvasHeight};
    int roundTo = 1;
    bool any = false;
    double minX = 0, minY = 0, maxX = 0, maxY = 0;
    for (const TitleFrameInput& o : outputs) {
        roundTo = std::max(roundTo, std::max(1, o.roundTo));
        if (!o.trim) return canvas;
        if (o.contentBounds.empty()) continue;
        const double pad = std::max(0.0, o.paddingCanvasPx);
        const double x0 = o.contentBounds.x - pad, y0 = o.contentBounds.y - pad;
        const double x1 = o.contentBounds.x + o.contentBounds.width + pad, y1 = o.contentBounds.y + o.contentBounds.height + pad;
        if (!any) {
            minX = x0; minY = y0; maxX = x1; maxY = y1;
            any = true;
        } else {
            minX = std::min(minX, x0); minY = std::min(minY, y0);
            maxX = std::max(maxX, x1); maxY = std::max(maxY, y1);
        }
    }
    if (!any) return {};
    int x0 = std::max(0, static_cast<int>(std::floor(minX)));
    int y0 = std::max(0, static_cast<int>(std::floor(minY)));
    int x1 = std::min(canvasWidth, static_cast<int>(std::ceil(maxX)));
    int y1 = std::min(canvasHeight, static_cast<int>(std::ceil(maxY)));
    if (x1 <= x0 || y1 <= y0) return {};
    auto roundSide = [&](int& lo, int& hi, int limit) {
        const int side = hi - lo;
        const int wanted = ((side + roundTo - 1) / roundTo) * roundTo;
        int grow = wanted - side;
        const int right = std::min(grow, limit - hi);
        hi += right;
        grow -= right;
        lo = std::max(0, lo - grow);
    };
    roundSide(x0, x1, canvasWidth);
    roundSide(y0, y1, canvasHeight);
    return {x0, y0, x1 - x0, y1 - y0};
}

void editor::titleArtOutputSize(const TitleRectI& frame, double scale, int roundTo, int& width, int& height) {
    roundTo = std::max(1, roundTo);
    auto side = [&](int canvasSide) {
        const int px = std::max(1, static_cast<int>(std::lround(canvasSide * scale)));
        return ((px + roundTo - 1) / roundTo) * roundTo;
    };
    width = side(frame.width);
    height = side(frame.height);
}

// ==================================================================== hashes

std::string editor::titleArtSourceHash(const titleart::TitleArtDoc& doc) {
    return sha256Hex(titleart::writeTitleArt(doc));
}

namespace {

    struct HashEntry {
        uintmax_t size = 0;
        int64_t time = 0;
        std::string hash;
    };
    std::mutex hashLock;
    std::map<std::string, HashEntry>& hashCache() {
        static std::map<std::string, HashEntry> cache;
        return cache;
    }

}

std::string editor::titleArtFileHash(const fs::path& path) {
    std::error_code ec;
    const uintmax_t size = fs::file_size(path, ec);
    if (ec) return {};
    const int64_t time = static_cast<int64_t>(fs::last_write_time(path, ec).time_since_epoch().count());
    if (ec) return {};
    const std::string key = path.lexically_normal().generic_string();
    {
        std::lock_guard<std::mutex> guard(hashLock);
        auto it = hashCache().find(key);
        if (it != hashCache().end() && it->second.size == size && it->second.time == time) return it->second.hash;
    }
    std::string hex;
    if (!sha256File(path, hex)) return {};
    std::lock_guard<std::mutex> guard(hashLock);
    hashCache()[key] = {size, time, hex};
    return hex;
}

void editor::titleArtHashCacheLoad(const fs::path& file) {
    std::string text;
    if (!assetio::readFile(file, text)) return;
    const TJson json = TJson::parse(text, nullptr, false);
    if (!json.is_object() || json.value("version", 0) != 1 || !json.contains("files") || !json["files"].is_object()) return;
    std::lock_guard<std::mutex> guard(hashLock);
    for (auto it = json["files"].begin(); it != json["files"].end(); ++it) {
        const TJson& e = it.value();
        if (!e.is_array() || e.size() != 3 || !e[0].is_number() || !e[1].is_number() || !e[2].is_string()) continue;
        hashCache()[it.key()] = {e[1].get<uintmax_t>(), e[0].get<int64_t>(), e[2].get<std::string>()};
    }
}

void editor::titleArtHashCacheSave(const fs::path& file) {
    TJson files = TJson::object();
    {
        std::lock_guard<std::mutex> guard(hashLock);
        for (const auto& [key, e] : hashCache()) files[key] = TJson::array({e.time, e.size, e.hash});
    }
    TJson json = TJson::object();
    json["version"] = 1;
    json["files"] = std::move(files);
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    assetio::writeFile(file, json.dump() + "\n", assetio::Durability::Cache);
}

std::vector<TitleBakeAsset> editor::titleArtBakeAssets(const titleart::TitleArtDoc& doc, const titleart::AssetResolver& assets) {
    std::map<std::string, TitleBakeAsset> unique;
    titleart::visitTitleArtRefs(doc.root, [&](const titleart::TitleAssetRef& ref, const std::string&, titleart::TitleAssetKind kind) {
        // a marker's sound is no input of the pixels: changing it leaves the bake fresh
        if (ref.empty() || kind == titleart::TitleAssetKind::Sound) return true;
        const titleart::ResolvedTitleAsset resolved = assets.resolve(ref, kind);
        TitleBakeAsset asset;
        asset.guid = resolved.found ? resolved.guid : ref.guid;
        asset.path = resolved.found ? resolved.assetPath : ref.path;
        if (resolved.found) asset.sha256 = titleArtFileHash(fs::path(resolved.path));
        unique.emplace(assetKey(asset), std::move(asset));
        return true;
    });
    std::vector<TitleBakeAsset> out;
    for (auto& entry : unique) out.push_back(std::move(entry.second));
    std::sort(out.begin(), out.end(), [](const TitleBakeAsset& a, const TitleBakeAsset& b) {
        return a.path != b.path ? a.path < b.path : a.guid < b.guid;
    });
    return out;
}

// ==================================================================== recipe

TJson TitleBakeRecipe::toJson() const {
    TJson j = TJson::object();
    j["format"] = TITLE_ART_RECIPE_FORMAT;
    j["version"] = TITLE_ART_RECIPE_VERSION;
    TJson source = TJson::object();
    source["guid"] = sourceGuid;
    source["path"] = sourcePath;
    j["source"] = std::move(source);
    j["source_hash"] = sourceHash;
    j["assets"] = assetsJson(assets);
    j["bake_version"] = bakeVersion;
    j["output"] = output;
    j["frame"] = rectArray(frame);
    j["scale"] = scale;
    j["bit_depth"] = bitDepth;
    j["path_used"] = pathUsed;
    j["backend"] = backend;
    j["gpu"] = gpu;
    j["editor_version"] = editorVersion;
    for (auto it = extra.begin(); it != extra.end(); ++it) {
        if (!j.contains(it.key())) j[it.key()] = it.value();
    }
    return j;
}

bool TitleBakeRecipe::fromJson(const TJson& json, TitleBakeRecipe& out, std::string& error) {
    if (!json.is_object() || str(json, "format") != TITLE_ART_RECIPE_FORMAT) {
        error = "not a Title Art bake recipe";
        return false;
    }
    const int version = json.value("version", 0);
    if (version > TITLE_ART_RECIPE_VERSION) {
        error = "the recipe is version " + std::to_string(version) + ", newer than this editor reads (" +
                std::to_string(TITLE_ART_RECIPE_VERSION) + ")";
        return false;
    }
    out = TitleBakeRecipe();
    const TJson source = json.contains("source") && json["source"].is_object() ? json["source"] : TJson::object();
    out.sourceGuid = str(source, "guid");
    out.sourcePath = str(source, "path");
    out.sourceHash = str(json, "source_hash");
    out.assets = assetsFrom(json.contains("assets") ? json["assets"] : TJson());
    out.bakeVersion = json.contains("bake_version") && json["bake_version"].is_number_unsigned() ? json["bake_version"].get<uint32_t>() :
                      static_cast<uint32_t>(json.value("bake_version", 0));
    out.output = str(json, "output");
    if (json.contains("frame")) readRect(json["frame"], out.frame);
    out.scale = json.value("scale", 1.0);
    out.bitDepth = json.value("bit_depth", 8);
    out.pathUsed = str(json, "path_used");
    out.backend = str(json, "backend");
    out.gpu = str(json, "gpu");
    out.editorVersion = str(json, "editor_version");
    static const char* known[] = {"format", "version", "source", "source_hash", "assets", "bake_version", "output", "frame",
                                  "scale", "bit_depth", "path_used", "backend", "gpu", "editor_version"};
    for (auto it = json.begin(); it != json.end(); ++it) {
        if (std::find_if(std::begin(known), std::end(known), [&](const char* k) { return it.key() == k; }) == std::end(known)) {
            out.extra[it.key()] = it.value();
        }
    }
    if (out.sourceHash.empty() || out.output.empty()) {
        error = "the recipe has no source_hash or output";
        return false;
    }
    return true;
}

std::optional<TitleBakeRecipe> editor::readTitleBakeRecipe(const fs::path& png, std::string* error) {
    std::string readError;
    const std::optional<std::string> text = texsynth::readRecipeTextHead(png, &readError, TITLE_ART_RECIPE_KEYWORD);
    if (!text) {
        if (error) *error = readError;
        return std::nullopt;
    }
    TJson json;
    try {
        json = TJson::parse(*text);
    } catch (const std::exception& e) {
        if (error) *error = std::string("the recipe is not JSON: ") + e.what();
        return std::nullopt;
    }
    TitleBakeRecipe recipe;
    std::string parseError;
    if (!TitleBakeRecipe::fromJson(json, recipe, parseError)) {
        if (error) *error = parseError;
        return std::nullopt;
    }
    return recipe;
}

bool editor::insertTitleBakeRecipe(const std::vector<uint8_t>& png, const TitleBakeRecipe& recipe, std::vector<uint8_t>& out, std::string& error) {
    return texsynth::insertRecipeChunk(png, recipe.toJson().dump(), out, error, TITLE_ART_RECIPE_KEYWORD);
}

// ==================================================================== sidecar

TJson TitleBakeInfo::toJson() const {
    TJson j = TJson::object();
    j["format"] = TITLE_ART_BAKEINFO_FORMAT;
    j["version"] = TITLE_ART_BAKEINFO_VERSION;
    j["frame"] = rectObject(frame);
    j["origin"] = TJson::array({originX, originY});
    TJson list = TJson::array();
    for (const TitleBakeInfoOutput& o : outputs) {
        TJson item = TJson::object();
        item["id"] = o.id;
        item["file"] = o.file;
        item["content"] = o.content;
        item["size"] = TJson::array({o.width, o.height});
        list.push_back(std::move(item));
    }
    j["outputs"] = std::move(list);
    j["alpha_bounds"] = rectObject(alphaBounds);
    j["source_hash"] = sourceHash;
    j["bake_version"] = bakeVersion;
    for (auto it = extra.begin(); it != extra.end(); ++it) {
        if (!j.contains(it.key())) j[it.key()] = it.value();
    }
    return j;
}

std::string TitleBakeInfo::toText() const {
    return titleart::writeTitleArtJson(toJson()) + "\n";
}

bool TitleBakeInfo::parse(const std::string& text, TitleBakeInfo& out, std::string& error) {
    TJson json;
    try {
        json = TJson::parse(text);
    } catch (const std::exception& e) {
        error = std::string("not JSON: ") + e.what();
        return false;
    }
    if (!json.is_object() || str(json, "format") != TITLE_ART_BAKEINFO_FORMAT) {
        error = "not a Title Art bake sidecar (format " + std::string(TITLE_ART_BAKEINFO_FORMAT) + ")";
        return false;
    }
    const int version = json.value("version", 0);
    if (version > TITLE_ART_BAKEINFO_VERSION) {
        error = "version_newer: the bake sidecar is version " + std::to_string(version) + ", written by a newer editor; this build reads version " +
                std::to_string(TITLE_ART_BAKEINFO_VERSION);
        return false;
    }
    out = TitleBakeInfo();
    if (json.contains("frame")) readRect(json["frame"], out.frame);
    if (json.contains("origin") && json["origin"].is_array() && json["origin"].size() == 2) {
        out.originX = json["origin"][0].is_number() ? json["origin"][0].get<double>() : 0.0;
        out.originY = json["origin"][1].is_number() ? json["origin"][1].get<double>() : 0.0;
    }
    if (json.contains("outputs") && json["outputs"].is_array()) {
        for (const TJson& item : json["outputs"]) {
            if (!item.is_object()) continue;
            TitleBakeInfoOutput o;
            o.id = str(item, "id");
            o.file = str(item, "file");
            o.content = str(item, "content");
            if (item.contains("size") && item["size"].is_array() && item["size"].size() == 2) {
                o.width = item["size"][0].is_number() ? item["size"][0].get<int>() : 0;
                o.height = item["size"][1].is_number() ? item["size"][1].get<int>() : 0;
            }
            out.outputs.push_back(std::move(o));
        }
    }
    if (json.contains("alpha_bounds")) readRect(json["alpha_bounds"], out.alphaBounds);
    out.sourceHash = str(json, "source_hash");
    out.bakeVersion = static_cast<uint32_t>(json.value("bake_version", 0));
    static const char* known[] = {"format", "version", "frame", "origin", "outputs", "alpha_bounds", "source_hash", "bake_version"};
    for (auto it = json.begin(); it != json.end(); ++it) {
        if (std::find_if(std::begin(known), std::end(known), [&](const char* k) { return it.key() == k; }) == std::end(known)) {
            out.extra[it.key()] = it.value();
        }
    }
    return true;
}

// ==================================================================== status

const char* editor::titleOutputStatusName(TitleOutputStatus status) {
    switch (status) {
        case TitleOutputStatus::Fresh: return "fresh";
        case TitleOutputStatus::Stale: return "stale";
        case TitleOutputStatus::Missing: return "missing";
        case TitleOutputStatus::Invalid: return "invalid";
    }
    return "missing";
}

const char* editor::titleOutputStatusLabel(TitleOutputStatus status) {
    switch (status) {
        case TitleOutputStatus::Fresh: return "Fresh";
        case TitleOutputStatus::Stale: return "Stale";
        case TitleOutputStatus::Missing: return "Missing";
        case TitleOutputStatus::Invalid: return "Invalid";
    }
    return "Missing";
}

TJson TitleOutputState::toJson(const fs::path& projectPath) const {
    TJson j = TJson::object();
    j["id"] = id;
    const fs::path relative = file.lexically_relative(projectPath);
    j["file"] = relative.empty() ? file.generic_string() : relative.generic_string();
    j["content"] = content;
    j["status"] = titleOutputStatusName(status);
    if (!changed.empty()) j["changed"] = changed;
    if (!reason.empty()) j["reason"] = reason;
    if (width > 0) j["size"] = TJson::array({width, height});
    return j;
}

bool TitleBakeStatus::allFresh() const {
    for (const TitleOutputState& o : outputs) if (o.status != TitleOutputStatus::Fresh) return false;
    return true;
}

bool TitleBakeStatus::anyBaked() const {
    for (const TitleOutputState& o : outputs) if (o.status != TitleOutputStatus::Missing) return true;
    return false;
}

std::vector<std::string> TitleBakeStatus::notFresh() const {
    std::vector<std::string> out;
    for (const TitleOutputState& o : outputs) if (o.status != TitleOutputStatus::Fresh) out.push_back(o.id);
    return out;
}

TitleBakeStatus editor::titleArtBakeStatus(const fs::path& document, const titleart::TitleArtDoc& doc, const titleart::AssetResolver& assets) {
    TitleBakeStatus status;
    status.sourceHash = titleArtSourceHash(doc);
    status.bakeVersion = titleart::titleArtBakeVersion(doc);
    status.assets = titleArtBakeAssets(doc, assets);
    std::error_code ec;
    status.sidecarPresent = fs::is_regular_file(titleArtSidecarPath(document), ec);
    std::map<std::string, std::string> current;
    for (const TitleBakeAsset& a : status.assets) current[assetKey(a)] = a.sha256;

    const TJson& outputs = doc.root.contains("outputs") ? doc.root["outputs"] : TJson::array();
    for (const TJson& output : outputs) {
        TitleOutputState state;
        state.id = str(output, "id");
        state.content = str(output, "content");
        state.fileName = titleArtOutputFileName(document.filename().string(), output);
        state.file = titleArtOutputPath(document, output);
        if (!fs::is_regular_file(state.file, ec)) {
            state.status = TitleOutputStatus::Missing;
            state.reason = "not baked";
            status.outputs.push_back(std::move(state));
            continue;
        }
        pngSize(state.file, state.width, state.height);
        std::string error;
        state.recipe = readTitleBakeRecipe(state.file, &error);
        if (!state.recipe) {
            state.status = TitleOutputStatus::Invalid;
            state.reason = error.empty() ? "the PNG has no jasper.titleart recipe" : "unreadable recipe: " + error;
            status.outputs.push_back(std::move(state));
            continue;
        }
        const TitleBakeRecipe& recipe = *state.recipe;
        if (recipe.sourceGuid != doc.guid() || recipe.output != state.id) {
            state.status = TitleOutputStatus::Invalid;
            state.reason = "the PNG was baked from " + (recipe.sourcePath.empty() ? recipe.sourceGuid : recipe.sourcePath) + " output '" +
                           recipe.output + "', not this document's output '" + state.id + "'";
            status.outputs.push_back(std::move(state));
            continue;
        }
        if (recipe.sourceHash != status.sourceHash) state.changed.push_back("document");
        if (recipe.bakeVersion != status.bakeVersion) state.changed.push_back("bake_version");
        std::map<std::string, std::string> baked;
        for (const TitleBakeAsset& a : recipe.assets) baked[assetKey(a)] = a.sha256;
        for (const TitleBakeAsset& a : status.assets) {
            auto it = baked.find(assetKey(a));
            if (it == baked.end()) state.changed.push_back("asset added " + a.path);
            else if (it->second != a.sha256) state.changed.push_back(a.sha256.empty() ? "asset missing " + a.path : "asset changed " + a.path);
        }
        for (const TitleBakeAsset& a : recipe.assets) {
            if (!current.count(assetKey(a))) state.changed.push_back("asset removed " + a.path);
        }
        if (!status.sidecarPresent) state.changed.push_back("sidecar missing");
        if (state.changed.empty()) {
            state.status = TitleOutputStatus::Fresh;
        } else {
            state.status = TitleOutputStatus::Stale;
            std::string joined;
            for (const std::string& c : state.changed) joined += (joined.empty() ? "" : ", ") + c;
            state.reason = "stale: " + joined;
        }
        status.outputs.push_back(std::move(state));
    }
    return status;
}
