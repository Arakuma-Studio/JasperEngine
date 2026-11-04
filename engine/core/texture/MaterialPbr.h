// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef MATERIALPBR_H
#define MATERIALPBR_H

#include "Export.h"
#include "math/Vector2.h"
#include "math/Vector3.h"
#include "texture/Texture.h"

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine{

    struct Material;
    enum class MaterialLighting;

    // The extended PBR lobes of the built-in Lit shader (glTF KHR_materials_*) and its hex
    // tiling (docs/specs/S11-materials-lighting.md). Each lobe has its own enabled flag: a
    // disabled lobe compiles out of the shader, an enabled one costs its lobe even with its
    // factors at zero (animating a factor never switches the variant). Held out of line by
    // Material::pbr: a default MaterialPbrExtensions costs a material one pointer.
    // Colours are linear. Each lobe owns at most one map, in glTF's channel layout.
    struct JASPERENGINE_API MaterialPbrExtensions{
        // KHR_materials_clearcoat: a dielectric coat (IOR 1.5) over the base
        struct Clearcoat{
            bool enabled = false;
            float factor = 0.0f;
            float roughness = 0.0f;
            float normalScale = 1.0f;
            Texture map;       // R factor, G roughness
            Texture normalMap; // tangent-space normal of the coat (RGB)
            int mapTexCoord = 0;
            int normalTexCoord = 0;
            bool operator == (const Clearcoat& o) const;
        } clearcoat;
        // KHR_materials_sheen: the Charlie lobe of cloth
        struct Sheen{
            bool enabled = false;
            Vector3 color = Vector3(0.0f, 0.0f, 0.0f);
            float roughness = 0.0f;
            Texture map; // RGB colour (sRGB), A roughness
            int mapTexCoord = 0;
            bool operator == (const Sheen& o) const;
        } sheen;
        // KHR_materials_anisotropy: the highlight stretched along a direction of the tangent plane
        struct Anisotropy{
            bool enabled = false;
            float strength = 0.0f;
            float rotation = 0.0f; // radians, from the tangent towards the bitangent
            Texture map;           // RG direction (tangent space, [-1, 1] as [0, 1]), B strength
            int mapTexCoord = 0;
            bool operator == (const Anisotropy& o) const;
        } anisotropy;
        // KHR_materials_iridescence: a thin film (soap bubbles, oil)
        struct Iridescence{
            bool enabled = false;
            float factor = 0.0f;
            float ior = 1.3f;
            Vector2 thicknessNm = Vector2(100.0f, 400.0f); // the map's G range; without a map, the maximum
            Texture map;                                   // R factor, G thickness
            int mapTexCoord = 0;
            bool operator == (const Iridescence& o) const;
        } iridescence;
        // KHR_materials_transmission: light through the surface (the scene behind it, refracted)
        struct Transmission{
            bool enabled = false;
            float factor = 0.0f;
            Texture map; // R transmission, G the volume's thickness (KHR_materials_volume)
            int mapTexCoord = 0;
            bool operator == (const Transmission& o) const;
        } transmission;
        // KHR_materials_volume: the transmitted light's absorption and refraction (needs transmission)
        struct Volume{
            bool enabled = false;
            float thickness = 0.0f;
            float attenuationDistance = INFINITY;
            Vector3 attenuationColor = Vector3(1.0f, 1.0f, 1.0f);
            bool operator == (const Volume& o) const;
        } volume;
        // KHR_materials_specular and KHR_materials_ior: the dielectric reflectance
        struct Specular{
            bool enabled = false;
            float factor = 1.0f;
            float ior = 1.5f;
            Vector3 color = Vector3(1.0f, 1.0f, 1.0f);
            Texture map; // RGB colour (sRGB), A factor
            int mapTexCoord = 0;
            bool operator == (const Specular& o) const;
        } specular;
        // hex tiling (Mikkelsen 2022): the textures of UV set 0 sampled on three randomly rotated
        // copies, which hides the repetition of a tiled texture
        struct Hex{
            bool enabled = false;
            float rotation = 1.0f;  // 0..1, the share of a full turn a tile may rotate
            float contrast = 0.6f;  // 0..1, how hard the copies blend (by luminance)
            bool operator == (const Hex& o) const;
        } hex;
        // the Hair shading model (Material > Shading Model > Hair, spec S57 R5): Kajiya-Kay
        // strands with two highlights along tangents shifted towards the normal, for hair cards.
        // The strands run along the mesh's tangent turned by rotation, or the flow map's direction
        struct Hair{
            bool enabled = false;
            Vector3 primaryColor = Vector3(0.5f, 0.5f, 0.5f);   // the light's highlight (linear)
            float primaryShift = 0.1f;                          // -1..1, towards the root
            float primaryWidth = 0.15f;                         // 0..1, the highlight's spread
            Vector3 secondaryColor = Vector3(0.5f, 0.5f, 0.5f); // tinted by the albedo (linear)
            float secondaryShift = -0.1f;                       // -1..1, towards the tip
            float secondaryWidth = 0.35f;
            float rotation = 0.0f;  // radians, from the tangent towards the bitangent
            Texture flowMap;        // RG the strands' tangent-space direction ([-1, 1] as [0, 1])
            int flowMapTexCoord = 0;
            bool operator == (const Hair& o) const;
        } hair;

        bool operator == (const MaterialPbrExtensions& o) const;
        bool operator != (const MaterialPbrExtensions& o) const { return !(*this == o); }
    };

    // The Lit material's Shading Model (spec S57 R5): the Hair lobe's enabled flag, as the editor,
    // MCP and Lua show it. Stored as ints: append only
    enum class ShadingModel{
        STANDARD,
        HAIR
    };

    // What the mobile and web exports leave out of the lobes (project export setting): REDUCED
    // strips iridescence, the anisotropy direction map and the coat normal map
    enum class PbrTier{
        FULL,
        REDUCED
    };

    // The extension maps, in the priority a variant keeps them by (it binds at most
    // MaterialPbr::MAX_EXTENSION_MAPS of them, u_pbrExtTexture0..2)
    enum class PbrMap : uint8_t{
        CLEARCOAT,
        SPECULAR,
        TRANSMISSION,
        SHEEN,
        ANISOTROPY,
        IRIDESCENCE,
        CLEARCOAT_NORMAL,
        HAIR_FLOW,
        COUNT
    };

    // The lobes as the editor, MCP and Lua name them
    enum class PbrLobe{
        CLEARCOAT,
        SHEEN,
        ANISOTROPY,
        IRIDESCENCE,
        TRANSMISSION,
        VOLUME,
        SPECULAR,
        HEX,
        HAIR,
        COUNT
    };

    // The built-in Lit variant a material's lobes select: its sorted #defines (the
    // "builtin:lit#..." reference), the maps it binds in slot order, and the maps it leaves out
    // (over the budget, or stripped by the tier)
    struct JASPERENGINE_API PbrVariant{
        std::vector<std::string> defines;
        std::vector<PbrMap> maps;
        std::vector<std::string> droppedMaps;  // over MAX_EXTENSION_MAPS, lowest priority first out
        std::vector<std::string> strippedMaps; // left out by the PbrTier
        bool empty() const { return defines.empty(); }
    };

    // One editable field of MaterialPbrExtensions: the table every consumer iterates
    // (serialization, generated C++, the Catalog paths material.pbr.<lobe>.<field>, MCP, Lua,
    // the inspector)
    enum class PbrFieldType{
        Bool,
        Float,
        Vector2,
        Color,    // Vector3, linear
        Texture,
        TexCoord  // int, the UV set (0 or 1) of the lobe's map
    };

    struct PbrField{
        PbrLobe lobe;
        const char* name;          // camelCase, the YAML / MCP key inside the lobe
        PbrFieldType type;
        void* (*ref)(MaterialPbrExtensions& pbr);
        float minValue;            // Float, Vector2, Color: the valid range
        float maxValue;
        const char* label;         // "Roughness": the inspector row (the lobe's label in front for undo)
        const char* luaName;       // the flat Material property in Lua, null when not exposed
        PbrMap map;                // Texture, TexCoord: the map it is or selects the UV set of
    };

    struct PbrLobeInfo{
        PbrLobe lobe;
        const char* name;   // "clearcoat": the YAML / MCP / Catalog key
        const char* label;  // "Clear Coat": the inspector header
        const char* define; // its #define
    };

    class JASPERENGINE_API MaterialPbr{
    public:
        static const int MAX_EXTENSION_MAPS = 3;
        // u_fs_pbrExt: 8 vec4 (includes/pbr_ext.glsl)
        static const int UNIFORM_FLOATS = 32;
        // a project warns past this many builtin:lit variants (each one is a compile and an
        // exported shader per backend)
        static const int VARIANT_WARNING = 64;

        static const std::vector<PbrLobeInfo>& getLobes();
        static const PbrLobeInfo& getLobe(PbrLobe lobe);
        static const PbrLobeInfo* findLobe(const std::string& name);
        static bool isLobeEnabled(const MaterialPbrExtensions& pbr, PbrLobe lobe);
        static bool& lobeEnabled(MaterialPbrExtensions& pbr, PbrLobe lobe);

        static const std::vector<PbrField>& getFields();
        // "clearcoat.roughness" (the lobe's enabled flag is "clearcoat.enabled")
        static const PbrField* findField(const std::string& path);
        static const PbrField* findField(PbrLobe lobe, const std::string& name);
        // the Lua flat name (clearcoatRoughness, ior, emissiveStrength excluded)
        static const PbrField* findLuaField(const std::string& luaName);
        // "Clear Coat Roughness": the undo name and the MCP error's field label
        static std::string getFieldLabel(const PbrField& field);
        static std::string getFieldPath(const PbrField& field); // "clearcoat.roughness"

        // a value for the field's range (the IOR also takes 0, glTF's "no refraction" value)
        static bool isInRange(const PbrField& field, float value);
        // every value into its field's range, a non-finite one to its default (true: one was)
        static bool clamp(MaterialPbrExtensions& pbr);

        static const char* getMapName(PbrMap map); // "clearcoat", "clearcoatNormal"
        static const char* getMapDefine(PbrMap map);
        static PbrLobe getMapLobe(PbrMap map);
        static Texture& getMap(MaterialPbrExtensions& pbr, PbrMap map);
        static const Texture& getMap(const MaterialPbrExtensions& pbr, PbrMap map);
        static int getMapTexCoord(const MaterialPbrExtensions& pbr, PbrMap map);
        // whether the map is a colour (sRGB-decoded in the shader, as the base colour)
        static bool isColorMap(PbrMap map);
        // the stripped maps of a tier
        static bool isMapStripped(PbrMap map, PbrTier tier);

        // The variant of a material on the built-in Lit (or a fork with the S11 hooks): the
        // lighting it resolves to (VERTEX and NONE take no lobe; hex tiling is no lobe), whether
        // it draws a Terrain (no lobe, no material hex: its layers tile on their own) and the
        // tier. Shared by the renderer and the exporter's variant collector. reservedMaps: slots the
        // variant needs for something else (a 2D shadow atlas), taken off the budget.
        static PbrVariant getPbrVariant(const Material& material, MaterialLighting resolved, bool terrain, PbrTier tier, int reservedMaps = 0);
        // The tier a build draws with: FULL on desktop, the project's mobile tier on Android,
        // iOS and HTML5. Shared by the renderer and the exporter.
        static PbrTier resolveTier(bool mobileTarget, PbrTier mobileTier);
        // The tier of this build (set by the exported game from its export settings; FULL by
        // default and on desktop)
        static void setMobileTier(PbrTier tier);
        static PbrTier getMobileTier();
        static PbrTier getRuntimeTier();

        // u_fs_pbrExt as uploaded (UNIFORM_FLOATS floats, includes/pbr_ext.glsl)
        static void getUniformBlock(const MaterialPbrExtensions& pbr, float out[UNIFORM_FLOATS]);
        // the Hair shading model's three vec4 (hair0..2 of mesh.frag's u_fs_pbrParams)
        static const int HAIR_UNIFORM_FLOATS = 12;
        static void getHairUniformBlock(const MaterialPbrExtensions& pbr, float out[HAIR_UNIFORM_FLOATS]);
        // a Width (0..1) as the highlight's exponent: 2 / width^2 - 2, at least 1
        static float hairExponent(float width);
        static ShadingModel getShadingModel(const MaterialPbrExtensions& pbr);
        static void setShadingModel(MaterialPbrExtensions& pbr, ShadingModel model);

        // "lit_" + two letters per define of a builtin:lit reference (Cc Cm Cn Sh Sm An Am Ir Im
        // Tr Tm Vo Sp Sq Hx Th Lq Ts Hr Hf), the basename its compiled shaders are stored under
        static std::string getVariantBasename(const std::vector<std::string>& defines);
        // the defines of a basename made by getVariantBasename (false: not one)
        static bool parseVariantBasename(const std::string& basename, std::vector<std::string>& defines);
        // whether a define is one a builtin:lit reference takes (PBR_*, TERRAIN_HEX_TILING,
        // TERRAIN_HEX_STOCHASTIC, USE_LIT_QUALITY)
        static bool isBuiltinLitDefine(const std::string& define);
        // those defines as bits (one per define, in the basename's order) and back, sorted
        static uint32_t getDefineBits(const std::vector<std::string>& defines);
        static uint32_t getDefineBit(const std::string& define);
        static std::vector<std::string> getDefinesFromBits(uint32_t bits);

        // the High Lit Quality's define (SceneSettings::litQuality)
        static const char* const LIT_QUALITY_DEFINE;   // USE_LIT_QUALITY
        // a Terrain with a hex tiled layer (TerrainSurfaceLayer::hexTiling)
        static const char* const TERRAIN_HEX_DEFINE;   // TERRAIN_HEX_TILING
        // and a layer of it (or another) takes Stochastic Tiling: without it, the hex variant
        // leaves that path out (its registers would halve the variant's occupancy)
        static const char* const TERRAIN_HEX_STOCHASTIC_DEFINE;   // TERRAIN_HEX_STOCHASTIC
        static const char* const TRANSMISSION_DEFINE;  // PBR_TRANSMISSION
    };

}

#endif //MATERIALPBR_H
