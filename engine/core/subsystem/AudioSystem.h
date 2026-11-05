// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef AUDIOSYSTEM_H
#define AUDIOSYSTEM_H

#include "SubSystem.h"

#include "component/AudioSourceComponent.h"
#include "ecs/Entity.h"

#include <atomic>
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

	class Scene;
	class SoundHandle;
	struct PlayOptions;
	struct SoundEventAsset;
	struct SoundEventSpatial;
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
		// the Sound Event generator's seed in a deterministic run (Project Settings audio.seed);
		// other runs draw one at each start
		uint32_t seed = 1;
	};

	// One line of what happened to a voice, for AudioSystem::setVoiceObserver: every field is the
	// capture log's (docs/specs/S37-sound-events.md R14). Built only while an observer is set.
	struct AudioVoiceLine {
		enum class Kind { Start, Stop, Reject, Pause, Resume };
		Kind kind = Kind::Start;
		// stop: "stop", "ended", "replaced", "clip_changed", "disabled", "removed", "scene", "fade_out",
		// "stop_all", "stolen_instance", "stolen_source", "stolen_budget". reject: "budget",
		// "max_voices", "cooldown", "instance_limit", "missing_clip", "missing_event", "impact_gate"
		const char* reason = "";
		unsigned int handle = 0;
		Scene* scene = nullptr;
		Entity entity = NULL_ENTITY;
		std::string clip;
		// the event's GUID and path; empty for a clip
		std::string event;
		std::string eventPath;
		// its clip index in the event, and its place in a layered pick
		int variation = -1;
		int layer = -1;
		uint64_t instance = 0;
		// the instance that stole it
		uint64_t byInstance = 0;
		// main, one_shot, point, free, cue (S40: a clip's sound cue)
		const char* owner = "main";
		// play, event, trigger, lua, cpp, animation (S40: a timeline started it)
		const char* cause = "play";
		const char* trigger = "";
		float impactSpeed = -1.0f;
		float gain = 1.0f;
		float pitch = 1.0f;
		float pan = 0.0f;
		bool looping = false;
		bool spatial = false;
		Vector3 position;
		int priority = 128;
		std::string group;
		uint32_t delayFrames = 0;
		// the frames it waited for its clip to load
		uint32_t loadDelay = 0;
		// the mixer's sample cursor in a deterministic run
		uint64_t sample = 0;
	};

	// What SoLoud mixes into. Auto: the device, else NoSound (mixes in its own thread with no
	// output: playback, events, fades and meters behave as on a device). Null: mixed on the main
	// thread by AudioMixer::update (tests); also the fallback where NoSound cannot run (no threads).
	// Deterministic: 48 kHz stereo mixed once a game frame, after the scenes' update, by the frame's
	// unscaled time (--headless, --silent-audio, a Play that captures, a Recorder movie; no thread,
	// every target). JASPERENGINE_AUDIO_BACKEND=nosound|null|deterministic|auto in the environment.
	enum class AudioBackend { Auto, NoSound, Null, Deterministic };

	// where the mix goes: a device, nowhere (NoSound or the Null fallback), or the deterministic driver
	enum class AudioOutputMode { Device, NoOutput, Deterministic };

	// a voice beyond what the source itself says (a Sound Event's values, clip variation)
	struct AudioVoiceExtras {
		float speedScale = 1.0f;
		// -1: the source's
		int priority = -1;
		// null: the source's group
		const std::string* groupId = nullptr;
		bool eventLoop = false;
		uint32_t delayFrames = 0;
		const SoundEventSpatial* spatial = nullptr;
		const char* owner = "main";
		const char* cause = "play";
		const char* trigger = "";
		float impactSpeed = -1.0f;
		uint32_t loadDelay = 0;
		Entity entity = NULL_ENTITY;
		const char* clip = "";
		const char* event = "";
		const char* eventPath = "";
		int variation = -1;
		int layer = -1;
		uint64_t instance = 0;
	};
	class JASPERENGINE_API AudioSystem : public SubSystem {
		// the unit tests reach a voice's parameters without the frame's 3D pass after them
		friend struct AudioSystemTestAccess;

    private:
		static bool inited;
		// set when no backend opened at all: it is not tried again
		static bool outputUnavailable;

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

		using VoiceExtras = AudioVoiceExtras;
		// a voice the scene plays by a trigger (R12): queued by the signal, started by the update
		struct TriggerEvent {
			uint32_t flag = 0;
			Entity entity = NULL_ENTITY;
			Entity other = NULL_ENTITY;
			// a contact's approach speed; < 0: none
			float speed = -1.0f;
		};
		// what the signals' callbacks hold: copies of a component keep them, so they must not hold the system
		struct TriggerSink {
			AudioSystem* system = nullptr;
		};

		std::shared_ptr<TriggerSink> triggerSink;
		std::vector<TriggerEvent> triggerQueue;
		std::vector<TriggerEvent> contactQueue3D;
		// the flags the sources need, and which physics signals that makes it subscribed to
		uint32_t physicsNeeded = 0;
		uint32_t physicsBound = 0;
		// a play of an event, through startEvent
		struct EventPlay {
			enum class Owner { Main, OneShot };
			Owner owner = Owner::Main;
			AudioSourceComponent* audio = nullptr;
			Entity entity = NULL_ENTITY;
			bool use3D = false;
			Vector3 position;
			float volumeScale = 1.0f;
			float impactGain = 1.0f;
			float impactSpeed = -1.0f;
			const char* cause = "event";
			const char* trigger = "";
			// a free voice: its name in the lines, and its group when the play asked for one
			const char* ownerName = nullptr;
			const std::string* group = nullptr;
			// a timeline's start (S40 R5): output frames before every voice, its speed scaled
			uint32_t delayFrames = 0;
			float speedScale = 1.0f;
		};
		struct StartedPlay {
			uint64_t instance = 0;
			std::vector<unsigned int> handles;
		};

        bool loadSoundSample(AudioSourceComponent& audio);
        void preloadSoundAssets();
		// the voices of a source: stop them all (main, one-shots, pending one-shots)
		void stopVoices(AudioSourceComponent& audio);
		void releaseSample(AudioSourceComponent& audio);
		unsigned int startVoice(AudioSourceComponent& audio, SoLoud::AudioSource& sample, float volume, bool use3D,
		                        const Vector3& position, bool mainVoice, const AudioVoiceExtras& extras = AudioVoiceExtras());
		void applyVoiceParameters(AudioSourceComponent& audio, unsigned int handle, bool mainVoice, bool use3D, float speedScale = 1.0f, bool eventLoop = false,
		                          SoLoud::AudioSource* sample = nullptr);
		void apply3dParameters(AudioSourceComponent& audio, unsigned int handle, const Vector3& position, const Vector3& velocity,
		                       const SoundEventSpatial* spatial = nullptr);
		// Starts an event (docs/specs/S37-sound-events.md R6), the one function every caller uses:
		// cooldown, instance limit, pick, voices, in this order
		StartedPlay startEvent(const SoundEventAsset& event, const EventPlay& play);
		// the source's event plays as its main voice; false when nothing started
		bool startMainEvent(AudioSourceComponent& audio, Entity entity, bool use3D, const Vector3& position);
		// the cap of a source (spec A10): false when no one-shot can give its place
		bool makeSourceRoom(AudioSourceComponent& audio, uint64_t instance);
		void stopMainVoices(AudioSourceComponent& audio);
		void pruneMainVoices(AudioSourceComponent& audio);
		// a source's event by its GUID, else its path; a reject line and one warning when it does not resolve
		const SoundEventAsset* resolveSourceEvent(const std::string& name, Entity entity, const char* owner, const char* cause, const char* trigger, float impactSpeed);
		// the event's group, as an AudioSource's resolves (id, name, then Master, warned once)
		std::string eventGroup(const SoundEventAsset& event, const std::string& fallback);
		void clipVariation(AudioSourceComponent& audio, Entity entity, float& volumeScale, float& speedScale);
		void preloadEvent(AudioSourceComponent& audio);
		// ---- triggers (R12)
		void queueTrigger(uint32_t flag, Entity entity, Entity other, float speed);
		void syncUiTriggers(AudioSourceComponent& audio, Entity entity, uint32_t desired);
		void syncPhysicsTriggers(uint32_t needed);
		void removeTriggerSubscriptions();
		void drainTriggers();
		void playTrigger(const TriggerEvent& event, Entity entity);
		// ---- free voices (R13)
		bool startFree(uint32_t slot);
		void updateFreeVoices(double scaledDt, double unscaledDt, bool hasListener, const std::function<Vector3(const Vector3&)>& heard,
		                      bool doppler, bool& any3D);
		void forgetFreeVoices();
		void logReject(const char* reason, Entity entity, const std::string& clip, const SoundEventAsset* event, const std::string& eventName,
		               const char* owner, const char* cause, const char* trigger, float impactSpeed, int priority, const std::string& group);
		bool desiredPause(const AudioSourceComponent& audio) const;
		void applyPause(AudioSourceComponent& audio);
		const std::string& resolveGroup(AudioSourceComponent& audio, Entity entity);
		void startPendingOneShots(AudioSourceComponent& audio, Entity entity, bool use3D, const Vector3& position, double unscaledDt);
		// hasListener: 3D against the frame's listener; heard: where it hears a world position
		void updatePointVoices(double unscaledDt, bool hasListener, const std::function<Vector3(const Vector3&)>& heard);
		void updateFade(AudioSourceComponent& audio, double scaledDt, double unscaledDt, std::vector<std::pair<Entity, int>>& events, Entity entity);

	public:
		AudioSystem(Scene* scene);
		~AudioSystem() override;

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
		// the Null backend: `seconds` of audio mixed now (AudioMixer::update does it each frame)
		static void mixManually(double seconds);

		// ---- the deterministic driver (docs/specs/S35-agent-audio.md)
		static AudioOutputMode getOutputMode();
		// `seconds` of audio mixed now in chunks for AudioTap's clients: the frame's unscaled time, as
		// the double Engine::runFrame computes (0 while the app, the simulation or the Play is paused)
		static void pumpDeterministic(double unscaledSeconds);
		// The frames mixed since the audio opened, on any null driver
		static uint64_t getMixedFrames();
		// the frames between a voice's stamp and its first sample in the mix: one for the voice, one
		// for each bus on its way out and the limiters' lookahead there, so it depends on the group ("": Master)
		static unsigned int getDeterministicLatency(const std::string& groupId = "");
		// A Play that captures and a Recorder movie hold the driver: the first hold opens it and the
		// last one reopens what was selected before. Both re-initialise SoLoud, so voices stop: only
		// at a Play's start and stop and a Recorder's start and end. False when it did not open.
		static bool beginDeterministic();
		static void endDeterministic();
		// a Play's capture or a movie holds the driver: its mix is theirs, no preview plays into it
		static bool isDeterministicHeld();
		// What was selected, and whether a device was open, beneath a hold: a Recorder movie makes
		// neither of them "no device" for the Play's capture
		static AudioBackend getSelectedBackend();
		static bool hasDevice();

		// ---- Sound Events (docs/specs/S37-sound-events.md)
		// Forgets every event's bag, sequence, cooldown and instances, the one-per-Play warnings and
		// the seed. Play start and stop.
		static void resetSoundEventRuntime();
		// the mixing is on the null driver, in step with the game: the same frames give the same
		// samples, and a seed is audio.seed
		static bool isDeterministic();
		// Called with every voice's start, stop and rejection (nullptr: none). The capture hook.
		static void setVoiceObserver(std::function<void(const AudioVoiceLine&)> observer);
		// The generator's seed of this run, chosen at its first use
		static uint64_t getSoundEventSeed();

		// ---- free voices: Audio::play and SoundHandle
		enum class FreeValue { Volume, Pitch, Pan };
		static SoundHandle playFree(const std::string& eventOrClip, const PlayOptions& options, const char* cause);
		// A clip's sound cue (S40 R6): a free voice owned "cue", cause "animation", starting
		// delayFrames output frames into this frame's mix
		static SoundHandle playFreeFromTimeline(const std::string& eventOrClip, const PlayOptions& options, uint32_t delayFrames);
		// stops it with a fade of `seconds`, its lines saying `reason` (a cue's state_exit: "cue_exit")
		static void freeStopFaded(uint32_t id, uint32_t generation, float seconds, const char* reason);
		// the voices of Audio.play, triggers and one-shots: all, or those on `group` or below it
		static int stopFreeAndOneShots(const std::string& group);
		static bool freeIsValid(uint32_t id, uint32_t generation);
		static bool freeIsPlaying(uint32_t id, uint32_t generation);
		static void freeStop(uint32_t id, uint32_t generation);
		static void freePause(uint32_t id, uint32_t generation, bool paused);
		static void freeFadeOut(uint32_t id, uint32_t generation, float seconds, bool pause, bool unscaled);
		static void freeFadeTo(uint32_t id, uint32_t generation, float volume, float seconds, bool unscaled);
		static float freeGet(uint32_t id, uint32_t generation, FreeValue value);
		static void freeSet(uint32_t id, uint32_t generation, FreeValue value, float amount);
		static Vector3 freeGetPosition(uint32_t id, uint32_t generation);
		static void freeSetPosition(uint32_t id, uint32_t generation, const Vector3& position);
		static double freeGetTime(uint32_t id, uint32_t generation);
		static int freeGetVoiceCount(uint32_t id, uint32_t generation);

		// While one lives on this thread (the physics system delivering its contacts), the calls of
		// Audio and SoundHandle defer to the frame's audio update (R13); off the main thread they always do
		class JASPERENGINE_API PhysicsCallbackScope {
		public:
			PhysicsCallbackScope();
			~PhysicsCallbackScope();
			PhysicsCallbackScope(const PhysicsCallbackScope&) = delete;
			PhysicsCallbackScope& operator=(const PhysicsCallbackScope&) = delete;
		};
		static bool isDeferringCalls();

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
