// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SynthesisRecipe.h"

#include "EditorVersion.h"

#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>

namespace fs = std::filesystem;
using nlohmann::json;

namespace jasperengine::editor::texsynth {

    namespace {

        const uint8_t kPngSignature[8] = {137, 80, 78, 71, 13, 10, 26, 10};

        uint32_t crc32(const uint8_t* data, size_t length, uint32_t crc = 0) {
            static const std::array<uint32_t, 256> table = [] {
                std::array<uint32_t, 256> t{};
                for (uint32_t n = 0; n < 256; n++) {
                    uint32_t c = n;
                    for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                    t[n] = c;
                }
                return t;
            }();
            crc = ~crc;
            for (size_t i = 0; i < length; i++) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
            return ~crc;
        }

        uint32_t readU32(const uint8_t* p) {
            return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) | (static_cast<uint32_t>(p[2]) << 8) | p[3];
        }

        void putU32(std::vector<uint8_t>& out, uint32_t v) {
            out.push_back(static_cast<uint8_t>(v >> 24));
            out.push_back(static_cast<uint8_t>(v >> 16));
            out.push_back(static_cast<uint8_t>(v >> 8));
            out.push_back(static_cast<uint8_t>(v));
        }

        struct Chunk {
            size_t offset;   // of the length field
            uint32_t length;
            char type[5];
        };

        // The chunks of a PNG; false when the structure is broken (truncated, bad signature)
        bool listChunks(const uint8_t* bytes, size_t size, std::vector<Chunk>& chunks, std::string& error) {
            if (size < 8 || std::memcmp(bytes, kPngSignature, 8) != 0) {
                error = "not a PNG";
                return false;
            }
            size_t pos = 8;
            while (pos < size) {
                if (size - pos < 12) {
                    error = "truncated PNG";
                    return false;
                }
                Chunk chunk;
                chunk.offset = pos;
                chunk.length = readU32(bytes + pos);
                std::memcpy(chunk.type, bytes + pos + 4, 4);
                chunk.type[4] = 0;
                if (chunk.length > size - pos - 12) {
                    error = "truncated PNG";
                    return false;
                }
                chunks.push_back(chunk);
                pos += 12 + static_cast<size_t>(chunk.length);
                if (std::strcmp(chunk.type, "IEND") == 0) break;
            }
            return true;
        }

        bool isRecipeChunk(const uint8_t* bytes, const Chunk& chunk) {
            if (std::strcmp(chunk.type, "iTXt") != 0 && std::strcmp(chunk.type, "tEXt") != 0) return false;
            const size_t keyLength = std::strlen(kRecipeKeyword);
            return chunk.length > keyLength && std::memcmp(bytes + chunk.offset + 8, kRecipeKeyword, keyLength) == 0 &&
                   bytes[chunk.offset + 8 + keyLength] == 0;
        }

        std::string recipePath(const fs::path& path, const RecipePaths& paths) {
            if (path.empty()) return std::string();
            if (!paths.projectDir.empty()) {
                const fs::path rel = path.lexically_relative(paths.projectDir);
                if (!rel.empty() && *rel.begin() != ".." && !rel.is_absolute()) return rel.generic_u8string();
                return path.generic_u8string();
            }
            if (!paths.outputDir.empty()) {
                const fs::path rel = path.lexically_relative(paths.outputDir);
                if (!rel.empty()) return rel.generic_u8string();
            }
            return path.generic_u8string();
        }

