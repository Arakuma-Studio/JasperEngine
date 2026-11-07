// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Sprite Normal Map's entries outside its window (sprite normals Phase 4 spec 4-6): what a
// PNG's jasper.spritenormals recipe says (read once per path, mtime and size), the generated
// output paired with a source image, and the Inspector's buttons: the Normal Texture row's wand
// (click: the window on the row's Base Texture, Assign on; Shift-click: generate or regenerate
// and assign at once) and the Sprite's "Normal Map..." button. Header-only, the
// InspectorVatBake.h pattern; used by the Project window and the Inspector.

#pragma once

#include "EditorHost.h"
#include "Project.h"
#include "component/MeshComponent.h"
#include "external/IconsFontAwesome6.h"
#include "mcp/McpValues.h"
#include "spritenormals/SpriteNormalsRecipe.h"
#include "spritenormals/SpriteNormalsWatcher.h"
#include "util/UIProbe.h"
#include "util/Util.h"

#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>
#include "util/FileUtils.h"

namespace jasperengine::editor::spritenormalsentry {

    namespace fs = std::filesystem;

    // What a PNG's recipe says (generated = it carries a valid jasper.spritenormals recipe)
    struct RecipeHead {
        bool generated = false;
        std::string role;                 // "normal", "atlas", "height"
        std::string source;               // as recorded (project-relative inside a project)
        fs::path sourceAbsolute;
        int painted = 0;                  // its paint layer's pixels (the recipe's "paint", Phase 5)
    };

    inline std::string lowerExtension(const fs::path& path) {
        std::string ext = jasperengine::editor::FileUtils::pathToUtf8(path.extension());
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return ext;
    }

    // The recipe head of a PNG, cached by path + mtime + size (a Project window refresh or a
    // popup frame never reads an unchanged file again)
    inline RecipeHead recipeHead(const fs::path& projectDir, const fs::path& png) {
        struct Entry {
            fs::file_time_type time{};
            uintmax_t size = 0;
            RecipeHead head;
        };
        static std::unordered_map<std::string, Entry> cache;
        if (lowerExtension(png) != ".png") return RecipeHead();
        std::error_code ec;
        const fs::file_time_type time = fs::last_write_time(png, ec);
        if (ec) return RecipeHead();
        const uintmax_t size = fs::file_size(png, ec);
        if (ec) return RecipeHead();
        const std::string key = jasperengine::editor::FileUtils::pathToGenericUtf8(png.lexically_normal());
        auto it = cache.find(key);
        if (it != cache.end() && it->second.time == time && it->second.size == size) return it->second.head;
        Entry entry;
        entry.time = time;
        entry.size = size;
        std::string error;
        const std::optional<nlohmann::json> recipe = spritenormals::readRecipeHead(png, &error);
        if (recipe && spritenormals::validateRecipe(*recipe, error)) {
            entry.head.generated = true;
            entry.head.role = recipe->contains("role") ? (*recipe)["role"].get<std::string>() : std::string("normal");
            entry.head.source = (*recipe)["source"]["path"].get<std::string>();
            if (recipe->contains("paint")) entry.head.painted = (*recipe)["paint"].value("painted", 0);
            const fs::path rel = fs::u8path(entry.head.source);
            entry.head.sourceAbsolute = rel.is_absolute() ? rel.lexically_normal()
                                        : projectDir.empty() ? (png.parent_path() / rel).lexically_normal()
                                                             : (projectDir / rel).lexically_normal();
        }
        if (cache.size() > 20000) cache.clear();
        cache[key] = entry;
        return entry.head;
    }

    inline std::string fileName(const std::string& path) {
        const std::string name = jasperengine::editor::FileUtils::pathToUtf8(fs::u8path(path).filename());
        return name.empty() ? path : name;
    }

    // The generated normal map beside a source image (<stem>_normal.png or <stem>_sheet_normal.png)
    // whose recipe names that image; empty when there is none
    inline fs::path pairedOutput(const fs::path& projectDir, const fs::path& source) {
        const std::string stem = jasperengine::editor::FileUtils::pathToUtf8(source.stem());
        for (const char* suffix : {"_normal.png", "_sheet_normal.png"}) {
            const fs::path candidate = source.parent_path() / fs::u8path(stem + suffix);
            const RecipeHead head = recipeHead(projectDir, candidate);
            if (head.generated && head.sourceAbsolute == source.lexically_normal()) return candidate;
        }
        return fs::path();
    }

    // The Project window's tooltip line for a generated PNG
    inline std::string generatedText(const RecipeHead& head) {
        const std::string source = fileName(head.source);
        if (head.role == "atlas") return "Colour atlas of " + source + " (Sprite Normal Map)";
        if (head.role == "height") return "Height map of " + source + " (Sprite Normal Map)";
        std::string text = "Generated by Sprite Normal Map from " + source + ": regenerate instead of editing";
        if (head.painted > 0) text += "\nPainted: " + std::to_string(head.painted) + " px (kept when it is regenerated)";
        return text;
    }

