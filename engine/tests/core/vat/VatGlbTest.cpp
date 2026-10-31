// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// readVatGlb (spec 2.1): the JASPER_vertex_animation extension's payload and socket buffer views
// read back from a good .vat.glb, and every corrupted or foreign input refused with a clear error
// (no crash, no out-of-bounds read).

#include <gtest/gtest.h>

#include "VatTestEnvironment.h"

#include "animation/VertexAnimation.h"
#include "json.hpp"

#include <functional>

using namespace jasperengine;
using namespace jasperengine::vat;

namespace {

    struct Glb{
        nlohmann::json json;
        std::vector<uint8_t> bin;
    };

    // the sample asset's glb, as JSON + BIN to edit
    Glb sampleGlb(){
        VatData data = vattest::sampleData();
        std::vector<uint8_t> bytes = vattest::makeVatGlb(data.layout.layoutHash, vattest::samplePayload(data),
                                                         vattest::sampleSockets(data));
        uint32_t jsonLength = bytes[12] | (bytes[13] << 8) | (bytes[14] << 16) | ((uint32_t)bytes[15] << 24);
        Glb glb;
        glb.json = nlohmann::json::parse(std::string(bytes.begin() + 20, bytes.begin() + 20 + jsonLength));
        glb.bin.assign(bytes.begin() + 20 + jsonLength + 8, bytes.end());
        return glb;
    }

    std::string readError(const std::vector<uint8_t>& bytes){
        VatGlbContents contents;
        std::string error;
        EXPECT_FALSE(readVatGlb(bytes, contents, &error));
        EXPECT_EQ(error.rfind("invalid vertex animation mesh: ", 0), 0u) << error;
        return error;
    }

    std::string editError(const std::function<void(Glb&)>& edit){
        Glb glb = sampleGlb();
        edit(glb);
        return readError(vattest::makeGlb(glb.json.dump(), glb.bin));
    }

    const char* EXT = VAT_GLTF_EXTENSION;

}

TEST(VatGlb, ReadsPayloadAndSockets){
    VatData data = vattest::sampleData();
    VatPayload payload = vattest::samplePayload(data);
    VatSocketTracks sockets = vattest::sampleSockets(data);
    std::vector<uint8_t> bytes = vattest::makeVatGlb(data.layout.layoutHash, payload, sockets);
    VatGlbContents contents;
    std::string error;
    ASSERT_TRUE(readVatGlb(bytes, contents, &error)) << error;
    EXPECT_EQ(contents.layoutHash, data.layout.layoutHash);
    EXPECT_EQ(contents.payload.precision, VatPrecision::Float);
    EXPECT_EQ(contents.payload.width, 8);
    EXPECT_EQ(contents.payload.height, 6);
    EXPECT_EQ(contents.payload.bytes, payload.bytes);
    EXPECT_EQ(contents.sockets.sockets, 2);
    EXPECT_EQ(contents.sockets.frames, 7);
    EXPECT_EQ(contents.sockets.data, sockets.data);

    // the contents build the asset
    std::shared_ptr<const VertexAnimation> asset = VertexAnimation::create(data, contents.payload, contents.sockets, &error);
    EXPECT_TRUE(asset) << error;

    // sockets are optional; half payloads
    data.layout.precision = VatPrecision::Half;
    payload = vattest::samplePayload(data);
    bytes = vattest::makeVatGlb("ab12", payload, VatSocketTracks());
    ASSERT_TRUE(readVatGlb(bytes, contents, &error)) << error;
    EXPECT_EQ(contents.payload.precision, VatPrecision::Half);
    EXPECT_EQ(contents.payload.bytes.size(), 8u * 6u * 8u);
    EXPECT_EQ(contents.sockets.sockets, 0);
    EXPECT_TRUE(contents.sockets.data.empty());
    EXPECT_EQ(contents.layoutHash, "ab12");

    // a null error pointer is fine
    EXPECT_FALSE(readVatGlb(std::vector<uint8_t>(), contents, nullptr));
}

