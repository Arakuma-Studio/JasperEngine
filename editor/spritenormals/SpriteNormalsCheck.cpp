// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SpriteNormalsCheck.h"
#include "util/FileUtils.h"

#include "SpriteNormalsPaint.h"
#include "SpriteNormalsPreview.h"
#include "SpriteNormalsRecipe.h"

#include "sprite/SpriteSheetBuilder.h"
#include "texsynth/SynthImage.h"
#include "texsynth/SynthesisRecipe.h"

#include "json.hpp"
#include "yaml-cpp/yaml.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <stdexcept>
#include <map>
#include <optional>
#include <set>
#include <string>

namespace fs = std::filesystem;
using nlohmann::json;

namespace jasperengine::editor::spritenormals {

    namespace {

        using Finding = ProjectChecks::Finding;

        // ---- the cache (.jasperengine/normals-check-cache.json)

        constexpr int kCacheVersion = 3;   // 3: island_side (normals-before-hires)

        struct Cache {
            fs::path file;
            json pngs = json::object();      // rel -> {mtime, size, recipe, recipe_error?, width, height, analysis?}
            json sources = json::object();   // rel -> {mtime, size, sha1}
            json index = json::object();     // source rel -> [generated PNG rels]
            bool dirty = false;
        };

        fs::path cacheFile(const fs::path& root) {
            return root / ".jasperengine" / "normals-check-cache.json";
        }

        void loadCache(Cache& cache, const fs::path& root) {
            std::error_code ec;
            if (!fs::is_directory(root / ".jasperengine", ec)) return;
            cache.file = cacheFile(root);
            std::string text;
            if (!ProjectChecks::readFile(cache.file, text)) return;
            const json j = json::parse(text, nullptr, false);
            // version 2 (Phase 5): PNG entries carry paint / paint_error; a version 1 cache is read again
            if (!j.is_object() || !j.contains("version") || j["version"] != kCacheVersion) return;
            if (j.contains("pngs") && j["pngs"].is_object()) cache.pngs = j["pngs"];
            if (j.contains("sources") && j["sources"].is_object()) cache.sources = j["sources"];
            if (j.contains("index") && j["index"].is_object()) cache.index = j["index"];
        }

        void saveCache(const Cache& cache) {
            if (cache.file.empty() || !cache.dirty) return;
            const json j = {{"version", kCacheVersion}, {"pngs", cache.pngs}, {"sources", cache.sources}, {"index", cache.index}};
            fs::path temporary = cache.file;
            temporary += ".partial";
            // (in a write scope: beneath its root; FileUtils writes aside and renames)
            (void)temporary;
            FileUtils::writeFileReplacing(cache.file, texsynth::jsonTextLossy(j));
        }

        // JSON keys must be UTF-8: other names are not cached (read again each run)
        bool cacheable(const std::string& key) {
            return texsynth::isValidUtf8(key);
        }

        bool stamp(const fs::path& path, int64_t& time, std::uintmax_t& size) {
            std::error_code ec;
            const auto t = fs::last_write_time(path, ec);
            if (ec) return false;
            size = fs::file_size(path, ec);
            if (ec) return false;
            time = static_cast<int64_t>(t.time_since_epoch().count());
            return true;
        }

        bool sameStamp(const json& entry, int64_t time, std::uintmax_t size) {
            return entry.is_object() && entry.contains("mtime") && entry["mtime"].is_number_integer() && entry["mtime"].get<int64_t>() == time &&
                   entry.contains("size") && entry["size"].is_number_unsigned() && entry["size"].get<std::uintmax_t>() == size;
        }

        std::string lowerExt(const fs::path& path) {
            std::string ext = jasperengine::editor::FileUtils::pathToUtf8(path.extension());
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return ext;
        }

        std::string name(const fs::path& path) {
            return jasperengine::editor::FileUtils::pathToUtf8(path.filename());
        }

        // ---- what a PNG is (cached by mtime and size)

        struct PngInfo {
            bool exists = false;
            std::optional<json> recipe;      // a valid recipe
            std::string recipeError;         // a garbled one
            int width = 0;
            int height = 0;
            std::string paintError;          // the paint chunk can't be read
            int paintWidth = 0;              // its size (a readable one)
            int paintHeight = 0;
            double islandSide = -1.0;        // a version 1 recipe's map over 128 px: medianIslandSide of its pixels
        };

