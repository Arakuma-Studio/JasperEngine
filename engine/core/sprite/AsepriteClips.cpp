// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AsepriteClips.h"

#include "animation/AnimationClipIO.h"
#include "asset/ImportedAssets.h"
#include "asset/SubAssetKeys.h"
#include "io/Data.h"
#include "sprite/SpriteSheetPool.h"

#include <charconv>
#include <cctype>
#include <filesystem>

namespace jasperengine::anim{

    namespace{

        struct AsepriteClipEntry{
            std::string name;
            std::string key;        // the ImportedAssets key part: "animation:walk", "animation#0"
            float duration = 0.0f;
        };

        // the clip names and keys the import gives (AsepriteImport: tags keyed over every tag,
        // the untagged clip "animation#0" named after the file)
        bool entriesOf(const std::string& path, std::vector<AsepriteClipEntry>& out, std::string* error){
            out.clear();
            std::shared_ptr<const SpriteSheetData> sheet = SpriteSheetPool::get(path);
            if (!sheet){
                if (error) *error = "no import of " + path + " yet";
                return false;
            }
            auto duration = [&](const std::vector<int>& frames){
                double ms = 0;
                for (int f : frames){
                    if (f >= 0 && f < (int)sheet->frames.size()) ms += sheet->frames[(size_t)f].durationMs;
                }
                return (float)(ms / 1000.0);
            };
            const std::string source = ImportedAssets::normalize(path);
            if (sheet->tags.empty()){
                std::vector<int> all;
                for (int f = 0; f < (int)sheet->frames.size(); f++) all.push_back(f);
                out.push_back({std::filesystem::path(path).stem().string(), subAssetIndexKey(SubAssetKind::Animation, 0), duration(all)});
            }else{
                std::vector<std::string> names;
                for (const SpriteTag& tag : sheet->tags) names.push_back(tag.name);
                const std::vector<std::string> keys = subAssetKeys(SubAssetKind::Animation, names);
                for (size_t i = 0; i < sheet->tags.size(); i++){
                    const SpriteTag& tag = sheet->tags[i];
                    // tags the settings leave out have no clip
                    std::string file;
                    if (!ImportedAssets::find(ImportedAssets::reference(source, keys[i]), file)) continue;
                    out.push_back({tag.name, keys[i], duration(tag.repeat == 0 ? tag.sequence() : tag.playback())});
                }
            }
            return true;
        }

        bool isNumber(const std::string& text){
            if (text.empty()) return false;
            for (char c : text){
                if (!std::isdigit((unsigned char)c)) return false;
            }
            return true;
        }

    }

    bool isAsepriteClipContainer(const std::string& path){
        return SpriteSheetPool::isAsepritePath(path);
    }

    bool listAsepriteClips(const std::string& path, std::vector<ModelClipInfo>& out, std::string* error){
        out.clear();
        std::vector<AsepriteClipEntry> entries;
        if (!entriesOf(path, entries, error)) return false;
        for (size_t i = 0; i < entries.size(); i++){
            ModelClipInfo info;
            info.index = (int)i;
            info.name = entries[i].name;
            info.duration = entries[i].duration;
            info.channels = 1;
            out.push_back(info);
        }
        return true;
    }

    bool buildAsepriteClip(const std::string& path, const std::string& clip, ClipData& out,
                           std::vector<ClipWarning>& warnings, std::string* error){
        out = ClipData();
        std::vector<AsepriteClipEntry> entries;
        if (!entriesOf(path, entries, error)) return false;
        const AsepriteClipEntry* found = nullptr;
        for (const AsepriteClipEntry& e : entries){
            if (e.name == clip && !found) found = &e;
        }
        if (!found && isNumber(clip)){
            size_t index = 0;
            // (from_chars: a long run of digits fails instead of throwing)
            const auto parsed = std::from_chars(clip.data(), clip.data() + clip.size(), index);
            if (parsed.ec == std::errc() && index < entries.size()) found = &entries[index];
        }
        if (!found){
            if (error) *error = "no clip \"" + clip + "\" in " + path;
            return false;
        }
        std::string file;
        if (!ImportedAssets::find(ImportedAssets::reference(ImportedAssets::normalize(path), found->key), file)){
            if (error) *error = "the clip \"" + clip + "\" of " + path + " is not imported";
            return false;
        }
        Data data;
        if (data.open(file.c_str()) != FileErrors::FILEDATA_OK){
            if (error) *error = "cannot read " + file;
            return false;
        }
        ClipParseResult parsed = parseClip(std::string(reinterpret_cast<const char*>(data.getMemPtr()), data.length()));
        if (!parsed.ok()){
            if (error) *error = file + ": " + parsed.error.toString();
            return false;
        }
        warnings.insert(warnings.end(), parsed.warnings.begin(), parsed.warnings.end());
        out = std::move(*parsed.data);
        return true;
    }

}
