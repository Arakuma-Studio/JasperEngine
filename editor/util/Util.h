// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <cctype>
#include <cstring>
#include <vector>
#include <string>
#include <string_view>
#include <algorithm>
#include <filesystem>
#include <unordered_set>
#include "imgui.h"
#include "shader/ShaderVariantCompiler.h"

namespace jasperengine::editor{
    class Util{

    private:
        static std::string fileExtension(const std::string& path) {
            std::string ext = std::filesystem::path(path).extension().string();
            if (ext.empty() && !path.empty() && path[0] == '.') {
                ext = path;
            }
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            return ext;
        }

    public:

        inline static bool isImageFile(const std::string& path) {
            static const std::unordered_set<std::string> imageExtensions = {
                ".png", ".jpg", ".jpeg", ".bmp", ".tga", ".gif", ".hdr", ".psd", ".pic", ".pnm", ".svg"
            };

            const std::string ext = fileExtension(path);
            return imageExtensions.find(ext) != imageExtensions.end();
        }

        inline static bool isFontFile(const std::string& path) {
             static const std::unordered_set<std::string> fontExtensions = {
                ".ttf", ".otf"
            };

            const std::string ext = fileExtension(path);
            return fontExtensions.find(ext) != fontExtensions.end();
        }

        inline static bool isSceneFile(const std::string& path) {
            return fileExtension(path) == ".scene";
        }

        inline static bool isMaterialFile(const std::string& path) {
            return fileExtension(path) == ".material";
        }

        inline static bool isShaderFile(const std::string& path) {
             static const std::unordered_set<std::string> shaderExtensions = {
                ".vert", ".frag", ".glsl"
            };

            const std::string ext = fileExtension(path);
            return shaderExtensions.find(ext) != shaderExtensions.end();
        }

        // A forked custom shader is stored in a component's customShader field as either:
        //   - a shared base path with no extension (vert = base+".vert", frag = base+".frag"), or
        //   - two explicit project-relative paths joined by '|' ("vert|frag") when the entry
        //     points live in differently-named files.
        // All stored paths are project-relative.
        static constexpr char customShaderSeparator = '|';

        struct CustomShaderPaths {
            std::string vert;  // project-relative, with extension (empty if customShader empty)
            std::string frag;
        };

        // Resolves a customShader value into its concrete .vert/.frag project-relative paths.
        // The engine resolves them the same way when it compiles a fork at runtime.
        inline static CustomShaderPaths resolveCustomShaderPaths(const std::string& customShader) {
            CustomShaderPaths paths;
            ShaderVariantCompiler::getCustomShaderPaths(customShader, paths.vert, paths.frag);
            return paths;
        }

        inline static bool isSeparateCustomShader(const std::string& customShader) {
            return customShader.find(customShaderSeparator) != std::string::npos;
        }

        // True for a relative path that cannot escape the directory it is resolved
        // against. Guards every project-relative path that reaches the filesystem.
        inline static bool isSafeRelativePath(const std::filesystem::path& path) {
            if (path.empty() || path.is_absolute())
                return false;

            std::filesystem::path normalized = path.lexically_normal();
            return !normalized.empty() && *normalized.begin() != "..";
        }

        // True when path is root itself or sits below it. Lexical: it runs per folder
        // per frame in the creation dialogs.
        inline static bool isInsidePath(const std::filesystem::path& path, const std::filesystem::path& root) {
            std::filesystem::path relative = path.lexically_normal().lexically_relative(root.lexically_normal());
            return !relative.empty() && *relative.begin() != "..";
        }

        // Builds a customShader value from two project-relative entry-point paths (with
        // extensions). Collapses to the shared-base shorthand when they are <base>.vert and
        // <base>.frag of the same base; otherwise stores the explicit "vert|frag" form.
        inline static std::string makeCustomShader(const std::string& vertPath, const std::string& fragPath) {
            if (vertPath.empty() || fragPath.empty())
                return "";

            std::filesystem::path vp(vertPath);
            std::filesystem::path fp(fragPath);
            std::string vBase = (vp.parent_path() / vp.stem()).generic_string();
            std::string fBase = (fp.parent_path() / fp.stem()).generic_string();
            if (vBase == fBase && vp.extension() == ".vert" && fp.extension() == ".frag")
                return vBase;

            return vertPath + customShaderSeparator + fragPath;
        }

        inline static bool isPrefabFile(const std::string& path) {
            return fileExtension(path) == ".prefab";
        }

        inline static bool isModelFile(const std::string& path) {
             static const std::unordered_set<std::string> modelExtensions = {
                ".gltf", ".glb", ".obj", ".fbx"
            };

            const std::string ext = fileExtension(path);
            return modelExtensions.find(ext) != modelExtensions.end();
        }

        inline static bool isAudioFile(const std::string& path) {
             static const std::unordered_set<std::string> audioExtensions = {
                ".wav", ".ogg", ".mp3", ".flac"
            };

            const std::string ext = fileExtension(path);
            return audioExtensions.find(ext) != audioExtensions.end();
        }

        // HTML UI documents (UIDocumentComponent::filename); .rml is RmlUi's own markup
        inline static bool isHtmlFile(const std::string& path) {
             static const std::unordered_set<std::string> htmlExtensions = {
                ".html", ".htm", ".rml"
            };

            const std::string ext = fileExtension(path);
            return htmlExtensions.find(ext) != htmlExtensions.end();
        }