        struct Checker {
            fs::path root;
            Cache cache;
            NormalsCheck result;

            std::string rel(const fs::path& path) const { return ProjectChecks::projectRelative(root, path); }

            // the cache entry of a PNG, refreshed when the file changed: only the chunks before
            // IDAT are read (the recipe) and the header (the size)
            json* pngEntry(const fs::path& png, json& scratch) {
                int64_t time = 0;
                std::uintmax_t size = 0;
                if (!stamp(png, time, size)) return nullptr;
                const std::string key = rel(png);
                json* entry = cacheable(key) ? &cache.pngs[key] : &scratch;
                if (sameStamp(*entry, time, size)) return entry;
                json fresh = {{"mtime", time}, {"size", size}, {"recipe", nullptr}};
                std::string error;
                RecipeReadError origin = RecipeReadError::None;
                const std::optional<json> recipe = readRecipeHead(png, &error, &origin);
                std::string invalid;
                if (recipe && validateRecipe(*recipe, invalid)) fresh["recipe"] = *recipe;
                else if (recipe) fresh["recipe_error"] = invalid;
                // only a damaged jasper.spritenormals chunk is a garbled recipe; a file that is not a
                // PNG, can't be opened or has a broken chunk list has no recipe to report (decided on
                // the error's origin, never its text: a path may hold the keyword)
                else if (origin == RecipeReadError::Chunk) fresh["recipe_error"] = error;
                int w = 0, h = 0;
                texsynth::probeImage(png, w, h);
                fresh["width"] = w;
                fresh["height"] = h;
                // made before high-resolution support and over 128 px: how large its islands (or
                // frames' islands) are, from its alpha (a sheet of 32 px frames is pixel art)
                if (fresh["recipe"].is_object() && fresh["recipe"].value("version", 1) < 2 && std::min(w, h) > kHiresSide) {
                    texsynth::Image image;
                    std::string why;
                    if (texsynth::loadImage(png, image, why)) {
                        const json& r = fresh["recipe"];
                        std::vector<Cell> cells;
                        if (r.contains("frames") && r["frames"].contains("cells")) {
                            for (const json& c : r["frames"]["cells"]) cells.push_back({c[0].get<int>(), c[1].get<int>(), c[2].get<int>(), c[3].get<int>()});
                        }
                        float threshold = 0.03f;
                        if (r.contains("settings") && r["settings"].contains("alpha_threshold")) threshold = r["settings"]["alpha_threshold"].get<float>();
                        fresh["island_side"] = medianIslandSide(texsynth::toRgba8(image), cells, threshold);
                    }
                }
                // the paint layer (Phase 5): its head first, then (only when there is one) the
                // whole chunk decoded, so a damaged one is found
                std::string paintError;
                if (recipe && (readPaintSummary(png, &paintError) || !paintError.empty())) {
                    const std::optional<PaintOverlay> paint = readPaint(png, &paintError);
                    if (paint) fresh["paint"] = {{"painted", paint->paintedCount()}, {"width", paint->width}, {"height", paint->height}};
                    else if (!paintError.empty()) fresh["paint_error"] = paintError;
                }
                *entry = fresh;
                if (entry != &scratch) cache.dirty = true;
                return entry;
            }

            PngInfo png(const fs::path& path) {
                PngInfo info;
                json scratch;
                const json* entry = pngEntry(path, scratch);
                if (!entry) return info;
                info.exists = true;
                if ((*entry)["recipe"].is_object()) info.recipe = (*entry)["recipe"];
                if (entry->contains("recipe_error")) info.recipeError = (*entry)["recipe_error"].get<std::string>();
                info.width = entry->value("width", 0);
                info.height = entry->value("height", 0);
                if (entry->contains("paint_error") && (*entry)["paint_error"].is_string()) info.paintError = (*entry)["paint_error"].get<std::string>();
                if (entry->contains("island_side") && (*entry)["island_side"].is_number()) info.islandSide = (*entry)["island_side"].get<double>();
                if (entry->contains("paint") && (*entry)["paint"].is_object()) {
                    info.paintWidth = (*entry)["paint"].value("width", 0);
                    info.paintHeight = (*entry)["paint"].value("height", 0);
                }
                return info;
            }

