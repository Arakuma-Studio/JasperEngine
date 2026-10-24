// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef SHADERBUILDTYPES_H
#define SHADERBUILDTYPES_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include "Engine.h"
#include "ShaderData.h"

namespace jasperengine{

    // How many property bits a variant has room for: the property mask of every shader
    // type (ShaderPool::getShaderPropertyCount is at most this) is a uint64_t
    constexpr int SHADER_PROPERTY_BITS = 64;

    // Property bit n (0..63) of a variant's mask. Spelled out because (1u << n) is
    // undefined from bit 32 on, and ~(1u << n) clears the upper half of a 64-bit mask.
    constexpr uint64_t shaderPropertyBit(int bit){
        return uint64_t(1) << bit;
    }

    // A shader variant: its ShaderType, its property bits (one #define each, see
    // ShaderPool::getShaderPropertyName) and the custom (forked) shader it uses
    // (ShaderPool::registerCustomShader, 0 = built-in). Ordered by customId, type, then
    // properties: the order the packed uint64 key it replaces had, so sorted lists and
    // what is written from them keep their order.
    struct ShaderKey {
        uint64_t properties = 0;
        uint16_t type = 0;      // ShaderType
        uint16_t customId = 0;  // session-local, see ShaderPool::getStorageKey

        constexpr ShaderKey() = default;
        constexpr ShaderKey(uint16_t type, uint64_t properties, uint16_t customId)
            : properties(properties), type(type), customId(customId) {}

        friend constexpr bool operator==(const ShaderKey& a, const ShaderKey& b){
            return a.properties == b.properties && a.type == b.type && a.customId == b.customId;
        }
        friend constexpr bool operator!=(const ShaderKey& a, const ShaderKey& b){
            return !(a == b);
        }
        friend constexpr bool operator<(const ShaderKey& a, const ShaderKey& b){
            if (a.customId != b.customId) return a.customId < b.customId;
            if (a.type != b.type) return a.type < b.type;
            return a.properties < b.properties;
        }
        friend constexpr bool operator>(const ShaderKey& a, const ShaderKey& b){ return b < a; }
        friend constexpr bool operator<=(const ShaderKey& a, const ShaderKey& b){ return !(b < a); }
        friend constexpr bool operator>=(const ShaderKey& a, const ShaderKey& b){ return !(a < b); }
    };

    // What a shader is compiled for. One value per shader language string, which is
    // what names the file the runtime loads. Metal is split because the cross-compiler
    // emits different MSL for macOS and iOS.
    enum class ShaderBackend{
        GLCore,      // glsl410
        GLES3,       // glsl300es
        D3D11,       // hlsl5
        MetalMacOS,  // msl21macos
        MetalIOS,    // msl21ios
        Vulkan       // spirv10
    };

    struct ShaderBuildResult {
        ShaderData data;
        ResourceLoadState state;

        ShaderBuildResult() : state(ResourceLoadState::NotStarted) {}
        ShaderBuildResult(const ShaderData& shaderData, ResourceLoadState buildState)
            : data(shaderData), state(buildState) {}

        explicit operator bool() const {
            return state == ResourceLoadState::Finished;
        }
    };

}

namespace std {
    template <>
    struct hash<jasperengine::ShaderKey> {
        size_t operator()(const jasperengine::ShaderKey& key) const noexcept {
            // splitmix64 of the properties, mixed with type and customId
            uint64_t x = key.properties ^ ((((uint64_t)key.type << 32) | (uint64_t)key.customId) * 0x9E3779B97F4A7C15ULL);
            x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
            x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
            x ^= x >> 31;
            return static_cast<size_t>(x);
        }
    };
}

#endif // SHADERBUILDTYPES_H