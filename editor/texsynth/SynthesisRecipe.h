// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The "recipe" of a synthesized texture: every PNG the synthesis writes carries how it was made
// as compact JSON in an iTXt chunk with the keyword `jasper.synthesis`, placed right after IHDR.
// No sidecar file: it survives copies and renames, and costs nothing at runtime (stb_image and
// every other decoder skip unknown ancillary chunks). The source files' SHA-1s let a later check
// tell that a source changed since the texture was synthesized (checkRecipeSources).
//
// Schema (format 1):
//   format        1
//   generator     "jasperengine-editor texsynth"
//   engine        the editor version (JASPERENGINE_EDITOR_VERSION)
//   role          "primary", a companion's name, or "uncertainty": which output this file is
//   mode          "extend" | "tileable" | "make_tileable" | "inpaint" | "guided"
//   quality       "draft" | "balanced" | "high" (the preset the params started from)
//   params        the resolved parameters: nearestNeighbors, randomSampleLocations,
//                 cauchyDispersion, backtrackPercent, backtrackStages, guideAlpha, guideDecay,
//                 borderFraction
//   seed          the seed (a JSON number; seeds above 2^53 are written as a decimal string)
//   size          [width, height]
//   pathBase      "project": the paths are relative to the project directory (absolute when a
//                 file is outside it); "file": relative to the directory of the PNG itself
//   examples      [{path, sha1, sampleable, sampleMask?: {path, sha1}, guide?: {path, sha1}}]
//   companions    [{name, sources: [{path, sha1}] (one per example)}]
//   inpaintMask   {path, sha1, channel, invert} | {source: "alpha", invert} (inpaint only)
//   targetGuide   {path, sha1} (guided only)
//   outputs       {role: path} of every file written together
// A source given in memory has no path and "pixelsSha1" (SHA-1 of its decoded samples) instead.
//
// Paths are JSON text, so UTF-8: a file name that is not valid UTF-8 (raw Latin-1 bytes on Linux)
// can't be recorded losslessly. Every writer refuses such a path up front with a message saying to
// rename the file (SynthesisCli, checkSpec), and recipeText() reports it instead of throwing.
// Readers never assume the types above: a garbled recipe (another writer, a hand edit) is
// reported by validateRecipe() and the accessors below fall back on a wrong type.

#include "TextureSynthesis.h"

