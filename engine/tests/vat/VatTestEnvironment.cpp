// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "VatTestEnvironment.h"

#include <gtest/gtest.h>

#include "Engine.h"
#include "System.h"
#include "json.hpp"

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>

using namespace jasperengine;
using namespace jasperengine::vat;

namespace {

    std::string makeAssetDir(){
        std::random_device device;
        std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                    ("jasperengine-vat-tests-" + std::to_string(device()));
        std::filesystem::create_directories(dir);
        return dir.lexically_normal().generic_string();
    }

    class VatTestSystem : public System {
    public:
        int getScreenWidth() override { return 640; }
        int getScreenHeight() override { return 360; }
        std::string getAssetPath() override { return vattest::assetDir(); }
    };

    class EngineEnvironment : public ::testing::Environment {
    public:
        void SetUp() override {
            static bool started = false;   // the engine starts once per process
            if (started){
                return;
            }
            started = true;
            static char name[] = "jasperengine-vat-tests";
            static char* argv[] = {name, nullptr};
            Engine::systemInit(1, argv, new VatTestSystem());   // owned by the engine for the process
        }

        void TearDown() override {
            std::error_code ignored;
            std::filesystem::remove_all(vattest::assetDir(), ignored);
        }
    };

    // gtest owns the environment; registered when this file is loaded
    const ::testing::Environment* const engineEnvironment =
        ::testing::AddGlobalTestEnvironment(new EngineEnvironment());

    void appendU32(std::vector<uint8_t>& out, uint32_t value){
        for (int i = 0; i < 4; i++) out.push_back((uint8_t)(value >> (8 * i)));
    }

}

const std::string& vattest::assetDir(){
    static const std::string dir = makeAssetDir();
    return dir;
}

std::string vattest::writeAsset(const std::string& relativePath, const std::string& bytes){
    std::filesystem::path path = std::filesystem::path(assetDir()) / relativePath;
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << bytes;
    return path.lexically_normal().generic_string();
}

std::string vattest::writeAsset(const std::string& relativePath, const std::vector<uint8_t>& bytes){
    return writeAsset(relativePath, std::string(bytes.begin(), bytes.end()));
}

VatData vattest::sampleData(){
    VatData data;
    data.guid = "0123456789abcdef0123456789abcdef";
    data.mode = VatMode::Vertices;
    data.mesh = "hero.vat.glb";
    data.layout.precision = VatPrecision::Float;
    data.layout.width = 8;
    data.layout.samples = 7;
    data.layout.vertices = 3;
    data.layout.bones = 0;
    data.layout.normals = true;
    data.layout.colors = false;
    data.layout.height = 6;    // 7 x 3 x 2 = 42 texels in 8 x 6

    VatClip walk;
    walk.name = "Walk";
    walk.start = 0;
    walk.frames = 4;
    walk.fps = 4.0f;            // 1 s loop
    walk.wrap = ClipWrapMode::Loop;
    walk.bounds.extend(Vector3(-1, 0, -1));
    walk.bounds.extend(Vector3(1, 2, 1));
    ClipEventData step;
    step.time = 0.25f;
    step.function = "onStep";
    step.args = {AnimationEvent::stringArg("left"), AnimationEvent::numberArg(2), AnimationEvent::boolArg(true)};
    walk.events.push_back(step);
    data.clips.push_back(walk);

    VatClip wave;
    wave.name = "Wave";
    wave.start = 4;
    wave.frames = 3;
    wave.fps = 2.0f;            // 1 s once (frames - 1) / fps
    wave.wrap = ClipWrapMode::Once;
    wave.speed = 1.5f;
    wave.interpolate = false;
    data.clips.push_back(wave);

    data.bounds.extend(walk.bounds);
    data.sockets.push_back({"Hand", "Root/Hand"});
    data.sockets.push_back({"Head", "Root/Head"});
    data.recipe.sockets = {"Root/Hand", "Root/Head"};
    VatRecipeSource source;
    source.kind = "model";
    source.path = "models/hero.gltf";
    source.clips = {"Walk", "Wave"};
    source.hashes = {{"models/hero.gltf", "sha1:00ff"}};
    data.recipe.sources.push_back(source);
    data.stats.bytes = 8 * 6 * 16;
    data.stats.maxErrorMm = 0.25f;
    data.stats.meanErrorMm = 0.125f;
    data.layout.layoutHash = computeLayoutHash(data);
    return data;
}

