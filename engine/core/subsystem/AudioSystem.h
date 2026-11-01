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
		// set when opening the output failed: it is not tried again
		static bool outputUnavailable;
		// the Recorder's capture holds SoLoud on its null driver
		static bool offlineCapture;

		static void deInit();

		static float globalVolume;

		Vector3 cameraLastPosition;
		bool outputRequested = false;
		// off while the scheduler skips the system (Systems window): its sounds hold too
		bool schedulerEnabled = true;

		// pauses or resumes the scene's playing sounds
		void applySoundPause(bool pause);

        bool loadSoundSample(AudioSourceComponent& audio);
        void preloadSoundAssets();

	public:
		AudioSystem(Scene* scene);

		static SoLoud::Soloud& getSoloud();
		static bool init();
		// false once opening the audio output failed (no device): sounds are muted
		static bool isOutputAvailable();

		static void stopAll();
		static void pauseAll();
		static void resumeAll();
		static void checkActive();

		static void setGlobalVolume(float volume);
		static float getGlobalVolume();

		// Offline capture (the editor's Recorder, docs/specs/S27-editor-tools.md R29): SoLoud
		// on its null driver at `sampleRate` x `channels`, nothing reaches the device; the
		// caller mixes it in step with the game's fixed timestep. endOfflineCapture() drops
		// it: the device output opens again on the next sound.
		static bool beginOfflineCapture(unsigned int sampleRate, unsigned int channels);
		static void endOfflineCapture();
		static bool isOfflineCapture();
		// the next `frames` sample frames of the mix, interleaved floats (frames x channels)
		static void mixOffline(float* out, unsigned int frames);

        bool loadSound(AudioSourceComponent& audio);
		void destroySound(AudioSourceComponent& audio);
		void getLoadCount(size_t& loaded, size_t& total) const;
		void stopSceneSounds();
		bool seekSound(AudioSourceComponent& audio, double time);

		void setPaused(bool paused) override;
		void onSchedulerEnabledChanged(bool enabled) override;

		void load() override;
		void draw() override;
		void destroy() override;
		void update(double dt) override;

		void onComponentAdded(Entity entity, ComponentId componentId) override;
		void onComponentRemoved(Entity entity, ComponentId componentId) override;
	};

}

#endif //AUDIOSYSTEM_H