            // a source's SHA-1 (cached by mtime and size); "" when it is gone
            std::string sha1(const fs::path& source) {
                int64_t time = 0;
                std::uintmax_t size = 0;
                if (!stamp(source, time, size)) return std::string();
                const std::string key = rel(source);
                if (cacheable(key) && sameStamp(cache.sources[key], time, size) && cache.sources[key].contains("sha1")) {
                    return cache.sources[key]["sha1"].get<std::string>();
                }
                std::string content;
                if (!ProjectChecks::readFile(source, content)) return std::string();
                const std::string hash = texsynth::sha1Hex(reinterpret_cast<const uint8_t*>(content.data()), content.size());
                if (cacheable(key)) {
                    cache.sources[key] = {{"mtime", time}, {"size", size}, {"sha1", hash}};
                    cache.dirty = true;
                }
                return hash;
            }

            // the file a recipe's source.path names: project-relative, or (written outside a
            // project) relative to the PNG's directory
            fs::path sourceOf(const json& recipe, const fs::path& png) const {
                const fs::path recorded = fs::u8path(recipe["source"]["path"].get<std::string>());
                if (recorded.is_absolute()) return recorded.lexically_normal();
                const fs::path fromProject = (root / recorded).lexically_normal();
                const fs::path fromPng = (png.parent_path() / recorded).lexically_normal();
                std::error_code ec;
                if (!fs::exists(fromProject, ec) && fs::exists(fromPng, ec)) return fromPng;
                return fromProject;
            }

            // ---- generated maps

            void checkGenerated(const fs::path& path, const PngInfo& info, bool reportAllRoles) {
                if (!info.recipe && info.recipeError.empty()) return;
                const std::string at = rel(path);
                result.recipes++;
                if (!info.recipe) {
                    Finding f{"normals", at, 0, 0, "warning", "its sprite normal map recipe (jasper.spritenormals chunk) is garbled: " + info.recipeError +
                                                                  "; its source can't be checked nor the map regenerated from it",
                              "normals-recipe-invalid", "generate it again (a fresh recipe is written)"};
                    result.findings.push_back(f);
                    return;
                }
                const json& recipe = *info.recipe;
                const std::string role = recipe.value("role", std::string("normal"));
                // an atlas or height map: its normal map reports the set (unless that one is gone)
                if (!reportAllRoles && role != "normal" && recipe.contains("outputs") && recipe["outputs"].contains("normal") &&
                    recipe["outputs"]["normal"].is_string()) {
                    const fs::path normal = (root / fs::u8path(recipe["outputs"]["normal"].get<std::string>())).lexically_normal();
                    std::error_code ec;
                    if (normal != path.lexically_normal() && fs::is_regular_file(normal, ec)) return;
                }
                // the paint layer: the map works without it, the next generation would drop it
                const std::string paintFix = "regenerate with paint_mode: clear, or repaint";
                if (!info.paintError.empty()) {
                    result.findings.push_back({"normals", at, 0, 0, "warning",
                                               "its paint layer (jasper.spritenormals.paint chunk) can't be read: " + info.paintError +
                                                   "; the map still works, regenerating drops the paint",
                                               "normals-paint-unreadable", paintFix});
                } else if (info.paintWidth > 0 && (info.paintWidth != info.width || info.paintHeight != info.height)) {
                    result.findings.push_back({"normals", at, 0, 0, "warning",
                                               "its paint layer is " + std::to_string(info.paintWidth) + "x" + std::to_string(info.paintHeight) + " but the map is " +
                                                   std::to_string(info.width) + "x" + std::to_string(info.height) + ": regenerating refuses it (paint_size_mismatch)",
                                               "normals-paint-size", paintFix});
                }
                // made before high-resolution support (recipe version 1) with islands above 128 px
                // (the median over its frames of the largest island's shorter side, the generator's
                // gate): it keeps the old rules when regenerated (rounded creases, fans, unscaled relief)
                const bool hires = info.islandSide > kHiresSide;
                if (role == "normal" && hires && recipe.value("version", 1) < 2) {
                    result.findings.push_back({"normals", at, 0, 0, "warning",
                                               "made before high-resolution support (recipe version 1), its sprite " +
                                                   std::to_string(static_cast<int>(info.islandSide)) +
                                                   " px across: regenerating keeps its old look (creases down rounded shapes, fans, little relief)",
                                               "normals-before-hires",
                                               "regenerate it with settings version 2: generate_sprite_normals with get_asset_info's `regenerate` "
                                               "arguments and params.version 2, `generate-sprite-normals --settings-version 2`, or the window's "
                                               "Use the current generator"});
                }
                const fs::path source = sourceOf(recipe, path);
                const std::string sourceRel = rel(source);
                const std::string hash = sha1(source);
                if (hash.empty()) {
                    result.findings.push_back({"normals", at, 0, 0, "warning", "made from " + sourceRel + ", which no longer exists", "normals-source-missing",
                                               "restore " + sourceRel + ", or generate from another source; the normal map itself still works"});
                } else if (hash != recipe["source"].value("sha1", std::string())) {
                    result.findings.push_back({"normals", at, 0, 0, "warning",
                                               "made from " + sourceRel + ", which changed since: the normal map no longer matches its sprite",
                                               "normals-source-changed",
                                               "regenerate it: right-click it in the Project window > Regenerate Normal Map, generate_sprite_normals with the "
                                               "`regenerate` arguments get_asset_info gives, or `generate-sprite-normals` with the same settings"});
                }
            }