TEST(VatGlb, ContainerErrors){
    std::vector<uint8_t> good = vattest::makeVatGlb("ab", vattest::samplePayload(vattest::sampleData()), VatSocketTracks());

    EXPECT_NE(readError({}).find("not a binary glTF"), std::string::npos);
    EXPECT_NE(readError({'g', 'l', 'T', 'F'}).find("not a binary glTF"), std::string::npos);
    std::vector<uint8_t> bytes = good;
    bytes[0] = 'G';
    EXPECT_NE(readError(bytes).find("not a binary glTF"), std::string::npos);
    bytes = good;
    bytes[4] = 1;
    EXPECT_NE(readError(bytes).find("container version 1"), std::string::npos);
    // truncated: the header length disagrees
    bytes = good;
    bytes.resize(bytes.size() - 4);
    EXPECT_NE(readError(bytes).find("the header says"), std::string::npos);
    // a chunk longer than the file
    bytes = good;
    bytes[12] = 0xFF;
    bytes[13] = 0xFF;
    EXPECT_NE(readError(bytes).find("past the end of the file"), std::string::npos);
    // a truncated chunk header (header length fixed up to match)
    bytes = good;
    bytes.push_back(0);
    bytes.push_back(0);
    bytes[8] = (uint8_t)(bytes.size());
    bytes[9] = (uint8_t)(bytes.size() >> 8);
    bytes[10] = (uint8_t)(bytes.size() >> 16);
    EXPECT_NE(readError(bytes).find("truncated chunk header"), std::string::npos);
    // the first chunk must be JSON
    bytes = good;
    bytes[16] = 'X';
    EXPECT_NE(readError(bytes).find("the first chunk is not JSON"), std::string::npos);
    // no chunk at all
    std::vector<uint8_t> header(good.begin(), good.begin() + 12);
    header[8] = 12;
    header[9] = header[10] = header[11] = 0;
    EXPECT_NE(readError(header).find("no JSON chunk"), std::string::npos);
    // JSON that doesn't parse
    EXPECT_NE(readError(vattest::makeGlb("{\"asset\": ", {})).find("not a JSON object"), std::string::npos);
    EXPECT_NE(readError(vattest::makeGlb("[]", {})).find("not a JSON object"), std::string::npos);
}

