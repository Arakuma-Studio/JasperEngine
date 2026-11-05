// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#ifndef AudioMixer_h
#define AudioMixer_h

#include "Export.h"
#include "audio/AudioMixerIO.h"
#include "math/Vector3.h"

#include <string>
#include <vector>

namespace SoLoud {
    class AudioSource;
    class Bus;
}

namespace jasperengine {

    class MeterFilter;

    // A mixer group for scripts (Lua AudioMixer.getGroup): a handle by id, valid while the group
    // is in the mixer. Values are the runtime ones; setting the volume is a script value (it
    // overrides snapshots, like setFloat).
    class JASPERENGINE_API AudioMixerGroup {
        std::string id;
    public:
        AudioMixerGroup() = default;
        explicit AudioMixerGroup(std::string id);
        const std::string& getId() const { return id; }
        bool isValid() const;
        std::string getName() const;
        float getVolume() const;            // dB
        void setVolume(float db);
        void setVolumeOver(float db, float seconds, bool unscaled);
        bool getMute() const;
        void setMute(bool mute);
        bool getSolo() const;
        void setSolo(bool solo);
        bool getBypassEffects() const;
        void setBypassEffects(bool bypass);
        int getEffectCount() const;
        // the level of its output in dB (AudioMixer::getMeterDb)
        float getMeter() const;
    };

    // An effect of a mixer group for scripts (AudioMixerGroup:getEffect)
    class JASPERENGINE_API AudioMixerEffect {
        std::string id;
    public:
        AudioMixerEffect() = default;
        explicit AudioMixerEffect(std::string id);
        const std::string& getId() const { return id; }
        bool isValid() const;
        std::string getType() const;        // "lowpass", ...
        bool getBypass() const;
        void setBypass(bool bypass);
        float getWet() const;
        void setWet(float wet);
        // false when the effect has no such parameter
        bool setParameter(const std::string& name, float value, float seconds = 0.0f, bool unscaled = false);
        bool getParameter(const std::string& name, float& value) const;
        bool hasParameter(const std::string& name) const;
    };

    // The project's Audio Mixer (audio-mixer spec 6-7): one per process, engine-global (it
    // survives scene loads). Each group is a SoLoud bus played into its parent's (Master on the
    // core); the effects are the bus's filters, a level meter sits in the last slot. Without a
    // mixer asset it is an implicit Master at 0 dB, so a project without one sounds as before.
    //
    // Values: every parameter (a group's volume, an effect's parameter, its wet) has the value of
    // the asset (its fader), a current value moved by snapshots (transitionTo), and, once a script
    // set it (setFloat, a group's volume, an effect's setParameter), an override that wins until
    // cleared (Unity's SetFloat / ClearFloat). Timed changes run on scaled time unless the mixer's
    // updateMode is "unscaled" or the call asks for unscaled time. Main thread only.
    class JASPERENGINE_API AudioMixer {
    public:
        // ---- a capture's view of the groups: their meters, and each one's fader as last applied
        struct GroupMeter {
            std::string id;
            std::string name;
            // its parent's name, "" for Master
            std::string parent;
            const MeterFilter* meter = nullptr;
        };
        static std::vector<GroupMeter> getGroupMeters();
        // linear, a mute or a solo elsewhere included; 1 for no such group
        static float getGroupFader(const std::string& id);
        // the buses from a group to the output: 1 for Master (and for no such group)
        static int getGroupDepth(const std::string& id);
        // the lookahead frames of the limiters from a group to the output, its own among them ("": Master)
        static unsigned int getOutputLatencyFrames(const std::string& id);

        // ---- the asset