            // rebuilt from the cached recipes (whole-project runs)
            void rebuildIndex() {
                json index = json::object();
                for (auto it = cache.pngs.begin(); it != cache.pngs.end(); ++it) {
                    const json& recipe = it.value().is_object() && it.value().contains("recipe") ? it.value()["recipe"] : json();
                    if (!recipe.is_object()) continue;
                    const std::string source = rel(sourceOf(recipe, root / fs::u8path(it.key())));
                    if (!cacheable(source)) continue;
                    index[source].push_back(it.key());
                }
                if (index != cache.index) {
                    cache.index = index;
                    cache.dirty = true;
                }
            }

            // ---- normal maps in use (scene and prefab YAML)

            struct Analysis {
                bool unitOk = true;
                double blueLowPct = 0.0;
                std::string green = "unknown";   // opengl, directx, unknown
            };

            // the pixels of a normal map without a recipe, decoded once per version (cached)
            std::optional<Analysis> analyse(const fs::path& path) {
                json scratch;
                json* entry = pngEntry(path, scratch);
                if (!entry) return std::nullopt;
                if (!entry->contains("analysis")) {
                    texsynth::Image loaded;
                    std::string error;
                    if (!texsynth::loadImage(path, loaded, error)) return std::nullopt;
                    const texsynth::Image img = texsynth::toRgba8(loaded);
                    const int w = img.width, h = img.height;
                    auto px = [&](int x, int y) { return img.data.data() + (static_cast<size_t>(y) * w + x) * 4; };
                    auto opaque = [&](int x, int y) { return x >= 0 && y >= 0 && x < w && y < h && px(x, y)[3] > 7; };
                    // a rim is an opaque pixel next to a transparent one inside the image: the image's
                    // border is no silhouette (an opaque tile has no rims at all)
                    auto transparent = [&](int x, int y) { return x >= 0 && y >= 0 && x < w && y < h && px(x, y)[3] <= 7; };
                    double lenError = 0.0;
                    size_t count = 0, blueLow = 0, top = 0, topDown = 0, bottom = 0, bottomUp = 0;
                    for (int y = 0; y < h; y++) {
                        for (int x = 0; x < w; x++) {
                            if (!opaque(x, y)) continue;
                            const uint8_t* p = px(x, y);
                            const double nx = p[0] / 255.0 * 2 - 1, ny = p[1] / 255.0 * 2 - 1, nz = p[2] / 255.0 * 2 - 1;
                            lenError += std::fabs(std::sqrt(nx * nx + ny * ny + nz * nz) - 1.0);
                            count++;
                            if (p[2] < 128) blueLow++;
                            // the rims: transparent (or the image's border) above / below
                            if (transparent(x, y - 1)) {
                                top++;
                                if (p[1] < 128) topDown++;
                            }
                            if (transparent(x, y + 1)) {
                                bottom++;
                                if (p[1] > 128) bottomUp++;
                            }
                        }
                    }
                    Analysis a;
                    if (count > 0) {
                        a.unitOk = lenError / static_cast<double>(count) <= 0.1;
                        a.blueLowPct = 100.0 * static_cast<double>(blueLow) / static_cast<double>(count);
                    }
                    if (top >= 32 && bottom >= 32) {
                        const double down = static_cast<double>(topDown) / static_cast<double>(top);
                        const double up = static_cast<double>(bottomUp) / static_cast<double>(bottom);
                        if (down >= 0.7 && up >= 0.7) a.green = "directx";
                        else if (1.0 - down >= 0.7) a.green = "opengl";
                    }
                    (*entry)["analysis"] = {{"unit_ok", a.unitOk}, {"blue_low_pct", std::round(a.blueLowPct * 10.0) / 10.0}, {"green_verdict", a.green}};
                    if (entry != &scratch) cache.dirty = true;
                }
                const json& j = (*entry)["analysis"];
                Analysis a;
                a.unitOk = j.value("unit_ok", true);
                a.blueLowPct = j.value("blue_low_pct", 0.0);
                a.green = j.value("green_verdict", std::string("unknown"));
                return a;
            }