TEST(VatGlb, ExtensionErrors){
    EXPECT_NE(editError([](Glb& g){ g.json.erase("extensions"); }).find("no JASPER_vertex_animation extension"), std::string::npos);
    EXPECT_NE(editError([](Glb& g){ g.json["extensions"].erase(EXT); }).find("rebake"), std::string::npos);
    EXPECT_NE(editError([](Glb& g){ g.json["extensions"][EXT] = 1; }).find("is not an object"), std::string::npos);
    EXPECT_NE(editError([](Glb& g){ g.json["extensions"][EXT]["version"] = 2; }).find("version 2"), std::string::npos);
    EXPECT_NE(editError([](Glb& g){ g.json["extensions"][EXT]["version"] = 0; }).find(".version"), std::string::npos);
    EXPECT_NE(editError([](Glb& g){ g.json["extensions"][EXT].erase("layoutHash"); }).find(".layoutHash"), std::string::npos);
    EXPECT_NE(editError([](Glb& g){ g.json["extensions"][EXT]["layoutHash"] = ""; }).find(".layoutHash"), std::string::npos);
    EXPECT_NE(editError([](Glb& g){ g.json["extensions"][EXT].erase("payload"); }).find(".payload: missing"), std::string::npos);
    EXPECT_NE(editError([](Glb& g){ g.json["extensions"][EXT]["payload"]["format"] = "rgba8"; }).find(".payload: expected"),
              std::string::npos);
    EXPECT_NE(editError([](Glb& g){ g.json["extensions"][EXT]["payload"]["width"] = 0; }).find(".payload: expected"),
              std::string::npos);
    EXPECT_NE(editError([](Glb& g){ g.json["extensions"][EXT]["payload"]["height"] = -3; }).find(".payload: expected"),
              std::string::npos);
    EXPECT_NE(editError([](Glb& g){ g.json["extensions"][EXT]["payload"]["bufferView"] = 1.5; }).find(".payload: expected"),
              std::string::npos);
    // payload size vs width x height
    EXPECT_NE(editError([](Glb& g){ g.json["extensions"][EXT]["payload"]["height"] = 5; }).find("payload: 768 bytes"),
              std::string::npos);
    EXPECT_NE(editError([](Glb& g){ g.json["extensions"][EXT]["payload"]["format"] = "half"; }).find("payload: 768 bytes"),
              std::string::npos);
    EXPECT_NE(editError([](Glb& g){ g.json["extensions"][EXT]["sockets"] = 1; }).find(".sockets: expected"), std::string::npos);
    EXPECT_NE(editError([](Glb& g){ g.json["extensions"][EXT]["sockets"].erase("frames"); }).find(".sockets: expected"),
              std::string::npos);
    EXPECT_NE(editError([](Glb& g){ g.json["extensions"][EXT]["sockets"]["count"] = 3; }).find("sockets: 392 bytes"),
              std::string::npos);
    EXPECT_NE(editError([](Glb& g){
        g.json["extensions"][EXT]["sockets"]["count"] = 1u << 30;
        g.json["extensions"][EXT]["sockets"]["frames"] = 1u << 30;
    }).find("sockets:"), std::string::npos);
}

TEST(VatGlb, BufferViewErrors){
    EXPECT_NE(editError([](Glb& g){ g.json["extensions"][EXT]["payload"]["bufferView"] = 9; }).find("bufferView 9 doesn't exist"),
              std::string::npos);
    EXPECT_NE(editError([](Glb& g){ g.json.erase("bufferViews"); }).find("doesn't exist"), std::string::npos);
    EXPECT_NE(editError([](Glb& g){ g.json["bufferViews"][0] = 1; }).find("is not an object"), std::string::npos);
    EXPECT_NE(editError([](Glb& g){ g.json["bufferViews"][0].erase("byteLength"); }).find("no valid buffer"), std::string::npos);
    EXPECT_NE(editError([](Glb& g){ g.json["bufferViews"][0]["byteOffset"] = -4; }).find("no valid buffer"), std::string::npos);
    EXPECT_NE(editError([](Glb& g){ g.json["bufferViews"][0]["byteStride"] = 16; }).find("tightly packed"), std::string::npos);
    EXPECT_NE(editError([](Glb& g){ g.json["bufferViews"][0]["buffer"] = 1; }).find("the GLB's own buffer"), std::string::npos);
    EXPECT_NE(editError([](Glb& g){ g.json["buffers"][0]["uri"] = "data.bin"; }).find("the GLB's own buffer"), std::string::npos);
    EXPECT_NE(editError([](Glb& g){ g.json.erase("buffers"); }).find("the GLB's own buffer"), std::string::npos);
    EXPECT_NE(editError([](Glb& g){ g.json["bufferViews"][1]["byteOffset"] = 100000; }).find("is past the BIN chunk"),
              std::string::npos);
    EXPECT_NE(editError([](Glb& g){ g.json["bufferViews"][0]["byteLength"] = 1u << 31; }).find("is past the BIN chunk"),
              std::string::npos);
    // a BIN chunk cut short
    EXPECT_NE(editError([](Glb& g){ g.bin.resize(100); }).find("is past the BIN chunk"), std::string::npos);
    // no BIN chunk
    Glb glb = sampleGlb();
    EXPECT_NE(readError(vattest::makeGlb(glb.json.dump(), {}, false)).find("no BIN chunk"), std::string::npos);
}
