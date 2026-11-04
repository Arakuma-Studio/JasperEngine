// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef AUDIOSYSTEM_H
#define AUDIOSYSTEM_H

#include "SubSystem.h"

#include "component/AudioSourceComponent.h"

#include <functional>
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

	// What the output does to the mix. Roundoff: SoLoud's soft clipper and post-clip scaler (a small
	// signal comes out 1.66 dB lower). Linear: samples pass unchanged, hard-clipped at +/-1 only as a
	// last guard, so that a Master limiter's ceiling and the measured loudness hold at the output.
	enum class AudioOutputStage { Roundoff, Linear };
	// The iOS audio session. Default leaves miniaudio's own choice (it ignores the silent switch and
	// interrupts the player's music); ignored on the other platforms.
	enum class IosAudioSession { Default, Ambient, SoloAmbient, Playback };

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
		// this one applies at once; the iOS session at the next audio init, like the sample rate
		AudioOutputStage outputStage = AudioOutputStage::Roundoff;
		IosAudioSession iosSession = IosAudioSession::Default;
	};

	// What SoLoud mixes into. Auto: the device, else NoSound (mixes in its own thread with no
	// output: playback, events, fades and meters behave as on a device). Null: mixed on the main
	// thread by AudioMixer::update, deterministic (tests); also the fallback where NoSound cannot
	// run (no threads). JASPERENGINE_AUDIO_BACKEND=nosound|null|auto in the environment.
	enum class AudioBackend { Auto, NoSound, Null };

	class JASPERENGINE_API AudioSystem : public SubSystem {
		// the unit tests reach a voice's parameters without the frame's 3D pass after them
		friend struct AudioSystemTestAccess;

    private:
		static bool inited;
		// set when no backend opened at all: it is not tried again
		static bool outputUnavailable;
		// the Recorder's capture holds SoLoud on its null driver
		static bool offlineCapture;

		static void deInit();
		// after a successful init or a change: the output stage's flag and post-clip scaler, and the log line
		static void applyOutputStage();

		static float globalVolume;

		// where the listener heard from last frame (the camera that heard), and its velocity this frame
		Vector3 cameraLastPosition;
		bool listenerTracked = false;
		Scene* listenerTrackedScene = nullptr;
		Entity listenerTrackedCamera = NULL_ENTITY;
		Vector3 listenerVelocity;
		// the sources' and the listener's last positions are last frame's (false after the system held)
		bool motionFresh = false;
		// the 3D listener at that scene's camera (the main scene's, for every scene's system)
		void placeListener(Scene& listenerScene);
		bool outputRequested = false;
		// off while the scheduler skips the system (Systems window): its sounds hold too
		bool schedulerEnabled = true;

		// the scene's sounds paused or resumed as setPaused, the scheduler and the listener say
		void applySoundPause();
		// this system places the 3D listener (always the main scene's camera) when it plays 3D voices;
		// off, it leaves that to the others (setListenerEnabled(false))
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
		// hasListener: 3D against the frame's listener; heard: where it hears a world position
		void updatePointVoices(double unscaledDt, bool hasListener, const std::function<Vector3(const Vector3&)>& heard);
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
		// "linear" / "ambient", "solo_ambient", "playback" (project.yaml, game.json); anything else the default
		static AudioOutputStage outputStageFromName(const std::string& name);
		static IosAudioSession iosSessionFromName(const std::string& name);
		static const AudioConfig& getConfig();
		// before the audio opens (the next init after a deinit)
		static void setBackend(AudioBackend backend);
		static AudioBackend getBackend();
		// "MiniAudio", "NoSound", "null driver", "" before init
		static std::string getBackendName();
		// the Null backend: `seconds` of audio mixed now (AudioMixer::update does it each frame);
		// nothing during an offline capture, whose caller mixes (mixOffline)
		static void mixManually(double seconds);

		static void stopAll();
		// the game's pause of everything (Lua AudioSystem.pauseAll / resumeAll): the Master bus (every
		// routed voice holds; the sources keep their own pause)
		static void pauseAll();
		static void resumeAll();
		// the platform's (Engine::applyPause: the app in the background): the Master bus too, on its
		// own flag, so returning from the background leaves the game's pauseAll in place
		static void setPlatformPaused(bool paused);
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
		// sounds playing now (getVoiceCount: not the mixer's buses) and the most that play at once
		// (0, 0 before the output opens)
		static void getVoiceCounts(uint32_t& active, uint32_t& max);

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