            static std::string scalar(const YAML::Node& node, const char* key, const std::string& fallback = std::string()) {
                const YAML::Node v = node.IsMap() ? node[key] : YAML::Node();
                return v && v.IsScalar() ? v.Scalar() : fallback;
            }

            // A texture node's file path (source: path), "" otherwise
            static std::string texturePath(const YAML::Node& texture) {
                if (!texture || !texture.IsMap()) return std::string();
                const std::string source = scalar(texture, "source", "path");
                if (source != "path") return std::string();
                return scalar(texture, "path");
            }

            struct SceneUse {
                size_t mapped = 0;           // submeshes with a normal texture
                int lights = 0;
                int lightsWithHeight = 0;
            };

            // whether the GameObject has a Sprite, Tilemap, Polygon or Polygon Mesh component (component keys
            // compared case-insensitively, as Catalog reads them)
            static bool is2D(const YAML::Node& components) {
                if (!components.IsMap()) return false;
                for (const auto& item : components) {
                    if (!item.first.IsScalar()) continue;
                    std::string key = item.first.Scalar();
                    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                    if (key == "sprite" || key == "tilemap" || key == "polygon" || key == "meshpolygon") return true;
                }
                return false;
            }

            // one GameObject (an entity, an override or an added GameObject of a prefab instance)
            void checkComponents(const fs::path& file, const fs::path& assets, const std::string& entityName, const YAML::Node& components,
                                 SceneUse& use, std::set<std::string>& seenMaps) {
                const std::string at = rel(file);
                if (const YAML::Node light = components["light2d"]; light && light.IsMap()) {
                    if (scalar(light, "enabled", "true") != "false") {
                        use.lights++;
                        double height = 0.0;
                        try {
                            height = light["height"] ? light["height"].as<double>() : 0.0;
                        } catch (const std::exception&) {
                        }
                        if (height > 0.0) use.lightsWithHeight++;
                    }
                }
                // only 2D meshes (a Sprite, Tilemap, Polygon or Polygon Mesh): a 3D mesh's normal map is a Lit
                // material's, sized and filtered independently of its colour texture
                if (!is2D(components)) return;
                const YAML::Node mesh = components["mesh"];
                if (!mesh || !mesh.IsMap() || !mesh["submeshes"] || !mesh["submeshes"].IsSequence()) return;
                // the Sprite's frames as stored: pixel rects, or a rect given as fractions of the
                // texture (each of x, y, w, h in [0, 1], Rect::isNormalized), scaled like the session's
                // spriteInfo does (double precision, rounded once)
                struct Frame {
                    double x, y, w, h;
                };
                std::vector<Frame> frames;
                // a sheet sprite drawing a texture sheet (S45): that texture's frames, and its colour texture
                std::string sheetTexture;
                if (const YAML::Node sprite = components["sprite"]; sprite && sprite.IsMap() && sprite["sheet"] && sprite["sheet"].IsScalar()) {
                    const std::string sheet = sprite["sheet"].as<std::string>("");
                    if (SpriteSheetSources::isTexturePath(sheet)) {
                        sheetTexture = sheet;
                        const fs::path texture = (assets / fs::u8path(sheet)).lexically_normal();
                        const sprite::TextureSheetInfo info = sprite::textureSheetSettings(texture);
                        const sprite::TextureSheetBuild built = sprite::buildTextureSheet(texture, sheet, info.settings);
                        for (const SpriteFrame& f : built.sheet.frames) {
                            Rect r = f.rect;
                            frames.push_back({r.getX(), r.getY(), r.getWidth(), r.getHeight()});
                        }
                    }
                } else if (const YAML::Node sprite = components["sprite"]; sprite && sprite.IsMap() && sprite["framesRect"] && sprite["framesRect"].IsSequence()) {
                    for (const YAML::Node& frame : sprite["framesRect"]) {
                        const YAML::Node r = frame.IsMap() ? frame["rect"] : YAML::Node();
                        if (!r || !r.IsSequence() || r.size() != 4) continue;
                        try {
                            const double x = r[0].as<double>(), y = r[1].as<double>(), w = r[2].as<double>(), h = r[3].as<double>();
                            if (w <= 0 || h <= 0) continue;
                            frames.push_back({x, y, w, h});
                        } catch (const std::exception&) {
                        }
                    }
                }
                const YAML::Node submeshes = mesh["submeshes"];
                for (size_t i = 0; i < submeshes.size(); i++) {
                    const YAML::Node material = submeshes[i].IsMap() ? submeshes[i]["material"] : YAML::Node();
                    if (!material || !material.IsMap()) continue;
                    const YAML::Node normalNode = material["normalTexture"];
                    const std::string normalStored = texturePath(normalNode);
                    if (normalStored.empty()) continue;
                    use.mapped++;
                    const fs::path normal = (assets / fs::u8path(normalStored)).lexically_normal();
                    std::error_code ec;
                    if (lowerExt(normal) != ".png" || !fs::is_regular_file(normal, ec)) continue;
                    if (seenMaps.insert(rel(normal)).second) result.maps++;
                    const std::string where = "GameObject '" + entityName + "' submesh " + std::to_string(i) + ": ";
                    const YAML::Node baseNode = material["baseColorTexture"];
                    // (a sheet sprite saves no colour texture path: its sheet's texture is it)
                    const std::string baseStored = i == 0 && !sheetTexture.empty() ? sheetTexture : texturePath(baseNode);
                    const fs::path base = baseStored.empty() ? fs::path() : (assets / fs::u8path(baseStored)).lexically_normal();
                    const PngInfo info = png(normal);
                    int bw = 0, bh = 0;
                    const bool baseKnown = !base.empty() && texsynth::probeImage(base, bw, bh) && bw > 0 && bh > 0;
                    if (baseKnown && info.width > 0 && (info.width != bw || info.height != bh)) {
                        result.findings.push_back({"normals", at, 0, 0, "error",
                                                   where + name(normal) + " is " + std::to_string(info.width) + "x" + std::to_string(info.height) +
                                                       " but its colour texture " + name(base) + " is " + std::to_string(bw) + "x" + std::to_string(bh) +
                                                       ": they share UVs, so the lighting lands on the wrong pixels",
                                                   "normals-size-mismatch",
                                                   "generate the normal map from this colour texture (generate_sprite_normals {source: <GameObject>, output: "
                                                   "true, apply: true})"});
                    }
                    if (baseNode && baseNode.IsMap()) {
                        const std::string bmin = scalar(baseNode, "minFilter", "linear"), bmag = scalar(baseNode, "magFilter", "linear");
                        const std::string nmin = scalar(normalNode, "minFilter", "linear"), nmag = scalar(normalNode, "magFilter", "linear");
                        if (bmin != nmin || bmag != nmag) {
                            result.findings.push_back({"normals", at, 0, 0, "warning",
                                                       where + "the colour texture " + name(base) + " is filtered " + bmin + "/" + bmag + " but its normal map " +
                                                           name(normal) + " " + nmin + "/" + nmag + ": the lighting smears across pixel edges",
                                                       "normals-filter-mismatch", "set the normal texture's filter to the colour texture's (apply: true does it)"});
                        }
                    }
                    if (info.recipe) {
                        const json& recipe = *info.recipe;
                        const json settings = recipe.contains("settings") ? recipe["settings"] : json::object();
                        if (settings.is_object() && settings.value("directx", false)) {
                            result.findings.push_back({"normals", at, 0, 0, "warning",
                                                       where + name(normal) + " was generated with DirectX green (directx: true); the engine reads OpenGL "
                                                                              "(green up), so the lights look upside down",
                                                       "normals-directx", "regenerate without directx: the engine reads OpenGL (green up)"});
                        }
                        // the frames it was made for vs the Sprite's now (submesh 0 draws the Sprite)
                        const json recipeFrames = recipe.contains("frames") ? recipe["frames"] : json::object();
                        if (i == 0 && !frames.empty() && recipeFrames.is_object() && recipeFrames.contains("cells") && !recipeFrames["cells"].empty()) {
                            std::vector<Cell> cells;
                            for (const json& c : recipeFrames["cells"]) cells.push_back({c[0].get<int>(), c[1].get<int>(), c[2].get<int>(), c[3].get<int>()});
                            std::vector<Cell> sprite;
                            // fractions are of the colour texture (the normal map's size when it is unknown)
                            const double tw = baseKnown ? bw : std::max(1, info.width), th = baseKnown ? bh : std::max(1, info.height);
                            for (const Frame& f : frames) {
                                const bool normalized = f.x >= 0 && f.x <= 1 && f.y >= 0 && f.y <= 1 && f.w <= 1 && f.h <= 1;
                                const double sx = normalized ? tw : 1.0, sy = normalized ? th : 1.0;
                                sprite.push_back({static_cast<int>(std::lround(f.x * sx)), static_cast<int>(std::lround(f.y * sy)),
                                                  static_cast<int>(std::lround(f.w * sx)), static_cast<int>(std::lround(f.h * sy))});
                            }
                            if (sprite != cells) {
                                result.findings.push_back({"normals", at, 0, 0, "warning",
                                                           where + name(normal) + " was made for " + std::to_string(cells.size()) +
                                                               " frame(s) that differ from the Sprite's " + std::to_string(sprite.size()) +
                                                               " (the sprite was resliced after generating): each frame is lit by another frame's normals",
                                                           "normals-frames-mismatch", "regenerate with frames auto from the Sprite"});
                            }
                        }
                    } else if (info.recipeError.empty()) {
                        // a map made elsewhere: what its pixels say
                        const std::optional<Analysis> a = analyse(normal);
                        if (!a) continue;
                        if (!a->unitOk || a->blueLowPct > 5.0) {
                            result.findings.push_back({"normals", at, 0, 0, "warning",
                                                       where + name(normal) + " does not look like a normal map (" +
                                                           (!a->unitOk ? std::string("its opaque texels are not unit vectors")
                                                                       : std::to_string(static_cast<int>(std::lround(a->blueLowPct))) + "% of its opaque texels have blue below 128") +
                                                           "): a colour image in the normal slot?",
                                                       "normals-not-a-normal-map", "assign a normal map (generate_sprite_normals) or clear the slot"});
                        } else if (a->green == "directx") {
                            result.findings.push_back({"normals", at, 0, 0, "warning",
                                                       where + name(normal) + " looks like a DirectX normal map: its top rims point down and its bottom rims up",
                                                       "normals-green-convention",
                                                       "its green looks flipped (DirectX): the engine reads OpenGL since this version; invert its green channel, "
                                                       "or generate it again"});
                        }
                    }
                }
            }

