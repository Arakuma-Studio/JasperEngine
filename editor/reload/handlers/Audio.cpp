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
        ctx.service.startJob("audio", path, [filename]() -> HotReloadService::AdoptFn {
            std::shared_ptr<SoLoud::Wav> sound = SoundPool::decode(filename, filename);
            return [filename, sound](std::string&) {
                SoundPool::replace(filename, sound);
                return true;
            };
        });
    }
}
