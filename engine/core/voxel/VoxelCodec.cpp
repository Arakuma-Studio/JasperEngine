// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "VoxelCodec.h"

using namespace jasperengine::voxel;

namespace {
    constexpr size_t MAX_RUN = 128;
    constexpr size_t MAX_LITERALS = 128;
}

void jasperengine::voxel::drle16Encode(const uint16_t* values, size_t count, std::vector<uint8_t>& out) {
    uint16_t previous = 0;
    size_t i = 0;
    while (i < count) {
        if (values[i] == previous) {
            size_t run = 1;
            while (i + run < count && run < MAX_RUN && values[i + run] == previous) run++;
            out.push_back(static_cast<uint8_t>(run - 1));
            i += run;
            continue;
        }
        size_t literals = 1;
        uint16_t last = values[i];
        // A lone zero delta costs one byte as a run and two as a literal, but a token: keep going
        // through it only when another literal follows at once
        while (i + literals < count && literals < MAX_LITERALS && values[i + literals] != last) {
            last = values[i + literals];
            literals++;
        }
        out.push_back(static_cast<uint8_t>(0x7F + literals));
        for (size_t k = 0; k < literals; k++) {
            const uint16_t delta = static_cast<uint16_t>(values[i + k] - previous);
            out.push_back(static_cast<uint8_t>(delta));
            out.push_back(static_cast<uint8_t>(delta >> 8));
            previous = values[i + k];
        }
        i += literals;
    }
}

bool jasperengine::voxel::drle16Decode(const uint8_t* bytes, size_t size, uint16_t* values, size_t count) {
    size_t at = 0;
    size_t produced = 0;
    uint16_t previous = 0;
    while (at < size) {
        const uint8_t token = bytes[at++];
        if (token < 0x80) {
            const size_t run = static_cast<size_t>(token) + 1;
            if (produced + run > count) return false;
            for (size_t k = 0; k < run; k++) values[produced++] = previous;
        } else {
            const size_t literals = static_cast<size_t>(token) - 0x7F;
            if (produced + literals > count || at + literals * 2 > size) return false;
            for (size_t k = 0; k < literals; k++) {
                const uint16_t delta = static_cast<uint16_t>(bytes[at] | (static_cast<uint16_t>(bytes[at + 1]) << 8));
                at += 2;
                previous = static_cast<uint16_t>(previous + delta);
                values[produced++] = previous;
            }
        }
    }
    return produced == count;
}
