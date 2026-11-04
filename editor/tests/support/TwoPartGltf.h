// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// A glTF made of two mesh nodes, which MeshSystem::loadGLTF loads as a model made of parts
// (ModelComponent::meshNodesMapping): "Body" (node 0, two primitives, morph targets Smile and
// Blink) at the origin and "Eyes" (node 1, one primitive, morph target Blink) at x = 2. Each
// primitive is the triangle (0,0,0) (1,0,0) (0,1,0); a target moves it by (0,0,1).

#include "json.hpp"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace jasperengine::editor::test {

    // one triangle mesh (no morph targets) on the given nodes and scene roots
    inline std::filesystem::path writeTriangleGltf(const std::filesystem::path& dir, const std::string& name,
                                                   const nlohmann::json& nodes, const nlohmann::json& roots) {
        std::filesystem::create_directories(dir);
        const float data[18] = {0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 1, 0, 0, 1};  // positions, normals
        const uint16_t indices[4] = {0, 1, 2, 0};
        {
            std::ofstream bin(dir / (name + ".bin"), std::ios::binary);
            bin.write(reinterpret_cast<const char*>(data), 72);
            bin.write(reinterpret_cast<const char*>(indices), sizeof(indices));
        }
        const nlohmann::json gltf = {
            {"asset", {{"version", "2.0"}}},
            {"scene", 0},
            {"scenes", {{{"nodes", roots}}}},
            {"nodes", nodes},
            {"meshes", {{{"primitives", {{{"attributes", {{"POSITION", 0}, {"NORMAL", 1}}}, {"indices", 2}}}}}}},
            {"buffers", {{{"uri", name + ".bin"}, {"byteLength", 80}}}},
            {"bufferViews", {
                {{"buffer", 0}, {"byteOffset", 0}, {"byteLength", 36}, {"target", 34962}},
                {{"buffer", 0}, {"byteOffset", 36}, {"byteLength", 36}, {"target", 34962}},
                {{"buffer", 0}, {"byteOffset", 72}, {"byteLength", 6}, {"target", 34963}}}},
            {"accessors", {
                {{"bufferView", 0}, {"componentType", 5126}, {"count", 3}, {"type", "VEC3"},
                 {"min", {0, 0, 0}}, {"max", {1, 1, 0}}},
                {{"bufferView", 1}, {"componentType", 5126}, {"count", 3}, {"type", "VEC3"}},
                {{"bufferView", 2}, {"componentType", 5123}, {"count", 3}, {"type", "SCALAR"}}}},
        };
        const std::filesystem::path path = dir / (name + ".gltf");
        std::ofstream(path) << gltf.dump(2);
        return path;
    }

    inline std::filesystem::path writeTwoPartGltf(const std::filesystem::path& dir, const std::string& name) {
        std::filesystem::create_directories(dir);
        const float positions[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
        const float normals[9] = {0, 0, 1, 0, 0, 1, 0, 0, 1};
        const uint16_t indices[4] = {0, 1, 2, 0}; // padded to 4 bytes
        std::vector<unsigned char> data(sizeof(positions) + sizeof(normals) + sizeof(indices));
        std::memcpy(data.data(), positions, sizeof(positions));
        std::memcpy(data.data() + sizeof(positions), normals, sizeof(normals));
        std::memcpy(data.data() + sizeof(positions) + sizeof(normals), indices, sizeof(indices));
        {
            std::ofstream bin(dir / (name + ".bin"), std::ios::binary);
            bin.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        }
        const nlohmann::json primitive = {{"attributes", {{"POSITION", 0}, {"NORMAL", 1}}}, {"indices", 2}};
        nlohmann::json bodyPrimitive = primitive;
        bodyPrimitive["targets"] = {{{"POSITION", 3}}, {{"POSITION", 3}}};
        nlohmann::json eyesPrimitive = primitive;
        eyesPrimitive["targets"] = {{{"POSITION", 3}}};
        const nlohmann::json gltf = {
            {"asset", {{"version", "2.0"}}},
            {"scene", 0},
            {"scenes", {{{"nodes", {0, 1}}}}},
            {"nodes", {
                {{"name", "Body"}, {"mesh", 0}},
                {{"name", "Eyes"}, {"mesh", 1}, {"translation", {2, 0, 0}}}}},
            {"meshes", {
                {{"name", "Body"}, {"primitives", {bodyPrimitive, bodyPrimitive}}, {"extras", {{"targetNames", {"Smile", "Blink"}}}}},
                {{"name", "Eyes"}, {"primitives", {eyesPrimitive}}, {"extras", {{"targetNames", {"Blink"}}}}}}},
            {"buffers", {{{"uri", name + ".bin"}, {"byteLength", data.size()}}}},
            {"bufferViews", {
                {{"buffer", 0}, {"byteOffset", 0}, {"byteLength", 36}, {"target", 34962}},
                {{"buffer", 0}, {"byteOffset", 36}, {"byteLength", 36}, {"target", 34962}},
                {{"buffer", 0}, {"byteOffset", 72}, {"byteLength", 6}, {"target", 34963}}}},
            {"accessors", {
                {{"bufferView", 0}, {"componentType", 5126}, {"count", 3}, {"type", "VEC3"},
                 {"min", {0, 0, 0}}, {"max", {1, 1, 0}}},
                {{"bufferView", 1}, {"componentType", 5126}, {"count", 3}, {"type", "VEC3"}},
                {{"bufferView", 2}, {"componentType", 5123}, {"count", 3}, {"type", "SCALAR"}},
                {{"bufferView", 1}, {"componentType", 5126}, {"count", 3}, {"type", "VEC3"},
                 {"min", {0, 0, 1}}, {"max", {0, 0, 1}}}}},
        };
        const std::filesystem::path path = dir / (name + ".gltf");
        std::ofstream(path) << gltf.dump(2);
        return path;
    }

}
