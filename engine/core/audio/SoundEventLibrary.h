// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef SoundEventLibrary_h
#define SoundEventLibrary_h

#include "Export.h"
#include "audio/SoundEvent.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine {

    // what the clock says when a play is asked for. In a deterministic run the mixer's sample
    // cursor (a cooldown compares samples, so no float subtraction decides a play on the
    // boundary); with a device its seconds.
    struct SoundEventClock {
        bool samples = false;
        uint64_t cursor = 0;
        unsigned int rate = 48000;
        double seconds = 0.0;
    };

    // one play of an event (SoundEventLibrary::pick)
    struct SoundEventPick {
        // the clip indices to start: one, or every clip in layer mode
        std::vector<int> clips;
        // the play's draws, the same for every layer
        double pitchSemitones = 0.0;
        double volumeDb = 0.0;
        double delayMs = 0.0;
        // the event's accepted plays before this one since the last reset
        uint64_t playIndex = 0;
    };

    // A loaded event: its definition, with every clip's path resolved by its GUID
    struct SoundEventAsset {
        // assets-relative, as the game names it
        std::string path;
        // 32 hex digits (the .meta's); the path when the project has no record of the file
        std::string key;
        std::shared_ptr<const SoundEventDef> def;
    };

    // Every Sound Event a game has played (docs/specs/S37-sound-events.md R5): read from its file
    // on first use (small files, synchronously), kept per path, GUIDs mapped through AssetMap (the
    // asset database in the editor, assetmap.bin in a game). Main thread only.
    //
    // The runtime state of an event (its shuffle bag, last clip, sequence position, play count and
    // last accepted start) is per event: every source and script playing it shares it.
    class JASPERENGINE_API SoundEventLibrary {
    public:
        // a GUID (32 hex digits, either case) or an assets-relative path; nullptr when it does not
        // resolve or its file does not read (logged once until it changes or the library resets).
        // The pointer is good until the next reload or clear.
        static const SoundEventAsset* find(const std::string& guidOrPath);
        static bool isGuid(const std::string& text);
        // names an event: a .soundevent path, or the GUID of one
        static bool isEventName(const std::string& text);

        // The event file changed (S05, the editor's own writes): the next play uses the new
        // definition; a change of its clip list or mode resets its bag and sequence. Voices that
        // play keep playing. A file that went or does not read drops out.
        static void reload(const std::string& path);
        // Replaces an event from text without a file (tests, the editor's unsaved preview)
        static const SoundEventAsset* load(const std::string& path, const std::string& json, std::string* error = nullptr);
        // Forgets every definition and its runtime (a project closed)
        static void clear();

        // Drops every bag, last clip, sequence position, play count and cooldown stamp, and the
        // seed. Play start, a game's start.
        static void resetRuntime();
        // the plays use this seed from now on until the next reset (a deterministic run's audio.seed)
        static void setSeed(uint64_t seed);
        // the seed of the plays since the last reset (drawn from std::random_device when none was set)
        static uint64_t getSeed();

        // step 1 of a start: the event's cooldown has passed since its last accepted play. Changes nothing.
        static bool cooledDown(const SoundEventAsset& event, const SoundEventClock& now);
        // step 3: the play's clips and draws with the play's generator, which advances the event's
        // bag, last clip, sequence position and play count, and stamps `now` as its last start
        static SoundEventPick pick(const SoundEventAsset& event, const SoundEventClock& now);
        // A pick computed apart from the runtime (Audition): the next `count` plays of `def` from
        // a fresh state under `seed`
        static std::vector<SoundEventPick> preview(const SoundEventDef& def, const std::string& key, uint64_t seed, int count);
    };

}

#endif /* SoundEventLibrary_h */