        // HTML UI stylesheets; .rcss is RmlUi's own
        inline static bool isCssFile(const std::string& path) {
             static const std::unordered_set<std::string> cssExtensions = {
                ".css", ".rcss"
            };

            const std::string ext = fileExtension(path);
            return cssExtensions.find(ext) != cssExtensions.end();
        }

        // colour LUTs a scene's post-processing grades with besides image strips (.cube)
        inline static bool isColorLutFile(const std::string& path) {
            return fileExtension(path) == ".cube";
        }

        // particle effects (S20)
        inline static bool isParticleEffectFile(const std::string& path) {
            return fileExtension(path) == ".particles";
        }

        // Pose animations (PoseAnimatorComponent::animation)
        inline static bool isPoseAnimationFile(const std::string& path) {
            return fileExtension(path) == ".poseanim";
        }

        // Animation clips (AnimationPlayerComponent clips, the Animation window); a model
        // sub-clip reference ("hero.gltf#Walk") is not a file and is not one
        inline static bool isAnimationClipFile(const std::string& path) {
            return fileExtension(path) == ".anim";
        }

        // Sound Events (S37)
        inline static bool isSoundEventFile(const std::string& path) {
            return fileExtension(path) == ".soundevent";
        }

        // The Animator's assets (S21): controllers, avatar masks, montages
        inline static bool isAnimatorControllerFile(const std::string& path) {
            return fileExtension(path) == ".controller";
        }
        inline static bool isAvatarMaskFile(const std::string& path) {
            return fileExtension(path) == ".mask";
        }
        inline static bool isMontageFile(const std::string& path) {
            return fileExtension(path) == ".montage";
        }
        inline static bool isAnimatorAssetFile(const std::string& path) {
            return isAnimatorControllerFile(path) || isAvatarMaskFile(path) || isMontageFile(path);
        }
        // Vertex Animation assets (VertexAnimatorComponent::animation); their generated mesh is
        // "<name>.vat.glb", a model file
        inline static bool isVertexAnimationFile(const std::string& path) {
            return fileExtension(path) == ".vat";
        }

        // the GUID-tracked JSON assets of the animation system: clips, Vertex Animations, Avatar
        // Masks, Animator Controllers (their references follow by GUID: remapClipFilePath)
        inline static bool isGuidJsonAssetFile(const std::string& path) {
            return isAnimationClipFile(path) || isVertexAnimationFile(path) || isAvatarMaskFile(path) || isAnimatorControllerFile(path);
        }

        // (upstream's isAvatarMaskFile / isAnimatorControllerFile: S21's above, the same extensions)

        // Shake Presets (CameraShakerComponent::startPreset, CameraShake.shake("x.shake"))
        inline static bool isShakePresetFile(const std::string& path) {
            return fileExtension(path) == ".shake";
        }

        // Files referenced relative to the assets directory
        inline static bool isAssetFile(const std::string& path) {
            return isImageFile(path) || isModelFile(path) || isFontFile(path) || isAudioFile(path) ||
                   isHtmlFile(path) || isCssFile(path) || isColorLutFile(path) || isParticleEffectFile(path) || isPoseAnimationFile(path) ||
                   isAnimationClipFile(path) || isAnimatorAssetFile(path) ||
                   isShakePresetFile(path);
        }

        inline static bool isScriptFile(const std::string& path) {
             static const std::unordered_set<std::string> scriptExtensions = {
                ".lua", ".cpp", ".cc", ".cxx", ".h", ".hh", ".hpp", ".hxx"
            };

            const std::string ext = fileExtension(path);
            return scriptExtensions.find(ext) != scriptExtensions.end();
        }

        inline static bool isLuaFile(const std::string& path) {
            return fileExtension(path) == ".lua";
        }

        inline static bool isHeaderFile(const std::string& path) {
             static const std::unordered_set<std::string> headerExtensions = {
                ".h", ".hh", ".hpp", ".hxx"
            };

            const std::string ext = fileExtension(path);
            return headerExtensions.find(ext) != headerExtensions.end();
        }

        inline static bool isSourceFile(const std::string& path) {
             static const std::unordered_set<std::string> sourceExtensions = {
                ".cpp", ".cc", ".cxx"
            };

            const std::string ext = fileExtension(path);
            return sourceExtensions.find(ext) != sourceExtensions.end();
        }

        // String-list drag payloads ("external_files", "resource_files") are
        // strings packed back-to-back, each terminated by '\0'. Encoder and
        // decoder live together so every producer/consumer shares one format.
        inline static std::vector<char> encodeStringsPayload(const std::vector<std::string>& strings){
            std::vector<char> payload;
            for (const std::string& value : strings) {
                payload.insert(payload.end(), value.begin(), value.end());
                payload.push_back('\0');
            }
            return payload;
        }

        inline static std::vector<std::string> getStringsFromPayload(const ImGuiPayload* payload){
            std::vector<std::string> receivedStrings;
            if (!payload || !payload->Data || payload->DataSize <= 0) {
                return receivedStrings;
            }

            const char* current = static_cast<const char*>(payload->Data);
            const char* end = current + payload->DataSize;
            while (current < end) {
                // Bounded scan: never run past DataSize even if the final
                // string is missing its terminator.
                const void* terminator = std::memchr(current, '\0', static_cast<size_t>(end - current));
                const char* valueEnd = terminator ? static_cast<const char*>(terminator) : end;
                if (valueEnd != current) {
                    receivedStrings.emplace_back(current, valueEnd);
                }
                current = valueEnd + 1;
            }

            return receivedStrings;
        }
    };
}