VatPayload vattest::samplePayload(const VatData& data){
    VatPayload payload;
    payload.precision = data.layout.precision;
    payload.width = data.layout.width;
    payload.height = data.layout.height;
    size_t texels = (size_t)payload.width * payload.height;
    payload.bytes.assign(texels * (payload.precision == VatPrecision::Float ? 16 : 8), 0);
    if (payload.precision == VatPrecision::Float){
        for (size_t t = 0; t < texels; t++){
            float texel[4] = {(float)t, (float)t + 0.5f, -(float)t, 1.0f};
            std::memcpy(payload.bytes.data() + t * 16, texel, 16);
        }
    }
    return payload;
}

VatSocketTracks vattest::sampleSockets(const VatData& data){
    VatSocketTracks tracks;
    tracks.sockets = (int)data.recipe.sockets.size();
    tracks.frames = data.layout.samples;
    for (int f = 0; f < tracks.frames; f++){
        for (int s = 0; s < tracks.sockets; s++){
            double half = (5.0 * f) * 3.14159265358979323846 / 180.0;
            float row[7] = {(float)f, 10.0f * s, 0.0f, 0.0f, (float)std::sin(half), 0.0f, (float)std::cos(half)};
            tracks.data.insert(tracks.data.end(), row, row + 7);
        }
    }
    return tracks;
}

std::vector<uint8_t> vattest::makeGlb(const std::string& json, const std::vector<uint8_t>& bin, bool withBin){
    std::string text = json;
    while (text.size() % 4 != 0) text += ' ';
    std::vector<uint8_t> binary = bin;
    while (binary.size() % 4 != 0) binary.push_back(0);
    bool hasBin = withBin || !binary.empty();
    uint32_t total = 12 + 8 + (uint32_t)text.size() + (hasBin ? 8 + (uint32_t)binary.size() : 0);
    std::vector<uint8_t> out;
    appendU32(out, 0x46546C67);
    appendU32(out, 2);
    appendU32(out, total);
    appendU32(out, (uint32_t)text.size());
    appendU32(out, 0x4E4F534A);
    out.insert(out.end(), text.begin(), text.end());
    if (hasBin){
        appendU32(out, (uint32_t)binary.size());
        appendU32(out, 0x004E4942);
        out.insert(out.end(), binary.begin(), binary.end());
    }
    return out;
}

std::vector<uint8_t> vattest::makeVatGlb(const std::string& layoutHash, const VatPayload& payload, const VatSocketTracks& sockets){
    std::vector<uint8_t> bin = payload.bytes;
    nlohmann::json views = nlohmann::json::array();
    views.push_back({{"buffer", 0}, {"byteOffset", 0}, {"byteLength", payload.bytes.size()}});
    nlohmann::json extension = {
        {"version", 1},
        {"layoutHash", layoutHash},
        {"payload", {{"bufferView", 0}, {"format", precisionName(payload.precision)}, {"width", payload.width},
                     {"height", payload.height}}}
    };
    if (sockets.sockets > 0){
        while (bin.size() % 4 != 0) bin.push_back(0);
        size_t offset = bin.size();
        size_t bytes = sockets.data.size() * sizeof(float);
        bin.resize(offset + bytes);
        std::memcpy(bin.data() + offset, sockets.data.data(), bytes);
        views.push_back({{"buffer", 0}, {"byteOffset", offset}, {"byteLength", bytes}});
        extension["sockets"] = {{"bufferView", 1}, {"count", sockets.sockets}, {"frames", sockets.frames}};
    }
    nlohmann::json root = {
        {"asset", {{"version", "2.0"}}},
        {"buffers", nlohmann::json::array({{{"byteLength", bin.size()}}})},
        {"bufferViews", views},
        {"extensionsUsed", nlohmann::json::array({VAT_GLTF_EXTENSION})},
        {"extensions", {{VAT_GLTF_EXTENSION, extension}}}
    };
    return makeGlb(root.dump(), bin);
}
