// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The engine's System for the vat tests (started once per process by VatTestEnvironment.cpp):
// asset-relative paths resolve under a fresh temporary asset directory, so tests can write .vat and
// .vat.glb files and load them through both the asset-relative and the absolute form. Plus the
// shared fixtures: a small valid asset (data, payload, socket tracks) and a minimal GLB writer.

#ifndef VAT_TEST_ENVIRONMENT_H
#define VAT_TEST_ENVIRONMENT_H

#include "animation/VertexAnimation.h"

#include <cstdint>
#include <string>
#include <vector>

namespace vattest {

    // the asset root (absolute, generic separators, no trailing '/')
    const std::string& assetDir();
    // writes `bytes` to assetDir()/relativePath (directories created); returns the absolute path
    std::string writeAsset(const std::string& relativePath, const std::string& bytes);
    std::string writeAsset(const std::string& relativePath, const std::vector<uint8_t>& bytes);

    // A vertices asset: 3 vertices, normals, half precision, width 8, clips "Walk" (loop, 4
    // frames at 0) and "Wave" (once, 3 frames at 4); sockets "Hand" (recipe socket "Root/Hand")
    // and "Head" ("Root/Head"); layout hash computed. Payload texel t holds (t, t + 0.5, -t, 1) as
    // floats in float precision, 0 in half precision. Socket tracks: socket s at frame f has
    // position (f, 10 * s, 0) and rotation about Y by 10 * f degrees (x y z w).
    jasperengine::vat::VatData sampleData();
    jasperengine::vat::VatPayload samplePayload(const jasperengine::vat::VatData& data);
    jasperengine::vat::VatSocketTracks sampleSockets(const jasperengine::vat::VatData& data);

    // A GLB: the JSON chunk `json` (padded with spaces) and a BIN chunk `bin` (padded with 0s,
    // omitted when empty and `withBin` is false)
    std::vector<uint8_t> makeGlb(const std::string& json, const std::vector<uint8_t>& bin, bool withBin = true);
    // the .vat.glb of an asset: extension with payload (+ sockets when given) buffer views
    std::vector<uint8_t> makeVatGlb(const std::string& layoutHash, const jasperengine::vat::VatPayload& payload,
                                    const jasperengine::vat::VatSocketTracks& sockets);

}

#endif