            // every map with "components" below `node`: entities, prefab-instance overrides and
            // added GameObjects (the --anims walk)
            void walk(const fs::path& file, const fs::path& assets, const YAML::Node& node, SceneUse& use, std::set<std::string>& seen, int depth = 0) {
                if (depth > 64 || !node) return;
                if (node.IsMap()) {
                    const YAML::Node components = node["components"];
                    if (components && components.IsMap()) {
                        checkComponents(file, assets, scalar(node, "name", "(a prefab instance override)"), components, use, seen);
                    }
                    for (const auto& item : node) {
                        if (item.first.IsScalar() && item.first.Scalar() == "components") continue;
                        walk(file, assets, item.second, use, seen, depth + 1);
                    }
                } else if (node.IsSequence()) {
                    for (const YAML::Node& child : node) walk(file, assets, child, use, seen, depth + 1);
                }
            }

            void checkScenes() {
                const fs::path assets = ProjectChecks::assetsRootFromProjectFile(root);
                std::set<std::string> seen;
                for (const fs::path& file : ProjectChecks::findProjectFiles(root, {".scene", ".prefab"})) {
                    SceneUse use;
                    try {
                        // read through an fs::path stream: YAML::LoadFile takes a narrow string,
                        // which is not the file's name on Windows when it has non-ASCII characters
                        std::ifstream in(file, std::ios::binary);
                        if (!in) throw std::runtime_error("cannot open the file");
                        walk(file, assets, YAML::Load(in), use, seen);
                    } catch (const std::exception& e) {
                        result.findings.push_back({"normals", rel(file), 0, 0, "warning", std::string("could not read its normal maps: ") + e.what(), "", ""});
                        continue;
                    }
                    if (lowerExt(file) == ".scene" && use.mapped > 0 && use.lights > 0 && use.lightsWithHeight == 0) {
                        result.findings.push_back({"normals", rel(file), 0, 0, "warning",
                                                   "the scene uses normal maps and has " + std::to_string(use.lights) +
                                                       " Light 2D(s), none with a height above 0: the normal maps change nothing",
                                                   "normals-no-light-height", "set a Light 2D's height above 0 (64 is a good start): normal maps respond only then"});
                    }
                }
            }
        };