        // Reads a .mixer (asset path) and makes it the project mixer: the runtime starts over from
        // its start snapshot and the remembered parameters (UserSettings) come back. "" = the
        // implicit Master. False + error (the mixer stays as it was) when it does not read.
        static bool load(const std::string& path, std::string* error = nullptr);
        // the same from the file's text (the editor reads it itself)
        static bool loadFromText(const std::string& json, const std::string& path, std::string* error = nullptr);
        // A live edit (the editor, MCP): the new definition replaces the old without stopping a
        // voice. Changed values are heard at once (a script override still wins); groups and
        // effects are added, removed, moved and reordered in place; the voices of a removed group
        // go on in Master.
        static void apply(const AudioMixerDef& def);
        // back to the implicit Master
        static void unload();
        static const AudioMixerDef& getDef();
        // the asset path of the mixer ("" = implicit Master)
        static const std::string& getPath();
        // names the asset an apply()ed definition came from (the editor reads the file itself)
        static void setAssetPath(const std::string& path);
        static bool hasAsset();

        // The runtime values start over: overrides cleared, transitions stopped, then the start
        // snapshot (when `startSnapshot`) or the asset's values. Editor Play start / stop.
        static void resetRuntime(bool startSnapshot);

        // ---- exposed parameters (Unity AudioMixer.SetFloat / GetFloat / ClearFloat)

        // false (logged once per name) when no parameter has that name. `seconds` > 0 moves there
        // over time; `unscaled` uses unscaled time.
        static bool setFloat(const std::string& name, float value, float seconds = 0.0f, bool unscaled = false);
        static bool getFloat(const std::string& name, float& value);
        // the parameter goes back to its snapshot value at once
        static bool clearFloat(const std::string& name);
        static bool hasParameter(const std::string& name);
        static std::vector<std::string> getParameterNames();
        static bool isOverridden(const std::string& name);

        // ---- snapshots (Unity AudioMixerSnapshot.TransitionTo, AudioMixer.TransitionToSnapshots)

        static bool transitionTo(const std::string& snapshot, float seconds, bool unscaled = false);
        // While one lives, a capture's `mixer` lines say what caused them (S40: "animation" for a
        // timeline's snapshot cue or action)
        class JASPERENGINE_API CaptureCause {
        public:
            explicit CaptureCause(const char* cause);
            ~CaptureCause();
            CaptureCause(const CaptureCause&) = delete;
            CaptureCause& operator=(const CaptureCause&) = delete;
        private:
            const char* previous;
        };
        // the weighted blend of snapshots (weights >= 0, normalized)
        static bool transitionToSnapshots(const std::vector<std::string>& snapshots, const std::vector<float>& weights,
                                          float seconds, bool unscaled = false);
        static std::vector<std::string> getSnapshotNames();
        // the last snapshot transitioned to (the start snapshot at first), "" for none or a blend
        static std::string getCurrentSnapshot();
        // true while a snapshot transition or a timed change still moves
        static bool isTransitioning();

        // ---- any parameter by target ("<groupId>/volume", "<effectId>/<param>")

        static bool setTarget(const std::string& target, float value, float seconds = 0.0f, bool unscaled = false);
        static bool clearTarget(const std::string& target);
        // the value heard now (override, else snapshot / asset value); false when it does not resolve
        static bool getTarget(const std::string& target, float& value);
        static bool isTargetOverridden(const std::string& target);

        // ---- groups at runtime (names; also ids)

        static std::vector<std::string> getGroupNames();
        static bool hasGroup(const std::string& nameOrId);
        // the group's id ("" when not in the mixer)
        static std::string findGroupId(const std::string& nameOrId);
        static std::string getGroupName(const std::string& id);
        static bool getGroupMute(const std::string& group);
        static bool getGroupSolo(const std::string& group);
        static bool getGroupBypassEffects(const std::string& group);
        static bool setGroupMute(const std::string& group, bool mute);
        static bool setGroupSolo(const std::string& group, bool solo);
        static bool setGroupBypassEffects(const std::string& group, bool bypass);
        // false when muted, or when another group is soloed and this one is neither soloed nor
        // above or below a soloed group (Unity's solo)
        static bool isGroupAudible(const std::string& group);
        // the group's effect id by index (0-based) or type name ("lowpass"; the first of it); "" when none
        static std::string findEffectId(const std::string& group, int index);
        static std::string findEffectId(const std::string& group, const std::string& type);
        static bool getEffectBypass(const std::string& effectId);
        static bool setEffectBypass(const std::string& effectId, bool bypass);

