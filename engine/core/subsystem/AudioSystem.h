// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef AUDIOSYSTEM_H
#define AUDIOSYSTEM_H

#include "SubSystem.h"

#include "component/AudioSourceComponent.h"

namespace SoLoud{
	class Soloud;
}

namespace jasperengine{

	class JASPERENGINE_API AudioSystem : public SubSystem {

    private:
		static bool inited;

		static void deInit();

		static float globalVolume;

		Vector3 cameraLastPosition;
		bool outputRequested = false;

        bool loadSoundSample(AudioSourceComponent& audio);
        void preloadSoundAssets();

	public:
		AudioSystem(Scene* scene);

		static SoLoud::Soloud& getSoloud();
		static bool init();

		static void stopAll();
		static void pauseAll();
		static void resumeAll();
		static void checkActive();

		static void setGlobalVolume(float volume);
		static float getGlobalVolume();

        bool loadSound(AudioSourceComponent& audio);
		void destroySound(AudioSourceComponent& audio);
		void getLoadCount(size_t& loaded, size_t& total) const;
		void stopSceneSounds();
		bool seekSound(AudioSourceComponent& audio, double time);

		void setPaused(bool paused) override;

		void load() override;
		void draw() override;
		void destroy() override;
		void update(double dt) override;

		void onComponentAdded(Entity entity, ComponentId componentId) override;
		void onComponentRemoved(Entity entity, ComponentId componentId) override;
	};

}

#endif //AUDIOSYSTEM_H