        // <stem>_normal.png, <stem>_sheet_normal.png beside an image
        std::vector<fs::path> defaultMaps(const fs::path& image) {
            const std::string stem = jasperengine::editor::FileUtils::pathToUtf8(image.stem());
            return {image.parent_path() / fs::u8path(stem + "_normal.png"), image.parent_path() / fs::u8path(stem + "_sheet_normal.png")};
        }

    }

    NormalsCheck checkSpriteNormals(const fs::path& projectRoot, const std::vector<fs::path>& files) {
        Checker c;
        c.root = projectRoot.lexically_normal();
        loadCache(c.cache, c.root);
        if (files.empty()) {
            std::set<std::string> present;
            for (const fs::path& png : ProjectChecks::findProjectFiles(c.root, {".png"})) {
                present.insert(c.rel(png));
                c.checkGenerated(png, c.png(png), false);
            }
            // forget PNGs that are gone, then the reverse index from what is left
            for (auto it = c.cache.pngs.begin(); it != c.cache.pngs.end();) {
                if (!present.count(it.key())) {
                    it = c.cache.pngs.erase(it);
                    c.cache.dirty = true;
                } else {
                    ++it;
                }
            }
            c.rebuildIndex();
            c.checkScenes();
        } else {
            std::set<fs::path> checked;
            for (const fs::path& given : files) {
                const fs::path file = given.lexically_normal();
                const std::string ext = lowerExt(file);
                if (ext == ".svg") continue;
                if (ext == ".png") {
                    const PngInfo info = c.png(file);
                    if (info.recipe || !info.recipeError.empty()) {
                        if (checked.insert(file).second) c.checkGenerated(file, info, true);
                        continue;
                    }
                }
                // a source: the maps made from it (the cache index, the default names)
                std::vector<fs::path> candidates = defaultMaps(file);
                for (const fs::path& map : cachedMapsOf(c.root, file)) candidates.push_back(map);
                for (const fs::path& candidate : candidates) {
                    std::error_code ec;
                    if (checked.count(candidate.lexically_normal()) || !fs::is_regular_file(candidate, ec)) continue;
                    const PngInfo info = c.png(candidate);
                    if (!info.recipe || c.sourceOf(*info.recipe, candidate) != file) continue;
                    checked.insert(candidate.lexically_normal());
                    c.checkGenerated(candidate, info, true);
                }
            }
        }
        saveCache(c.cache);
        ProjectChecks::sortFindings(c.result.findings);
        return c.result;
    }

    std::vector<fs::path> cachedMapsOf(const fs::path& projectRoot, const fs::path& source) {
        std::vector<fs::path> out;
        Cache cache;
        loadCache(cache, projectRoot);
        const std::string key = ProjectChecks::projectRelative(projectRoot, source);
        if (!cache.index.contains(key) || !cache.index[key].is_array()) return out;
        for (const json& png : cache.index[key]) {
            if (png.is_string()) out.push_back((projectRoot / fs::u8path(png.get<std::string>())).lexically_normal());
        }
        return out;
    }

}