#include "json.hpp"

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace jasperengine::editor::texsynth {

    inline constexpr const char* kRecipeKeyword = "jasper.synthesis";
    inline constexpr int kRecipeFormat = 1;

    // Where the recipe's paths are relative to: the project directory when there is one, else the
    // directory of the written PNG
    struct RecipePaths {
        std::filesystem::path projectDir;
        std::filesystem::path outputDir;
        std::map<std::string, std::filesystem::path> outputs;   // role -> absolute path
    };

    // ---- text

    // True when `text` is well-formed UTF-8 (what a JSON string holds losslessly)
    bool isValidUtf8(const std::string& text);
    // `text` for messages: every byte that is not part of well-formed UTF-8 written as \xNN
    std::string printableUtf8(const std::string& text);
    // A JSON value as compact text without ever throwing: false with `error` naming the string
    // that is not valid UTF-8 (a dump with nlohmann's strict handler would throw type_error.316)
    bool jsonText(const nlohmann::json& value, std::string& text, std::string& error);
    // The same for diagnostics output where lossy is fine: invalid bytes become U+FFFD
    std::string jsonTextLossy(const nlohmann::json& value, int indent = -1);

    // ---- writing

    // The resolved parameters as JSON (floats rounded to 6 decimals: 0.8, not 0.800000011920929)
    nlohmann::json paramsToJson(const Params& params);

    // How a path is recorded in a recipe (relative to paths.projectDir, else to paths.outputDir,
    // else absolute; '/' separators): what must be valid UTF-8
    std::string recipePath(const std::filesystem::path& path, const RecipePaths& paths);

    // The recipe of one output file (`role`) of a finished synthesis
    nlohmann::json makeRecipe(const Request& request, const Result& result, const std::string& role, const RecipePaths& paths);
    // jsonText() of a recipe, the error saying which path to rename
    bool recipeText(const nlohmann::json& recipe, std::string& text, std::string& error);

    // ---- reading

    // Every type the readers rely on (format 1; unknown keys are ignored): false with `error`
    // ("examples[0].path is a number, expected a string") when the recipe is garbled
    bool validateRecipe(const nlohmann::json& recipe, std::string& error);
    // Type-checked reads: `fallback` when `object` is not an object, lacks `key` or holds another type
    std::string jsonString(const nlohmann::json& object, const char* key, const std::string& fallback = std::string());
    bool jsonBool(const nlohmann::json& object, const char* key, bool fallback);
    // The recipe's seed (a JSON number, or a decimal string above 2^53); nullopt when missing or garbled
    std::optional<uint64_t> recipeSeed(const nlohmann::json& recipe);

    // PNG bytes with an iTXt chunk (keyword jasper.synthesis, uncompressed UTF-8 `text`) spliced
    // in right after IHDR; an existing jasper.synthesis chunk is dropped. False (with `error`)
    // when `png` is not a PNG.
    bool insertRecipeChunk(const std::vector<uint8_t>& png, const std::string& text, std::vector<uint8_t>& out, std::string& error);

    // Encodes `image` as PNG (8- or 16-bit, its channel count), embeds `recipe` (dumped compact)
    // and writes it to `path` (through a temporary file in the same directory, then renamed).
    bool writePngWithRecipe(const std::filesystem::path& path, const Image& image, const nlohmann::json& recipe, std::string& error);
    // `image` as PNG bytes with `recipe` embedded (no file written)
    bool encodePngWithRecipe(const Image& image, const nlohmann::json& recipe, std::vector<uint8_t>& out, std::string& error);

    // Writes every file or none: each is staged next to its target as <name>.partial-<n>
    // (missing directories created), then each existing target is moved aside and the staged
    // file renamed into place. Any failure puts back what was there and removes what was
    // created; `failedPath` and `error` say which file failed and why. The CLI's writer (the
    // editor writes through its undoable WriteFilesCmd, all or nothing as well).
    struct StagedFile {
        std::filesystem::path path;
        std::vector<uint8_t> bytes;
    };
    bool writeFilesAllOrNothing(const std::vector<StagedFile>& files, std::string& failedPath, std::string& error);

    // The jasper.synthesis text of PNG bytes / a PNG file (iTXt, or a tEXt with that keyword).
    // nullopt when there is none, or the file is not a PNG, truncated, or the chunk is garbled
    // (CRC mismatch, compressed, malformed); `error` says which (empty for "no recipe").
    std::optional<std::string> readRecipeTextFromPng(const uint8_t* bytes, size_t size, std::string* error = nullptr);
    std::optional<std::string> readRecipeText(const std::filesystem::path& path, std::string* error = nullptr);
    // readRecipeText parsed: nullopt also when the text is not a JSON object
    std::optional<nlohmann::json> readRecipe(const std::filesystem::path& path, std::string* error = nullptr);
    // Fast form for scanning many files: reads only the chunks before the first IDAT (where
    // writePngWithRecipe puts the recipe), seeking over the others; a recipe placed after the
    // image data is not seen. nullopt for no recipe, not a PNG or a garbled chunk (`error`).
    std::optional<std::string> readRecipeTextHead(const std::filesystem::path& path, std::string* error = nullptr);

    // Each source a recipe names, hashed again
    struct SourceState {
        std::string role;                 // "examples[0]", "examples[0].sampleMask", "companions.normal[0]", ...
        std::string path;                 // as recorded
        std::filesystem::path resolved;   // where it was looked for
        std::string state;                // "unchanged", "changed", "missing", "unknown" (no path recorded)
    };
    // `recipeFile` is the PNG the recipe came from (pathBase "file"), `projectDir` the project
    // (pathBase "project"). `hashFile` (optional) gives a file's SHA-1, "" when it can't be read
    // (a caching hasher for scans); the default reads and hashes the file.
    std::vector<SourceState> checkRecipeSources(const nlohmann::json& recipe, const std::filesystem::path& recipeFile,
                                                const std::filesystem::path& projectDir,
                                                const std::function<std::string(const std::filesystem::path&)>& hashFile = nullptr);

}
