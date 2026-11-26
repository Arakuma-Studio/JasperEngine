// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Force To Mono at export (spec build-size-tools R19): a clip that plays in mono and whose file is a
// RIFF/WAVE of PCM (8, 16, 24 bit) or float32 samples ships as a mono WAV at the same path, at half its
// data bytes (a held clip decodes to half the memory too). Other formats ship as they are and fold at
// run time (AudioClipSource): the engine vendors no encoder.

#include <string>

namespace jasperengine::editor {

    namespace MonoWavCook {
        // The mono WAV of a WAV of two or more channels: the same rate and frame count, the same sample
        // format (8-bit becomes PCM16, its 8 bits widened), each frame the mean of its channels (integers
        // rounded half away from zero), only the fmt and data chunks; the same input gives the same bytes.
        // False, out untouched, for anything else (a mono file, RF64, another encoding, a cut file).
        bool cook(const std::string& wav, std::string& out);
    }

}
