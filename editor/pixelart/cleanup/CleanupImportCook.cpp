// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "pixelart/cleanup/CleanupImportCook.h"

#include "command/type/SetAssetMetaCmd.h"
#include "pixelart/PixelImage.h"
#include "pixelart/cleanup/CleanupRecipe.h"
#include "pixelart/cleanup/CleanupSession.h"

#include "yaml-cpp/yaml.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace jasperengine::editor::pixelart::cleanup {

    namespace {

        bool isAsepriteSource(const fs::path& path) {
            std::string ext = path.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return ext == ".aseprite" || ext == ".ase";
        }

        // nlohmann::json from a YAML scalar, map or sequence (the recipe as the .meta holds it)
        nlohmann::json yamlToJsonValue(const YAML::Node& node) {
            if (node.IsMap()) {
                nlohmann::json object = nlohmann::json::object();
                for (const auto& entry : node) object[entry.first.as<std::string>()] = yamlToJsonValue(entry.second);
                return object;
            }
            if (node.IsSequence()) {
                nlohmann::json array = nlohmann::json::array();
                for (const YAML::Node& item : node) array.push_back(yamlToJsonValue(item));
                return array;
            }
            if (node.IsNull() || !node.IsDefined()) return nullptr;
            const std::string text = node.as<std::string>("");
            if (node.Tag() == "!") return text;   // a quoted string
            if (text == "true" || text == "false") return text == "true";
            char* end = nullptr;
            const double number = std::strtod(text.c_str(), &end);
            if (end && *end == '\0' && !text.empty()) {
                const long long integer = std::strtoll(text.c_str(), &end, 10);
                if (end && *end == '\0') return integer;
                return number;
            }
            return text;
        }

    }

    std::string importSectionRecipeText(const YAML::Node& section) {
        if (!section || !section.IsMap()) return std::string();
        nlohmann::json recipe = nlohmann::json::object();
        if (const YAML::Node stored = section["recipe"]) {
            if (stored.IsScalar()) {
                recipe = nlohmann::json::parse(stored.as<std::string>(""), nullptr, false);
                if (recipe.is_discarded()) return stored.as<std::string>("");
            } else {
                recipe = yamlToJsonValue(stored);
            }
        }
        // canonical, so an equivalent edit of the .meta keeps the cache key; a recipe that does
        // not decode goes to the step as it is and fails there, with its error shown
        Recipe decoded;
        std::vector<PixelProblem> problems;
        if (!decodeRecipe(recipe, decoded, problems)) return recipe.dump();
        return recipeText(decoded);
    }

    std::string importStepRecipeText(const fs::path& absoluteAsset) {
        if (isAsepriteSource(absoluteAsset)) return std::string();
        const YAML::Node section = SetAssetMetaCmd::read(absoluteAsset, "import.pixelCleanup");
        if (!section || !section.IsMap()) return std::string();
        if (section["enabled"] && !section["enabled"].as<bool>(false)) return std::string();
        return importSectionRecipeText(section);
    }

    namespace {

        std::atomic<uint64_t> importCookFixes{0};

        // The queue plans a texture and cooks it in two stages, each with its own request, and both
        // need the step's pixels. The run is deterministic (R40), so the plan's output serves the
        // cook; taking it frees it. A few slots: textures cook side by side.
        struct CookMemo {
            std::string key;
            TextureCookStep::Outcome outcome;
        };
        constexpr size_t kCookMemoSlots = 8;
        std::mutex cookMemoMutex;
        std::deque<CookMemo> cookMemo;

        TextureCookStep::Outcome runCookFix(const std::string& recipeText, const uint8_t* rgba, int width, int height, const std::string& sourceSha256) {
            TextureCookStep::Outcome outcome;
            PixelImage source(width, height);
            std::memcpy(source.editPixels().data(), rgba, static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
            PixelImage out;
            Verdict verdict = Verdict::NeedsPerson;
            std::string guard, error;
            if (!runImportStep(source, recipeText, out, verdict, guard, error)) {
                outcome.error = error;
                return outcome;
            }
            outcome.verdict = verdictName(verdict);
            // R33: needs_person on a source other than the one the recipe was saved from
            Recipe recorded;
            std::vector<PixelProblem> problems;
            const bool sameSource = decodeRecipeText(recipeText, recorded, problems) && !recorded.source.sha256.empty() &&
                                    recorded.source.sha256 == sourceSha256;
            if (verdict == Verdict::NeedsPerson && !sameSource) {
                outcome.guard = guard.empty() ? std::string("needs_person") : guard;
                return outcome;
            }
            if (out.empty()) {
                outcome.guard = guard.empty() ? std::string("no_grid") : guard;
                return outcome;
            }
            outcome.applied = true;
            outcome.width = out.width();
            outcome.height = out.height();
            outcome.rgba.assign(out.pixels().begin(), out.pixels().end());
            return outcome;
        }

        bool takeCookMemo(const std::string& key, TextureCookStep::Outcome& outcome) {
            std::lock_guard<std::mutex> lock(cookMemoMutex);
            for (auto it = cookMemo.begin(); it != cookMemo.end(); ++it) {
                if (it->key == key) {
                    outcome = std::move(it->outcome);
                    cookMemo.erase(it);
                    return true;
                }
            }
            return false;
        }

        void putCookMemo(const std::string& key, const TextureCookStep::Outcome& outcome) {
            std::lock_guard<std::mutex> lock(cookMemoMutex);
            cookMemo.push_back({key, outcome});
            if (cookMemo.size() > kCookMemoSlots) cookMemo.pop_front();
        }

    }

    uint64_t importCookFixesRun() {
        return importCookFixes.load();
    }

    TextureCookStep importCookStep(const std::string& recipeText) {
        TextureCookStep step;
        if (recipeText.empty()) return step;
        step.key = recipeText + "\npipeline " + std::to_string(Recipe::kPipelineVersion);
        step.run = [recipeText](const uint8_t* rgba, int width, int height, const std::string& sourceSha256) {
            const std::string memoKey = recipeText + "\n" + sourceSha256 + "\n" + std::to_string(width) + "x" + std::to_string(height);
            TextureCookStep::Outcome outcome;
            if (takeCookMemo(memoKey, outcome)) return outcome;
            importCookFixes++;
            outcome = runCookFix(recipeText, rgba, width, height, sourceSha256);
            putCookMemo(memoKey, outcome);
            return outcome;
        };
        return step;
    }

}
