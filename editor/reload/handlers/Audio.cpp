// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Audio (S05 R11): a new sample decoded on a worker replaces the SoundPool entry. Voices
// playing the old one finish on it (their sources hold it); the next plays take the new one.

#include "ReloadHandlers.h"

#include "Project.h"
#include "component/AudioSourceComponent.h"
#include "pool/SoundPool.h"

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;

void reload::reloadAudio(Context& ctx, const std::vector<fs::path>& files) {
    Project* project = &ctx.project;
    for (const fs::path& path : files) {
        std::string filename;
        for (SceneProject* sceneProject : project->getScenesAndPrefabStages()) {
            Scene* scene = sceneProject->scene;
            if (!scene || !scene->isComponentRegistered<AudioSourceComponent>()) continue;
            auto sources = scene->getComponentArray<AudioSourceComponent>();
            for (size_t i = 0; i < sources->size() && filename.empty(); i++) {
                const AudioSourceComponent& audio = sources->getComponentFromIndex(i);
                if (!audio.filename.empty() && project->resolveAssetPath(audio.filename).lexically_normal() == path) filename = audio.filename;
            }
        }
        if (filename.empty()) continue;
        // every load type the file is cached with ("<file>#<type>"): decoded again as it was
        std::vector<std::string> ids = SoundPool::idsOfFile(filename);
        if (ids.empty()) ids.push_back(SoundPool::key(filename, SoundPool::resolveLoadType(filename, AudioLoadType::Auto)));
        ctx.service.startJob("audio", path, [filename, ids]() -> HotReloadService::AdoptFn {
            std::vector<std::shared_ptr<SoLoud::AudioSource>> sounds;
            for (const std::string& id : ids) sounds.push_back(SoundPool::decode(id, filename));
            return [ids, sounds](std::string&) {
                for (size_t i = 0; i < ids.size(); i++) SoundPool::replace(ids[i], sounds[i]);
                return true;
            };
        });
    }
}