    inline const char* stoppedReason() {
        return "Stop the scene to generate a normal map: it writes files and changes the scene.";
    }

    // The submesh's Base Texture is an image file (not a framebuffer, camera or fluid link)
    inline bool baseIsImage(Project* project, Scene* scene, Entity entity, unsigned submesh) {
        if (!scene || entity == NULL_ENTITY) return false;
        MeshComponent* mesh = scene->findComponent<MeshComponent>(entity);
        if (!mesh || submesh >= std::max(mesh->numSubmeshes, 1u)) return false;
        const Texture& base = mesh->submeshes[submesh].material.baseColorTexture;
        if (base.getPath().empty() || base.isFramebuffer()) return false;
        const std::string rel = mcp::McpValues::assetPathToJson(project, base.getPath());
        std::error_code ec;
        return Util::isImageFile(rel) && lowerExtension(fs::u8path(rel)) != ".svg" && fs::is_regular_file(project->getProjectPath() / fs::u8path(rel), ec);
    }

    // The Normal Texture row's wand (one square button of `size`), for the first selected entity
    inline void wandButton(Project* project, SceneProject* sceneProject, const std::vector<Entity>& entities, unsigned submesh, const ImVec2& size) {
        if (!sceneProject || entities.empty()) return;
        const Entity entity = entities[0];
        const bool stopped = sceneProject->playState == ScenePlayState::STOPPED;
        // the current normal texture's recipe: Edit / regenerate
        bool hasRecipe = false;
        if (MeshComponent* mesh = sceneProject->scene->findComponent<MeshComponent>(entity)) {
            const std::string stored = mesh->submeshes[submesh].material.normalTexture.getPath();
            if (!stored.empty()) {
                hasRecipe = recipeHead(project->getProjectPath(), project->getProjectPath() / fs::u8path(mcp::McpValues::assetPathToJson(project, stored))).generated;
            }
        }
        ImGui::BeginDisabled(!stopped);
        const bool clicked = ImGui::Button((std::string(ICON_FA_WAND_MAGIC_SPARKLES "##normalMapWand") + std::to_string(submesh)).c_str(), size);
        ImGui::EndDisabled();
        UIProbe::item("inspector.normalMapWand." + std::to_string(submesh));
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            std::string tip = !stopped ? stoppedReason()
                              : hasRecipe ? "Edit or regenerate this normal map (Sprite Normal Map). Shift-click: regenerate and assign."
                                          : "Generate a normal map from the Base Texture (Sprite Normal Map). Shift-click: generate with the last settings and assign.";
            if (stopped && entities.size() > 1) tip += " (first selected GameObject)";
            ImGui::SetTooltip("%s", tip.c_str());
        }
        if (clicked && stopped) getEditorHost().openSpriteNormalsForEntity(sceneProject->id, entity, submesh, ImGui::GetIO().KeyShift);
    }

    // The Sprite Frames section's "Normal Map..." button (half width beside the Slicer Tool)
    inline void normalMapButton(SceneProject* sceneProject, Entity entity, float width) {
        if (!sceneProject) return;
        const bool stopped = sceneProject->playState == ScenePlayState::STOPPED;
        ImGui::BeginDisabled(!stopped);
        const bool clicked = ImGui::Button(ICON_FA_WAND_MAGIC_SPARKLES " Normal Map...", ImVec2(width, 0));
        ImGui::EndDisabled();
        UIProbe::item("inspector.spriteNormalMap");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("%s", stopped ? "Generate a normal map for this sprite's texture with its frames" : stoppedReason());
        }
        if (clicked && stopped) getEditorHost().openSpriteNormalsForEntity(sceneProject->id, entity, 0, false);
    }

    // Under the Sprite's buttons: one subtle line when the scene's Light 2Ds would light this
    // sprite in depth but it has no normal map yet
    inline void normalMapHint(SceneProject* sceneProject, Entity entity) {
        if (!sceneProject || !sceneProject->scene) return;
        MeshComponent* mesh = sceneProject->scene->findComponent<MeshComponent>(entity);
        if (!mesh || !mesh->submeshes[0].material.normalTexture.getPath().empty()) return;
        if (mesh->submeshes[0].material.baseColorTexture.getPath().empty()) return;
        if (spritenormals::lightCheck(sceneProject->scene).lightsWithHeight == 0) return;
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
        ImGui::TextWrapped(ICON_FA_LIGHTBULB " Light 2Ds light this sprite flat: Normal Map... gives it depth.");
        ImGui::PopStyleColor();
        UIProbe::item("inspector.spriteNormalMapHint");
    }

}
