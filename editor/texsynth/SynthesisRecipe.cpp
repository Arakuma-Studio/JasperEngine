// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SynthesisRecipe.h"

#include "EditorVersion.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cmath>
#include <cstdio>
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

        // The first string (key or value) of `value` that is not valid UTF-8, with where it is
        bool findInvalidString(const json& value, const std::string& where, std::string& found) {
            if (value.is_string()) {
                if (isValidUtf8(value.get_ref<const std::string&>())) return false;
                found = where + " \"" + printableUtf8(value.get_ref<const std::string&>()) + "\"";
                return true;
            }
            if (value.is_object()) {
                for (auto it = value.begin(); it != value.end(); ++it) {
                    const std::string at = where.empty() ? it.key() : where + "." + it.key();
                    if (!isValidUtf8(it.key())) {
                        found = "the key \"" + printableUtf8(it.key()) + "\"";
                        return true;
                    }
                    if (findInvalidString(it.value(), printableUtf8(at), found)) return true;
                }
            } else if (value.is_array()) {
                for (size_t i = 0; i < value.size(); i++) {
                    if (findInvalidString(value[i], where + "[" + std::to_string(i) + "]", found)) return true;
                }
            }
            return false;
        }

        // ---- validateRecipe

        std::string article(const char* type) {
            const std::string t = type;
            return (t == "object" || t == "array") ? "an " + t : "a " + t;
        }

        struct RecipeChecker {
            std::string error;

            bool wrong(const std::string& where, const json& value, const char* expected) {
                if (error.empty()) error = where + " is " + article(value.type_name()) + ", expected " + expected;
                return false;
            }
            bool optionalString(const json& object, const char* key, const std::string& where) {
                if (!object.contains(key) || object[key].is_string()) return true;
                return wrong(where + key, object[key], "a string");
            }
            bool optionalBool(const json& object, const char* key, const std::string& where) {
                if (!object.contains(key) || object[key].is_boolean()) return true;
                return wrong(where + key, object[key], "true or false");
            }
            bool optionalInteger(const json& object, const char* key, const std::string& where, int64_t lo, int64_t hi) {
                if (!object.contains(key)) return true;
                const json& v = object[key];
                bool ok = false;
                if (v.is_number_unsigned()) {
                    const uint64_t u = v.get<uint64_t>();
                    ok = hi >= 0 && u <= static_cast<uint64_t>(hi) && (lo <= 0 || u >= static_cast<uint64_t>(lo));
                } else if (v.is_number_integer()) {
                    const int64_t x = v.get<int64_t>();
                    ok = x >= lo && x <= hi;
                }
                if (!ok && error.empty()) {
                    error = where + key + " is " + (v.is_number() ? v.dump() : article(v.type_name())) + ", expected an integer in [" +
                            std::to_string(lo) + ", " + std::to_string(hi) + "]";
                }
                return ok;
            }
            // within +-1e6: read as float, a larger value would not convert
            bool optionalNumber(const json& object, const char* key, const std::string& where) {
                if (!object.contains(key)) return true;
                const json& v = object[key];
                if (v.is_number() && std::abs(v.get<double>()) <= 1e6) return true;
                if (!v.is_number()) return wrong(where + key, v, "a number");
                if (error.empty()) error = where + key + " is " + v.dump() + ", expected a number within +-1e6";
                return false;
            }
            // {path?, sha1?, pixelsSha1?}
            bool source(const json& value, const std::string& where) {
                if (!value.is_object()) return wrong(where, value, "an object");
                const std::string at = where + ".";
                return optionalString(value, "path", at) && optionalString(value, "sha1", at) && optionalString(value, "pixelsSha1", at);
            }
        };

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

    // ------------------------------------------------------------------ text

    bool isValidUtf8(const std::string& text) {
        const auto* p = reinterpret_cast<const unsigned char*>(text.data());
        const size_t n = text.size();
        size_t i = 0;
        while (i < n) {
            const unsigned char c = p[i];
            if (c < 0x80) {
                i++;
                continue;
            }
            size_t length = 0;
            uint32_t cp = 0;
            if (c >= 0xC2 && c <= 0xDF) {
                length = 2;
                cp = c & 0x1Fu;
            } else if (c >= 0xE0 && c <= 0xEF) {
                length = 3;
                cp = c & 0x0Fu;
            } else if (c >= 0xF0 && c <= 0xF4) {
                length = 4;
                cp = c & 0x07u;
            } else {
                return false;   // a continuation byte, an overlong lead (C0, C1) or F5..FF
            }
            if (n - i < length) return false;
            for (size_t k = 1; k < length; k++) {
                if ((p[i + k] & 0xC0u) != 0x80u) return false;
                cp = (cp << 6) | (p[i + k] & 0x3Fu);
            }
            // overlong, surrogates, beyond U+10FFFF
            if ((length == 3 && cp < 0x800) || (length == 4 && cp < 0x10000) || (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) return false;
            i += length;
        }
        return true;
    }

    std::string printableUtf8(const std::string& text) {
        if (isValidUtf8(text)) return text;
        std::string out;
        size_t i = 0;
        while (i < text.size()) {
            // the longest valid sequence starting here (1..4 bytes), else one escaped byte
            size_t take = 0;
            for (size_t length = 1; length <= 4 && i + length <= text.size(); length++) {
                if (isValidUtf8(text.substr(i, length))) {
                    take = length;
                    break;
                }
            }
            if (take) {
                out.append(text, i, take);
                i += take;
            } else {
                char buffer[8];
                std::snprintf(buffer, sizeof(buffer), "\\x%02X", static_cast<unsigned char>(text[i]));
                out += buffer;
                i++;
            }
        }
        return out;
    }

    bool jsonText(const json& value, std::string& text, std::string& error) {
        std::string found;
        if (findInvalidString(value, "", found)) {
            error = found + " is not valid UTF-8 text";
            return false;
        }
        try {
            text = value.dump();
        } catch (const std::exception& e) {
            error = e.what();
            return false;
        }
        return true;
    }

    std::string jsonTextLossy(const json& value, int indent) {
        try {
            return value.dump(indent, ' ', false, json::error_handler_t::replace);
        } catch (const std::exception&) {
            return "null";
        }
    }

    // ------------------------------------------------------------------ writing

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

    bool recipeText(const json& recipe, std::string& text, std::string& error) {
        if (jsonText(recipe, text, error)) return true;
        error = "the recipe can't record " + error + " (JSON is UTF-8): rename the file";
        return false;
    }

    bool encodePngWithRecipe(const Image& image, const json& recipe, std::vector<uint8_t>& out, std::string& error) {
        std::vector<uint8_t> png;
        if (!encodePng(image, png)) {
            error = "PNG encoding failed (" + describeFormat(image) + ")";
            return false;
        }
        std::string text;
        if (!recipeText(recipe, text, error)) return false;
        return insertRecipeChunk(png, text, out, error);
    }

    bool writePngWithRecipe(const fs::path& path, const Image& image, const json& recipe, std::string& error) {
        std::vector<uint8_t> withRecipe;
        if (!encodePngWithRecipe(image, recipe, withRecipe, error)) return false;
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

    std::optional<std::string> readRecipeTextHead(const fs::path& path, std::string* error) {
        std::ifstream file(path, std::ios::binary);
        if (!file) {
            if (error) *error = "cannot open " + path.string();
            return std::nullopt;
        }
        static const uint8_t signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
        std::vector<uint8_t> head(8);
        if (!file.read(reinterpret_cast<char*>(head.data()), 8) || std::memcmp(head.data(), signature, 8) != 0) {
            if (error) *error = "not a PNG";
            return std::nullopt;
        }
        // the text chunks before IDAT, after the signature: the others are seeked over
        for (int chunks = 0; chunks < 4096; chunks++) {
            uint8_t header[8];
            if (!file.read(reinterpret_cast<char*>(header), 8)) break;
            const uint32_t length = readU32(header);
            const bool text = std::memcmp(header + 4, "iTXt", 4) == 0 || std::memcmp(header + 4, "tEXt", 4) == 0;
            if (std::memcmp(header + 4, "IDAT", 4) == 0 || std::memcmp(header + 4, "IEND", 4) == 0) break;
            if (text && length < (64u << 20)) {
                const size_t at = head.size();
                head.resize(at + 12 + length);
                std::memcpy(head.data() + at, header, 8);
                if (!file.read(reinterpret_cast<char*>(head.data() + at + 8), static_cast<std::streamsize>(length) + 4)) {
                    head.resize(at);
                    break;
                }
            } else {
                file.seekg(static_cast<std::streamoff>(length) + 4, std::ios::cur);
                if (!file) break;
            }
        }
        return readRecipeTextFromPng(head.data(), head.size(), error);
    }

    std::vector<SourceState> checkRecipeSources(const json& recipe, const fs::path& recipeFile, const fs::path& projectDir,
                                                const std::function<std::string(const fs::path&)>& hashFile) {
        std::vector<SourceState> states;
        if (!recipe.is_object()) return states;
        const bool fromProject = jsonString(recipe, "pathBase", "file") == "project";
        const fs::path base = fromProject ? projectDir : recipeFile.parent_path();
        auto check = [&](const std::string& role, const json& entry) {
            if (!entry.is_object()) return;
            SourceState state;
            state.role = role;
            state.path = jsonString(entry, "path");
            if (state.path.empty()) {
                state.state = "unknown";
                states.push_back(state);
                return;
            }
            const fs::path recorded = fs::u8path(state.path);
            state.resolved = recorded.is_absolute() ? recorded : base / recorded;
            std::string hash;
            if (hashFile) {
                hash = hashFile(state.resolved);
            } else {
                std::ifstream file(state.resolved, std::ios::binary);
                if (file) {
                    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
                    hash = sha1Hex(bytes.data(), bytes.size());
                }
            }
            if (hash.empty()) state.state = "missing";
            else state.state = hash == jsonString(entry, "sha1") ? "unchanged" : "changed";
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
                const std::string name = jsonString(companion, "name", "?");
                for (size_t i = 0; i < companion["sources"].size(); i++) {
                    check("companions." + name + "[" + std::to_string(i) + "]", companion["sources"][i]);
                }
            }
        }
        if (recipe.contains("inpaintMask")) check("inpaintMask", recipe["inpaintMask"]);
        if (recipe.contains("targetGuide")) check("targetGuide", recipe["targetGuide"]);
        return states;
    }

    // ------------------------------------------------------------------ reading

    bool validateRecipe(const json& recipe, std::string& error) {
        RecipeChecker c;
        if (!recipe.is_object()) {
            c.wrong("the recipe", recipe, "an object");
            error = c.error;
            return false;
        }
        if (!recipe.contains("format")) {
            error = "format is missing";
            return false;
        }
        bool ok = c.optionalInteger(recipe, "format", "", 1, 1000000);
        for (const char* key : {"generator", "engine", "role", "mode", "quality", "pathBase"}) ok = ok && c.optionalString(recipe, key, "");
        if (ok && recipe.contains("seed")) {
            const json& seed = recipe["seed"];
            if (seed.is_string()) {
                const std::string& text = seed.get_ref<const std::string&>();
                const bool digits = !text.empty() && text.size() <= 20 && std::all_of(text.begin(), text.end(), [](char ch) { return ch >= '0' && ch <= '9'; });
                if (!digits || (text.size() == 20 && text > "18446744073709551615")) {
                    c.error = "seed is \"" + printableUtf8(text) + "\", expected a whole number";
                    ok = false;
                }
            } else if (!seed.is_number_unsigned()) {
                ok = c.wrong("seed", seed, "a whole number from 0");
            }
        }
        if (ok && recipe.contains("size")) {
            const json& size = recipe["size"];
            if (!size.is_array() || size.size() != 2) {
                ok = c.wrong("size", size, "[width, height]");
            } else {
                const json pair = {{"width", size[0]}, {"height", size[1]}};
                ok = c.optionalInteger(pair, "width", "size.", 0, 65535) && c.optionalInteger(pair, "height", "size.", 0, 65535);
            }
        }
        if (ok && recipe.contains("params")) {
            const json& p = recipe["params"];
            if (!p.is_object()) {
                ok = c.wrong("params", p, "an object");
            } else {
                for (const char* key : {"nearestNeighbors", "randomSampleLocations", "backtrackStages"}) ok = ok && c.optionalInteger(p, key, "params.", -1000000, 1000000);
                for (const char* key : {"cauchyDispersion", "backtrackPercent", "guideAlpha", "borderFraction"}) ok = ok && c.optionalNumber(p, key, "params.");
                ok = ok && c.optionalBool(p, "guideDecay", "params.");
            }
        }
        if (ok && recipe.contains("examples")) {
            const json& examples = recipe["examples"];
            if (!examples.is_array()) ok = c.wrong("examples", examples, "an array");
            for (size_t i = 0; ok && examples.is_array() && i < examples.size(); i++) {
                const std::string at = "examples[" + std::to_string(i) + "]";
                const json& ex = examples[i];
                ok = c.source(ex, at) && c.optionalBool(ex, "sampleable", at + ".");
                if (ok && ex.contains("sampleMask")) ok = c.source(ex["sampleMask"], at + ".sampleMask");
                if (ok && ex.contains("guide")) ok = c.source(ex["guide"], at + ".guide");
            }
        }
        if (ok && recipe.contains("companions")) {
            const json& companions = recipe["companions"];
            if (!companions.is_array()) ok = c.wrong("companions", companions, "an array");
            for (size_t i = 0; ok && companions.is_array() && i < companions.size(); i++) {
                const std::string at = "companions[" + std::to_string(i) + "]";
                const json& companion = companions[i];
                if (!companion.is_object()) {
                    ok = c.wrong(at, companion, "an object");
                    break;
                }
                ok = c.optionalString(companion, "name", at + ".");
                if (ok && companion.contains("sources")) {
                    const json& sources = companion["sources"];
                    if (!sources.is_array()) ok = c.wrong(at + ".sources", sources, "an array");
                    for (size_t s = 0; ok && sources.is_array() && s < sources.size(); s++) {
                        ok = c.source(sources[s], at + ".sources[" + std::to_string(s) + "]");
                    }
                }
            }
        }
        if (ok && recipe.contains("inpaintMask")) {
            const json& mask = recipe["inpaintMask"];
            ok = c.source(mask, "inpaintMask") && c.optionalString(mask, "channel", "inpaintMask.") && c.optionalString(mask, "source", "inpaintMask.") &&
                 c.optionalBool(mask, "invert", "inpaintMask.");
        }
        if (ok && recipe.contains("targetGuide")) ok = c.source(recipe["targetGuide"], "targetGuide");
        if (ok && recipe.contains("outputs")) {
            const json& outputs = recipe["outputs"];
            if (!outputs.is_object()) ok = c.wrong("outputs", outputs, "an object");
            for (auto it = outputs.begin(); ok && outputs.is_object() && it != outputs.end(); ++it) {
                if (!it.value().is_string()) ok = c.wrong("outputs." + it.key(), it.value(), "a string");
            }
        }
        if (!ok) error = c.error;
        return ok;
    }

    std::string jsonString(const json& object, const char* key, const std::string& fallback) {
        if (!object.is_object()) return fallback;
        const auto it = object.find(key);
        if (it == object.end() || !it->is_string()) return fallback;
        return it->get<std::string>();
    }

    bool jsonBool(const json& object, const char* key, bool fallback) {
        if (!object.is_object()) return fallback;
        const auto it = object.find(key);
        if (it == object.end() || !it->is_boolean()) return fallback;
        return it->get<bool>();
    }

    std::optional<uint64_t> recipeSeed(const json& recipe) {
        if (!recipe.is_object() || !recipe.contains("seed")) return std::nullopt;
        const json& seed = recipe["seed"];
        if (seed.is_number_unsigned()) return seed.get<uint64_t>();
        if (seed.is_number_integer()) {
            const int64_t v = seed.get<int64_t>();
            if (v < 0) return std::nullopt;
            return static_cast<uint64_t>(v);
        }
        if (seed.is_string()) {
            const std::string& text = seed.get_ref<const std::string&>();
            if (text.empty() || text.size() > 20 || !std::all_of(text.begin(), text.end(), [](char ch) { return ch >= '0' && ch <= '9'; })) return std::nullopt;
            if (text.size() == 20 && text > "18446744073709551615") return std::nullopt;
            return std::strtoull(text.c_str(), nullptr, 10);
        }
        return std::nullopt;
    }

}
