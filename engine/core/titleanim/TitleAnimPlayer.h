// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S85-title-animation.md R24-R31, §6.5)
// - Purpose: plays a title's animations, always built (a Baked title plays with no module). One
//   base animation and any number of overlays (R25); ends, `next` chains, stops, resets, seeks,
//   samples and fades (R26); a driven mode where a clip or an Animator sets the time (R27); the
//   markers crossed each frame (R28, D4). It samples poses; what plays them is the caller's:
//   TitleArtSystem writes the companion's draw values (companionUniforms) and the flipbook frame,
//   the module applies layer and glyph values in Realtime and Glyph Atlas.
// - Clock (R26, R31): only the dt the caller gives (scaled or unscaled); never wall-clock time.
//   The first update after play advances nothing and fires the markers at the start time, so
//   the start pose shows before the frame draws and nothing between is skipped.
// - Allocation (R35): update and the summary allocate nothing in steady state (a library swap,
//   play and the end of an animation may).
// - Thread-safety: main thread.

#ifndef TITLE_ANIM_PLAYER_H
#define TITLE_ANIM_PLAYER_H

#include "Export.h"
#include "titleanim/TitleAnimSampling.h"
#include "titleanim/TitleAnimTypes.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine::titleanim{

    // what a title can play: compiled animations by name and the default
    struct JASPERENGINE_API TitleAnimLibrary{
        std::vector<CompiledTitleAnimation> animations;
        std::string defaultName;
        // true when the module compiled them from the document (every track); false: a Baked
        // player's `<file>.anim.json` (title tracks, flipbooks)
        bool fromDocument = false;
        const CompiledTitleAnimation* find(const std::string& name) const;
        std::vector<std::string> names() const;
    };

    struct JASPERENGINE_API TitleAnimPlayOptions{
        float speed = 1.0f;
        int loop = -1;              // -1: the animation's wrap; 0: once; 1: loop
        float fade = 0.0f;          // seconds (R26)
        double time = 0.0;          // start time
    };

    struct JASPERENGINE_API PlayingAnimation{
        const CompiledTitleAnimation* animation = nullptr;
        std::string name;
        double time = 0.0;          // unwrapped local seconds since its start
        float speed = 1.0f;
        AnimWrap wrap = AnimWrap::Once;
        bool started = false;       // false until the first update (which fires the start's markers)
        bool finished = false;      // a once animation at its end (holds its last pose)
        bool held = false;          // stopped: keeps its pose, does not advance
        uint64_t serial = 0;        // start order (the later overlay wins)
        TitleAnimPose pose;
        // the local time sampled (the wrap with this play's wrap override)
        double localTime() const;
    };

    struct JASPERENGINE_API FiredMarker{
        const CompiledMarker* marker = nullptr;
        const CompiledTitleAnimation* animation = nullptr;
        float fraction = 0.0f;      // where in this frame's dt it was crossed (0..1): S40's sub-frame placement
    };

    class JASPERENGINE_API TitleAnimPlayer{
    public:
        TitleAnimPlayer();

        // the animations (a document reload swaps them: what plays restarts by name, or stops
        // when the name is gone)
        void setLibrary(std::shared_ptr<const TitleAnimLibrary> library);
        const std::shared_ptr<const TitleAnimLibrary>& library() const{ return lib; }
        const CompiledTitleAnimation* find(const std::string& name) const;

        // false (and nothing changes) for a name the library does not have (R30)
        bool play(const std::string& name, const TitleAnimPlayOptions& options = {});
        bool playOverlay(const std::string& name, const TitleAnimPlayOptions& options = {});
        // every animation; the pose stays (Unity's rule)
        void stop();
        // one by name: the base keeps its pose, an overlay ends; false when it does not play
        bool stop(const std::string& name);
        // back to the rest values: nothing plays or holds a pose
        void reset();
        void setPaused(bool paused);
        bool isPaused() const{ return pausedFlag; }
        // shows `name` at `time` without playing (pauses); false for an unknown name
        bool sample(const std::string& name, double time);
        // the base's time (animationTime's write): no markers
        void seek(double time);
        // R27: a clip or Animator drives `name` at `time` this frame (the player does not advance
        // it); markers fire when the time moved forward since the last frame
        bool drive(const std::string& name, double time, std::vector<FiredMarker>* markers);
        // multiplies every animation's speed (the component's Animation Speed, 0..10)
        void setSpeedMultiplier(float multiplier){ speedMultiplier = multiplier; }
        bool isDriven() const{ return driven; }

        // One frame: advances what plays by `dt` (each animation's speed applied), chains `next`,
        // samples every pose; appends the crossed markers in time order and the names of once
        // animations that reached their end (before their `next` starts).
        void update(double dt, std::vector<FiredMarker>& markers, std::vector<std::string>& finished);

        bool isPlaying() const;                         // something advances
        bool isPlaying(const std::string& name) const;
        // something shows a pose (playing, paused, stopped with its pose kept)
        bool active() const{ return base.animation != nullptr || !overlayList.empty(); }
        const PlayingAnimation* baseAnimation() const{ return base.animation ? &base : nullptr; }
        const std::vector<PlayingAnimation>& overlays() const{ return overlayList; }
        const std::string& baseName() const{ return base.name; }
        double baseTime() const{ return base.time; }
        double duration(const std::string& name) const;     // -1 for an unknown name
        // a fade in progress: the animation it fades from (its pose frozen) and the new one's share 0..1
        const PlayingAnimation* fadingFrom(float* weight) const;

        // the whole title's draw values: base, then overlays (relative), then the fade
        void summary(const double* liveRest, TitlePoseSummary& out) const;
        // the base's flipbook frame (-1 when it has none); `blend` and `nextFrame` the frame
        // blend's second frame and its share
        int flipbookFrame(int* nextFrame, float* blend) const;

    private:
        std::shared_ptr<const TitleAnimLibrary> lib;
        PlayingAnimation base;
        std::vector<PlayingAnimation> overlayList;
        PlayingAnimation fadeFrom;
        float fadeDuration = 0.0f, fadeElapsed = 0.0f;
        TitlePoseSummary fadeSnapshot;
        bool fadeSnapshotValid = false;
        bool pausedFlag = false;
        bool stopped = false;       // stop(): poses held, nothing advances
        bool driven = false;
        double drivenTime = 0.0;
        float speedMultiplier = 1.0f;
        uint64_t serials = 0;
        std::vector<CrossedMarker> crossed;     // scratch, reused

        bool start(PlayingAnimation& slot, const CompiledTitleAnimation& animation, const TitleAnimPlayOptions& options);
        void samplePose(PlayingAnimation& p);
        // advances one animation by `dt` seconds of the frame (`dt` > 0 or its start); markers get
        // their place in the frame; returns true when a once animation reached its end, with the
        // overflow (seconds of its own time past the end) and the frame's seconds spent before it
        bool advance(PlayingAnimation& p, double dt, double frameDt, double frameOffset, std::vector<FiredMarker>& markers,
                     double* overflow, double* spent);
        void emitMarkers(const PlayingAnimation& p, double from, double to, bool includeStart, double rate, double frameDt,
                         double frameOffset, std::vector<FiredMarker>& markers);
    };

    // ---- the companion's draw (R13; S81's title_fx ANIM feature) ----

    // the values the system writes into title_fx: the vertex affine (rows; frame-normalised,
    // moves the companion's vertices so a scaled or slid title is never cut at its rect), the
    // fragment rows (identity) with the opacity and the flipbook blend in their w, the flash
    // (linear rgb, amount), and the flipbook frames' texture rects
    struct JASPERENGINE_API CompanionAnimUniforms{
        float vertex0[4] = {1, 0, 0, 0};
        float vertex1[4] = {0, 1, 0, 0};
        float affine0[4] = {1, 0, 0, 1};
        float affine1[4] = {0, 1, 0, 0};
        float color[4] = {1, 1, 1, 0};
        float frameRect[4] = {0, 0, 1, 1};
        float frameRect2[4] = {0, 0, 1, 1};
    };
    // `flipbook` null or empty: the still; else frame `frame` (and `next` blended by `blend`)
    JASPERENGINE_API void companionUniforms(const TitlePoseSummary& summary, const TitleFrameGeometry& still, TrackPivot pivot,
                                            const CompiledFlipbook* flipbook, int frame, int next, float blend, CompanionAnimUniforms& out);

}

#endif //TITLE_ANIM_PLAYER_H
