// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The recipe of a generated sprite normal map: every PNG the generator writes (the normal map,
// the colour atlas of an animation, the height map) carries how it was made as canonical JSON in
// an iTXt chunk with the keyword jasper.spritenormals, right after IHDR (contract section 6). It
// lets the editor regenerate the map, tell that its source changed since (staleness) and show
// what made it. Phase 5 adds a second chunk, jasper.spritenormals.paint.

#include "SpriteMedia.h"
#include "SpriteNormals.h"

#include "json.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace jasperengine::editor::spritenormals {

    inline constexpr const char* kRecipeKeyword = "jasper.spritenormals";
    inline constexpr const char* kRecipeFormat = "jasperengine.spritenormals";
    inline constexpr int kRecipeVersion = 1;

    // Every setting of the contract (resolved: what generate() used), snake_case keys; floats
    // rounded to 6 decimals; radius "auto" or a number; colours "#rrggbb"
    nlohmann::json settingsToJson(const Settings& settings);
    // The reverse; unknown keys ignored, missing keys keep `out`'s values; false with `error`
    // naming the key on a wrong type or an unknown enum value (values are not range-checked
    // here: validateSettings does that)
    bool settingsFromJson(const nlohmann::json& json, Settings& out, std::string& error);

    // How the frames were found (recipe "frames.from")
    enum class FramesFrom { Single, Sprite, Gif, Apng, Given };
    const char* toString(FramesFrom from);   // "single", "sprite", "gif", "apng", "given"
    bool parseFramesFrom(const std::string& text, FramesFrom& out);

    struct RecipeInput {
        std::string role;                    // "normal", "atlas", "height"
        std::string sourcePath;              // project-relative ('/') inside the project, else relative to the
                                             // directory of the PNG carrying the recipe ('/')
        std::string sourceSha1;
        SourceKind sourceKind = SourceKind::Image;
        bool fromSprite = false;             // the source was a Sprite GameObject's texture
        std::string spriteEntity;            // its hierarchy path (fromSprite)
        unsigned spriteSubmesh = 0;          // its submesh (fromSprite; source.submesh when not 0)
        FramesFrom framesFrom = FramesFrom::Single;
        std::vector<Cell> cells;
        int columns = 0;                     // animations (0 otherwise)
        int rows = 0;
        std::vector<Delay> delays;
        int loopCount = 0;
        Settings settings;
        const Result* result = nullptr;      // radius, outline, height range
        std::string normalOutput;            // project-relative paths of the set written together
        std::string atlasOutput;
        std::string heightOutput;
    };

    // The canonical recipe (contract section 6 + section 6.2 of the Phase 2 spec)
    nlohmann::json makeRecipe(const RecipeInput& input);
    // Compact canonical text (sorted keys, UTF-8 checked): false with `error` naming the path
    // that is not valid UTF-8
    bool recipeText(const nlohmann::json& recipe, std::string& text, std::string& error);

    // Every type the readers rely on; false with `error` ("settings.strength is a string")
    bool validateRecipe(const nlohmann::json& recipe, std::string& error);

    // PNG bytes of `image` (SpriteMedia::encodePng) with the recipe chunk spliced in
    bool encodeWithRecipe(const Image& image, const nlohmann::json& recipe, std::vector<uint8_t>& png, std::string& error);
    // The jasper.spritenormals recipe of PNG bytes / a file; nullopt when there is none (error
    // empty) or it is garbled (error says why). readRecipeHead reads only the chunks before IDAT.
    std::optional<nlohmann::json> readRecipe(const uint8_t* bytes, size_t size, std::string* error = nullptr);
    std::optional<nlohmann::json> readRecipe(const std::filesystem::path& png, std::string* error = nullptr);
    std::optional<nlohmann::json> readRecipeHead(const std::filesystem::path& png, std::string* error = nullptr);
}