        json source(const ImageInput& input, const RecipePaths& paths) {
            json j = json::object();
            if (!input.path.empty()) {
                j["path"] = recipePath(input.path, paths);
                j["sha1"] = input.sha1;
            } else if (!input.image.empty()) {
                j["pixelsSha1"] = sha1Hex(input.image.data.data(), input.image.data.size());
            }
            return j;
        }

    }

    json paramsToJson(const Params& p) {
        auto decimal = [](float v) { return std::round(static_cast<double>(v) * 1e6) / 1e6; };
        return {{"nearestNeighbors", p.nearestNeighbors},         {"randomSampleLocations", p.randomSampleLocations},
                {"cauchyDispersion", decimal(p.cauchyDispersion)}, {"backtrackPercent", decimal(p.backtrackPercent)},
                {"backtrackStages", p.backtrackStages},           {"guideAlpha", decimal(p.guideAlpha)},
                {"guideDecay", p.guideDecay},                     {"borderFraction", decimal(p.borderFraction)}};
    }

    json makeRecipe(const Request& request, const Result& result, const std::string& role, const RecipePaths& paths) {
        const Params& p = result.params;
        json recipe = json::object();
        recipe["format"] = kRecipeFormat;
        recipe["generator"] = "jasperengine-editor texsynth";
        recipe["engine"] = JASPERENGINE_EDITOR_VERSION;
        recipe["role"] = role;
        recipe["mode"] = toString(request.mode);
        recipe["quality"] = toString(request.quality);
        recipe["params"] = paramsToJson(p);
        if (result.seed <= (1ULL << 53)) recipe["seed"] = result.seed;
        else recipe["seed"] = std::to_string(result.seed);
        recipe["size"] = {result.width, result.height};
        recipe["pathBase"] = paths.projectDir.empty() ? "file" : "project";

        json examples = json::array();
        for (const Example& ex : request.examples) {
            json e = source(ex.image, paths);
            e["sampleable"] = ex.sampleable;
            if (ex.sampleMask.given()) e["sampleMask"] = source(ex.sampleMask, paths);
            if (ex.guide.given()) e["guide"] = source(ex.guide, paths);
            examples.push_back(e);
        }
        recipe["examples"] = examples;
        json companions = json::array();
        for (const Companion& companion : request.companions) {
            json sources = json::array();
            for (const ImageInput& input : companion.images) sources.push_back(source(input, paths));
            companions.push_back({{"name", companion.name}, {"sources", sources}});
        }
        recipe["companions"] = companions;
        if (request.mode == Mode::Inpaint) {
            json mask = request.inpaintMask.image.given() ? source(request.inpaintMask.image, paths) : json{{"source", "alpha"}};
            if (request.inpaintMask.image.given()) mask["channel"] = toString(request.inpaintMask.channel);
            mask["invert"] = request.inpaintMask.invert;
            recipe["inpaintMask"] = mask;
        }
        if (request.mode == Mode::Guided && request.targetGuide.given()) recipe["targetGuide"] = source(request.targetGuide, paths);
        json outputs = json::object();
        for (const auto& [name, path] : paths.outputs) outputs[name] = recipePath(path, paths);
        recipe["outputs"] = outputs;
        return recipe;
    }

    bool insertRecipeChunk(const std::vector<uint8_t>& png, const std::string& text, std::vector<uint8_t>& out, std::string& error) {
        std::vector<Chunk> chunks;
        if (!listChunks(png.data(), png.size(), chunks, error)) return false;
        if (chunks.empty() || std::strcmp(chunks[0].type, "IHDR") != 0) {
            error = "PNG without IHDR";
            return false;
        }
        // iTXt: keyword, 0, compression flag 0, method 0, empty language tag, 0, empty translated keyword, 0, text
        std::vector<uint8_t> chunk;
        const size_t keyLength = std::strlen(kRecipeKeyword);
        const uint32_t length = static_cast<uint32_t>(keyLength + 5 + text.size());
        putU32(chunk, length);
        const size_t typeStart = chunk.size();
        chunk.insert(chunk.end(), {'i', 'T', 'X', 't'});
        chunk.insert(chunk.end(), kRecipeKeyword, kRecipeKeyword + keyLength);
        chunk.insert(chunk.end(), {0, 0, 0, 0, 0});
        chunk.insert(chunk.end(), text.begin(), text.end());
        putU32(chunk, crc32(chunk.data() + typeStart, chunk.size() - typeStart));

        out.clear();
        out.reserve(png.size() + chunk.size());
        out.insert(out.end(), png.begin(), png.begin() + 8);
        for (size_t i = 0; i < chunks.size(); i++) {
            const Chunk& c = chunks[i];
            if (isRecipeChunk(png.data(), c)) continue;
            const auto begin = png.begin() + static_cast<std::ptrdiff_t>(c.offset);
            out.insert(out.end(), begin, begin + 12 + static_cast<std::ptrdiff_t>(c.length));
            if (i == 0) out.insert(out.end(), chunk.begin(), chunk.end());
        }
        return true;
    }

    bool writePngWithRecipe(const fs::path& path, const Image& image, const json& recipe, std::string& error) {
        std::vector<uint8_t> png, withRecipe;
        if (!encodePng(image, png)) {
            error = "PNG encoding failed (" + describeFormat(image) + ")";
            return false;
        }
        if (!insertRecipeChunk(png, recipe.dump(), withRecipe, error)) return false;
        fs::path temporary = path;
        temporary += ".partial";
        {
            std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
            if (!file) {
                error = "cannot write " + path.string();
                return false;
            }
            file.write(reinterpret_cast<const char*>(withRecipe.data()), static_cast<std::streamsize>(withRecipe.size()));
            if (!file) {
                error = "cannot write " + path.string();
                std::error_code ec;
                file.close();
                fs::remove(temporary, ec);
                return false;
            }
        }
        std::error_code ec;
        fs::rename(temporary, path, ec);
        if (ec) {
            fs::remove(temporary, ec);
            error = "cannot write " + path.string() + ": " + ec.message();
            return false;
        }
        return true;
    }

    std::optional<std::string> readRecipeTextFromPng(const uint8_t* bytes, size_t size, std::string* error) {
        std::string problem;
        std::vector<Chunk> chunks;
        const bool whole = listChunks(bytes, size, chunks, problem);
        // a truncated file may still hold the recipe in the chunks before the damage
        for (const Chunk& c : chunks) {
            if (!isRecipeChunk(bytes, c)) continue;
            const uint8_t* data = bytes + c.offset + 8;
            const uint32_t stored = readU32(data + c.length);
            if (crc32(bytes + c.offset + 4, 4 + static_cast<size_t>(c.length)) != stored) {
                if (error) *error = "garbled jasper.synthesis chunk (CRC mismatch)";
                return std::nullopt;
            }
            const size_t keyLength = std::strlen(kRecipeKeyword);
            size_t pos = keyLength + 1;
            if (std::strcmp(c.type, "tEXt") == 0) {
                return std::string(reinterpret_cast<const char*>(data + pos), c.length - pos);
            }
            // iTXt: compression flag, method, language tag\0, translated keyword\0, text
            if (c.length < pos + 2) break;
            if (data[pos] != 0) {
                if (error) *error = "compressed jasper.synthesis chunk (not written by the synthesis)";
                return std::nullopt;
            }
            pos += 2;
            for (int field = 0; field < 2; field++) {
                while (pos < c.length && data[pos] != 0) pos++;
                if (pos >= c.length) {
                    if (error) *error = "malformed jasper.synthesis chunk";
                    return std::nullopt;
                }
                pos++;
            }
            return std::string(reinterpret_cast<const char*>(data + pos), c.length - pos);
        }
        if (error) *error = whole ? std::string() : problem;
        return std::nullopt;
    }

    std::optional<std::string> readRecipeText(const fs::path& path, std::string* error) {
        std::ifstream file(path, std::ios::binary);
        if (!file) {
            if (error) *error = "cannot open " + path.string();
            return std::nullopt;
        }
        const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        return readRecipeTextFromPng(bytes.data(), bytes.size(), error);
    }

    std::optional<json> readRecipe(const fs::path& path, std::string* error) {
        const std::optional<std::string> text = readRecipeText(path, error);
        if (!text) return std::nullopt;
        json parsed = json::parse(*text, nullptr, false);
        if (parsed.is_discarded() || !parsed.is_object()) {
            if (error) *error = "the jasper.synthesis chunk is not a JSON object";
            return std::nullopt;
        }
        return parsed;
    }

    std::vector<SourceState> checkRecipeSources(const json& recipe, const fs::path& recipeFile, const fs::path& projectDir) {
        std::vector<SourceState> states;
        const bool fromProject = recipe.value("pathBase", std::string("file")) == "project";
        const fs::path base = fromProject ? projectDir : recipeFile.parent_path();
        auto check = [&](const std::string& role, const json& entry) {
            if (!entry.is_object()) return;
            SourceState state;
            state.role = role;
            state.path = entry.value("path", std::string());
            if (state.path.empty()) {
                state.state = "unknown";
                states.push_back(state);
                return;
            }
            const fs::path recorded = fs::u8path(state.path);
            state.resolved = recorded.is_absolute() ? recorded : base / recorded;
            std::ifstream file(state.resolved, std::ios::binary);
            if (!file) {
                state.state = "missing";
            } else {
                const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
                state.state = sha1Hex(bytes.data(), bytes.size()) == entry.value("sha1", std::string()) ? "unchanged" : "changed";
            }
            states.push_back(state);
        };
        if (recipe.contains("examples") && recipe["examples"].is_array()) {
            for (size_t i = 0; i < recipe["examples"].size(); i++) {
                const json& ex = recipe["examples"][i];
                const std::string role = "examples[" + std::to_string(i) + "]";
                check(role, ex);
                if (ex.is_object() && ex.contains("sampleMask")) check(role + ".sampleMask", ex["sampleMask"]);
                if (ex.is_object() && ex.contains("guide")) check(role + ".guide", ex["guide"]);
            }
        }
        if (recipe.contains("companions") && recipe["companions"].is_array()) {
            for (const json& companion : recipe["companions"]) {
                if (!companion.is_object() || !companion.contains("sources") || !companion["sources"].is_array()) continue;
                const std::string name = companion.value("name", std::string("?"));
                for (size_t i = 0; i < companion["sources"].size(); i++) {
                    check("companions." + name + "[" + std::to_string(i) + "]", companion["sources"][i]);
                }
            }
        }
        if (recipe.contains("inpaintMask")) check("inpaintMask", recipe["inpaintMask"]);
        if (recipe.contains("targetGuide")) check("targetGuide", recipe["targetGuide"]);
        return states;
    }

}
