// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef AUDIOSYSTEM_H
#define AUDIOSYSTEM_H

#include "SubSystem.h"

#include "component/AudioSourceComponent.h"

#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace SoLoud{
	class Soloud;
	class AudioSource;
}

namespace jasperengine{

	// Project Settings > Audio (audio-mixer spec A11): the exported init calls configure() before
	// the first sound
	struct AudioConfig {
		// voices SoLoud mixes at once (the quietest others are virtualised), the mixer buses on top
		int maxRealVoices = 32;
		// voices playing at all; past it the least important (highest priority number, oldest
		// first) is stopped, or the new one does not start when it is the least important
		int maxVoices = 256;
		// 0 = the device's default
		int sampleRate = 0;
	};

	// What SoLoud mixes into. Auto: the device, else NoSound (mixes in its own thread with no
	// output: playback, events, fades and meters behave as on a device). Null: mixed on the main
	// thread by AudioMixer::update, deterministic (tests); also the fallback where NoSound cannot
	// run (no threads). JASPERENGINE_AUDIO_BACKEND=nosound|null|auto in the environment.
	enum class AudioBackend { Auto, NoSound, Null };

	class JASPERENGINE_API AudioSystem : public SubSystem {

    private:
		static bool inited;
		// set when no backend opened at all: it is not tried again
		static bool outputUnavailable;

		static void deInit();

		static float globalVolume;

		Vector3 cameraLastPosition;
		// the 3D listener at that scene's camera (the persistent scene's system, every frame)
		void placeListener(Scene& listenerScene);
		bool outputRequested = false;
		// 3D voices are placed against this scene's camera (the persistent scene of feature
		// persistence leaves the listener to the main scene: setListenerEnabled(false))
		bool listenerEnabled = true;

		// AudioSource.playClipAtPoint voices: they belong to this scene and stop with it
		struct PointVoice {
			unsigned int handle = 0;
			std::shared_ptr<SoLoud::AudioSource> sample;
			bool is3D = false;
			Vector3 position;
			bool paused = false;
		};
		struct PendingPointVoice {
			std::string filename;
			Vector3 position;
			float volume = 1.0f;
			std::string group;
			double waited = 0;
		};
		std::vector<PointVoice> pointVoices;
		std::vector<PendingPointVoice> pendingPointVoices;

        bool loadSoundSample(AudioSourceComponent& audio);
        void preloadSoundAssets();
		// the voices of a source: stop them all (main, one-shots, pending one-shots)
		void stopVoices(AudioSourceComponent& audio);
		void releaseSample(AudioSourceComponent& audio);
		unsigned int startVoice(AudioSourceComponent& audio, SoLoud::AudioSource& sample, float volume, bool use3D,
		                        const Vector3& position, bool mainVoice);
		void applyVoiceParameters(AudioSourceComponent& audio, unsigned int handle, bool mainVoice, bool use3D);
		void apply3dParameters(AudioSourceComponent& audio, unsigned int handle, const Vector3& position, const Vector3& velocity);
		bool desiredPause(const AudioSourceComponent& audio) const;
		void applyPause(AudioSourceComponent& audio);
		const std::string& resolveGroup(AudioSourceComponent& audio, Entity entity);
		void startPendingOneShots(AudioSourceComponent& audio, bool use3D, const Vector3& position, double unscaledDt);
		void updatePointVoices(double unscaledDt);
		void updateFade(AudioSourceComponent& audio, double scaledDt, double unscaledDt, std::vector<std::pair<Entity, int>>& events, Entity entity);

	public:
		AudioSystem(Scene* scene);

		static SoLoud::Soloud& getSoloud();
		static bool init();
		// closes the audio (voices and buses stop; the next sound opens it again, with the
		// backend and sample rate set by then)
		static void close();
		// false when the sounds mix into no device (NoSound / Null, or nothing opened)
		static bool isOutputAvailable();
		static bool isInited();

		static void configure(const AudioConfig& config);
		static const AudioConfig& getConfig();
		// before the audio opens (the next init after a deinit)
		static void setBackend(AudioBackend backend);
		static AudioBackend getBackend();
		// "MiniAudio", "NoSound", "null driver", "" before init
		static std::string getBackendName();
		// the Null backend: `seconds` of audio mixed now (AudioMixer::update does it each frame)
		static void mixManually(double seconds);

		static void stopAll();
		// app pause / resume: the Master bus (every routed voice holds; the sources keep their own pause)
		static void pauseAll();
		static void resumeAll();
		static void checkActive();

		// AudioListener.volume (SoLoud's global volume, outside the mixer)
		static void setGlobalVolume(float volume);
		static float getGlobalVolume();
		// AudioListener.pause: every source but the ignoreListenerPause ones holds
		static void setListenerPaused(bool paused);
		static bool isListenerPaused();

		// Decodes (or opens) a clip ahead, so its first play or one-shot has no load; held until
		// the engine shuts down (SoundPool::pin)
		static bool preload(const std::string& filename, AudioLoadType loadType = AudioLoadType::Auto);

		// ---- the voice budget (every voice a source, a one-shot or playClipAtPoint started)
		static unsigned int getVoiceCount();
		// stops a voice the engine started (and forgets it)
		static void stopVoice(unsigned int handle);
		// the mixer moved a group's voices: they now count under `toGroupId`
		static void moveGroupVoices(const std::string& fromGroupId, const std::string& toGroupId);
		// the voices playing through a group (bus annexing when the mixer changes)
		static std::vector<unsigned int> getGroupVoices(const std::string& groupId);
		static void pruneVoices();

		// ---- persistence (DontDestroyOnLoad, feature `persistence`)
		// Moves a source's runtime audio state (main voice, one-shots, fades, pause state) from its
		// component in `from` to its copy in `to`, re-owning the voices: they keep playing, and
		// `source` is left without voices (destroying it or its scene stops nothing). Call it after
		// copying the component, before destroying the source entity. Voice handles are
		// engine-global, so the voices themselves do not move.
		static void moveSourceVoices(Scene* from, AudioSourceComponent& source, Scene* to, AudioSourceComponent& target);
		void setListenerEnabled(bool enabled);
		bool isListenerEnabled() const;

        bool loadSound(AudioSourceComponent& audio);
		void destroySound(AudioSourceComponent& audio);
		void getLoadCount(size_t& loaded, size_t& total) const;
		void stopSceneSounds();
		bool seekSound(AudioSourceComponent& audio, double time);

		// AudioSource.playOneShot: an extra voice of the source (started by the next update)
		bool playOneShot(AudioSourceComponent& audio, const std::string& filename, float volumeScale);
		// AudioSource.playClipAtPoint: a voice of this scene at a world position (3D when the
		// scene has a camera), through a mixer group (name; "" = Master)
		void playClipAtPoint(const std::string& filename, const Vector3& position, float volume, const std::string& group);
		// fadeIn / fadeOut / fadeTo (spec A12)
		void fadeTo(AudioSourceComponent& audio, double volume, double seconds, bool unscaled);
		void fadeIn(AudioSourceComponent& audio, double seconds, bool unscaled);
		void fadeOut(AudioSourceComponent& audio, double seconds, bool pause, bool unscaled);
		// the main voice and the one-shots playing now
		int getPlayingVoiceCount(const AudioSourceComponent& audio) const;

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