        // The level of a group's output (after its effects and fader, before its parent): the peak
        // of the last ~90 ms in dB, -80 when silent or unknown. Real levels without an audio device
        // (the nosound / null backends mix all the same).
        static float getMeterDb(const std::string& group);
        // A limiter's largest gain reduction of the last 100 ms (dB, 0 or more; 0 when it is not
        // running). The frames its lookahead delays a group's bus (0 when bypassed): a mixer's
        // latency per group, which a capture adds for Master (S38 R6).
        static float getEffectGainReductionDb(const std::string& effectId);
        static unsigned int getGroupLatencyFrames(const std::string& group);
        // The Master group's meter also passes its blocks on, up to 2 s of them, for a live loudness
        // measurement (the editor's; off by default, a game never turns it on). The drain gives
        // them post-fader, as getMeterDb reads, interleaved; 0 frames when there are none.
        static void setLoudnessTap(bool on);
        static bool isLoudnessTapOn();
        static size_t drainLoudnessTap(float* interleaved, size_t maxFrames, unsigned int& channels, unsigned int& sampleRate);

        // ---- remembered parameters (exposed with remember: UserSettings "audio.mixer.<name>")

        // the remembered parameters a script set to UserSettings now (and saved); the keys of the
        // others are removed (they follow the snapshots)
        static void saveRemembered();
        // the remembered values in UserSettings set again (as script values)
        static void loadRemembered();
        static std::string rememberedKey(const std::string& name);

        // ---- frame

        // Once per frame (Engine::runFrame, before the scene updates; also while the game is
        // paused, with 0 scaled time): transitions and timed changes, the values to SoLoud,
        // remembered writes, the voice budget, and the mixing of the null backend.
        static void update(double scaledDt, double unscaledDt);
        // the unscaled delta of the current frame (fades with unscaled time)
        static double getFrameUnscaledDelta();
        // the unscaled seconds of every frame so far (monotonic): a clocked voice starts at it, so
        // sounds played in the frames one output buffer spans start as far apart as those frames
        static double getClockTime();

        static float dbToLinear(float db);
        static float linearToDb(float linear);

        // ---- engine-internal (AudioSystem)

        // the SoLoud core opened / is closing: the buses are built / dropped
        static void onAudioInit();
        static void onAudioShutdown();
        // the bus of a group ("" = Master; an unknown group = Master), null before the audio opens
        static SoLoud::Bus* getBus(const std::string& groupIdOrName);
        static unsigned int getBusHandle(const std::string& groupIdOrName);
        // a voice through a group's bus (paused, so its parameters can be set first); 0 when it
        // could not start. No mixer bus yet (the audio not open): through the core.
        static unsigned int play(const std::string& groupId, SoLoud::AudioSource& sample, bool is3D,
                                 const Vector3& position, float volume, bool clocked, double clockTime);
        // groups + their meters: how many SoLoud voices the mixer itself holds
        static unsigned int getBusVoiceCount();
        static bool isBusHandle(unsigned int handle);
        // pauses everything at the Master bus: the game's AudioSystem.pauseAll / resumeAll (spec A13)
        static void setMasterPaused(bool paused);
        static bool isMasterPaused();
        // the platform's pause of the Master bus (the app in the background, a native SDK's hold):
        // apart from the game's, so a resume never lifts a pause menu's pauseAll, nor a game's
        // resumeAll the background's
        static void setPlatformPaused(bool paused);
        static bool isPlatformPaused();
        // SoLoud mixes at most this many voices at once, the buses included (Project Settings >
        // Audio's Max Real Voices + the mixer's groups are clamped to it)
        static constexpr unsigned int maxActiveVoices = 255;
        // tests: how often the voice limit was set (each reallocates SoLoud's voice buffers) and
        // effect filters were made
        static void getDebugCounters(unsigned int& voiceLimitChanges, unsigned int& filterBuilds);
    };

}

#endif /* AudioMixer_h */
