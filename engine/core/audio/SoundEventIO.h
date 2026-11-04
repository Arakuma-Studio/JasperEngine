// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef SoundEventIO_h
#define SoundEventIO_h

#include "Export.h"
#include "audio/SoundEvent.h"

#include <string>
#include <vector>

namespace jasperengine {

    // The .soundevent file (docs/specs/S37-sound-events.md R1): JSON, UTF-8, LF, 2-space indent,
    // {"format": "jasperengine.soundevent", "version": 1}; keys in the spec's order, every key but
    // format, version, mode and clips written only when not default; unknown keys kept.
    class JASPERENGINE_API SoundEventIO {
    public:
        static constexpr const char* FORMAT = "jasperengine.soundevent";
        static constexpr int VERSION = 1;

        // The file's structure: false + "<json path>: <reason>" when it is not JSON, not a Sound
        // Event, of a newer version, or a value has the wrong type. The ranges are validate's.
        static bool parse(const std::string& json, SoundEventDef& out, std::string* error = nullptr);
        static std::string write(const SoundEventDef& def);
        // Every value out of its range, each as "<json path>: <reason>"; empty when the event is
        // valid. The one check the editor, MCP and the engine share.
        static std::vector<std::string> validate(const SoundEventDef& def);
        // what `type` may be
        static const std::vector<std::string>& soundTypes();
    };

}

#endif /* SoundEventIO_h */
