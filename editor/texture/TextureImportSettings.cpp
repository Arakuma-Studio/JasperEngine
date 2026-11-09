// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TextureImportSettings.h"

#include "command/type/SetAssetMetaCmd.h"
#include "render/Render.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <locale>
#include <map>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    template<class E>
    struct EnumName {
        E value;
        const char* name;
    };

    const EnumName<TextureFamily> familyNames[] = {
        {TextureFamily::Desktop, "desktop"}, {TextureFamily::Mobile, "mobile"},
        {TextureFamily::AndroidEtc2, "android_etc2"}, {TextureFamily::Web, "web"}};
    const EnumName<TextureCookQuality> qualityNames[] = {
        {TextureCookQuality::None, "none"}, {TextureCookQuality::Low, "low"},
        {TextureCookQuality::Normal, "normal"}, {TextureCookQuality::High, "high"}};
    const EnumName<TextureEffort> effortNames[] = {
        {TextureEffort::Fast, "fast"}, {TextureEffort::Normal, "normal"}, {TextureEffort::Best, "best"}};
    const EnumName<MipFilter> filterNames[] = {
        {MipFilter::Box, "box"}, {MipFilter::Triangle, "triangle"}, {MipFilter::Mitchell, "mitchell"},
        {MipFilter::CatmullRom, "catmull_rom"}, {MipFilter::Nearest, "nearest"}};
    const EnumName<TextureAlphaSource> alphaSourceNames[] = {
        {TextureAlphaSource::Input, "input"}, {TextureAlphaSource::FromGray, "from_gray"}, {TextureAlphaSource::None, "none"}};
    const EnumName<TextureChannel> channelNames[] = {
        {TextureChannel::R, "r"}, {TextureChannel::G, "g"}, {TextureChannel::B, "b"}, {TextureChannel::A, "a"},
        {TextureChannel::Luma, "luma"}};
    const EnumName<TextureShape> shapeNames[] = {{TextureShape::Tex2D, "2d"}, {TextureShape::Cube, "cube"}};
    const EnumName<TextureFilterMode> filterModeNames[] = {
        {TextureFilterMode::Point, "point"}, {TextureFilterMode::Bilinear, "bilinear"}, {TextureFilterMode::Trilinear, "trilinear"}};
    const EnumName<TextureImportType> typeNames[] = {
        {TextureImportType::Default, "default"}, {TextureImportType::Normal, "normal"}, {TextureImportType::Sprite, "sprite"},
        {TextureImportType::UI, "ui"}, {TextureImportType::Lightmap, "lightmap"},
        {TextureImportType::SingleChannel, "single_channel"}, {TextureImportType::HDR, "hdr"}};

    template<class E, size_t N>
    const char* nameOf(const EnumName<E> (&table)[N], E value){
        for (const auto& entry : table)
            if (entry.value == value)
                return entry.name;
        return table[0].name;
    }

    template<class E, size_t N>
    bool valueOf(const EnumName<E> (&table)[N], const std::string& name, E& value){
        for (const auto& entry : table){
            if (name == entry.name){
                value = entry.value;
                return true;
            }
        }
        return false;
    }

    template<class E, size_t N>
    std::vector<std::string> namesOf(const EnumName<E> (&table)[N]){
        std::vector<std::string> out;
        for (const auto& entry : table)
            out.push_back(entry.name);
        return out;
    }

    std::string joined(const std::vector<std::string>& values){
        std::string out;
        for (const std::string& value : values){
            if (!out.empty()) out += ", ";
            out += value;
        }
        return out;
    }

    // the known keys of the section and of a platform block
    const char* const sectionKeys[] = {
        "version", "type", "inferredFrom", "shape", "srgb", "alphaSource", "alphaIsTransparency", "readable", "maxSize",
        "resizeFilter", "mipmaps", "mipFilter", "mipLimit", "preserveCoverage", "alphaCutoff", "flipGreen", "channel",
        "quality", "effort", "streamable", "shipSource", "platforms", "pixelArt", "alphaClip", "filterMode", "pixelsPerUnit"};

    bool isKnownKey(const std::string& key){
        return std::find_if(std::begin(sectionKeys), std::end(sectionKeys), [&](const char* k){ return key == k; }) != std::end(sectionKeys);
    }

    // a field read: absent stays absent, a wrong value is an error
    struct Reader {
        const YAML::Node& node;
        std::string& error;

        bool fail(const std::string& field, const std::string& why){
            if (error.empty()) error = field + ": " + why;
            return false;
        }
        bool boolean(const char* field, std::optional<bool>& out){
            if (!node[field]) return true;
            try { out = node[field].as<bool>(); return true; } catch (...) { return fail(field, "expected true or false"); }
        }
        bool integer(const char* field, std::optional<int>& out, int low, int high){
            if (!node[field]) return true;
            try{
                const int value = node[field].as<int>();
                if (value < low || value > high) return fail(field, "must be " + std::to_string(low) + "..." + std::to_string(high));
                out = value;
                return true;
            }catch (...){ return fail(field, "expected a whole number"); }
        }
        bool number(const char* field, std::optional<float>& out, float low, float high){
            if (!node[field]) return true;
            try{
                const float value = node[field].as<float>();
                if (!(value >= low && value <= high)) return fail(field, "must be " + std::to_string(low) + "..." + std::to_string(high));
                out = value;
                return true;
            }catch (...){ return fail(field, "expected a number"); }
        }
        template<class E, size_t N>
        bool enumeration(const char* field, const EnumName<E> (&table)[N], std::optional<E>& out){
            if (!node[field]) return true;
            std::string name;
            try { name = node[field].as<std::string>(); } catch (...) { name.clear(); }
            E value;
            if (!valueOf(table, name, value))
                return fail(field, "'" + name + "' is not one of " + joined(namesOf(table)));
            out = value;
            return true;
        }
    };

    std::string formatNumber(float value){
        return textureCanonicalNumber(value);
    }

    bool isPowerOfTwoSize(int value){
        return value >= 32 && value <= 16384 && (value & (value - 1)) == 0;
    }

}

std::string editor::textureCanonicalNumber(float value){
    // integers only: no locale (a decimal comma) and no printf rounding differences reach a key
    if (!std::isfinite(value))
        return "0";
    const long long scaled = std::llround((double)value * 1000000.0);
    const unsigned long long magnitude = (unsigned long long)(scaled < 0 ? -scaled : scaled);
    std::string text = (scaled < 0 ? "-" : "") + std::to_string(magnitude / 1000000ull);
    unsigned long long fraction = magnitude % 1000000ull;
    if (fraction != 0){
        std::string digits = std::to_string(fraction);
        digits.insert(0, 6 - digits.size(), '0');
        while (!digits.empty() && digits.back() == '0')
            digits.pop_back();
        text += "." + digits;
    }
    return text;
}

const char* editor::textureFamilyName(TextureFamily family){ return nameOf(familyNames, family); }
bool editor::textureFamilyFromName(const std::string& name, TextureFamily& family){ return valueOf(familyNames, name, family); }
const char* editor::textureQualityName(TextureCookQuality quality){ return nameOf(qualityNames, quality); }
const char* editor::textureEffortName(TextureEffort effort){ return nameOf(effortNames, effort); }
const char* editor::mipFilterName(MipFilter filter){ return nameOf(filterNames, filter); }
const char* editor::alphaSourceName(TextureAlphaSource source){ return nameOf(alphaSourceNames, source); }
const char* editor::textureChannelName(TextureChannel channel){ return nameOf(channelNames, channel); }
const char* editor::textureShapeName(TextureShape shape){ return nameOf(shapeNames, shape); }
const char* editor::textureFilterModeName(TextureFilterMode mode){ return nameOf(filterModeNames, mode); }
bool editor::textureFilterModeFromName(const std::string& name, TextureFilterMode& mode){ return valueOf(filterModeNames, name, mode); }

void editor::textureFilterModeFilters(TextureFilterMode mode, TextureFilter& minFilter, TextureFilter& magFilter){
    switch (mode){
        case TextureFilterMode::Point: minFilter = TextureFilter::NEAREST; magFilter = TextureFilter::NEAREST; return;
        case TextureFilterMode::Trilinear: minFilter = TextureFilter::LINEAR_MIPMAP_LINEAR; magFilter = TextureFilter::LINEAR; return;
        case TextureFilterMode::Bilinear: default: minFilter = TextureFilter::LINEAR; magFilter = TextureFilter::LINEAR; return;
    }
}

TextureFilter editor::textureFilterModeEngineFilter(TextureFilterMode mode){
    return mode == TextureFilterMode::Point ? TextureFilter::NEAREST : TextureFilter::LINEAR;
}

const char* editor::textureFamilyLabel(TextureFamily family){
    switch (family){
        case TextureFamily::Desktop: return "Desktop";
        case TextureFamily::Mobile: return "Mobile";
        case TextureFamily::AndroidEtc2: return "Android ETC2";
        case TextureFamily::Web: return "Web";
    }
    return "Desktop";
}

const std::vector<std::string>& editor::textureEnumValues(const std::string& field){
    static const std::map<std::string, std::vector<std::string>> values = {
        {"type", namesOf(typeNames)}, {"shape", namesOf(shapeNames)}, {"alphaSource", namesOf(alphaSourceNames)},
        {"resizeFilter", namesOf(filterNames)}, {"mipFilter", namesOf(filterNames)}, {"channel", namesOf(channelNames)},
        {"quality", namesOf(qualityNames)}, {"effort", namesOf(effortNames)}, {"platform", namesOf(familyNames)},
        {"filterMode", namesOf(filterModeNames)}};
    static const std::vector<std::string> none;
    auto it = values.find(field);
    return it == values.end() ? none : it->second;
}

bool TextureImportSettings::defaultSrgb(TextureImportType type){
    return type == TextureImportType::Default || type == TextureImportType::Sprite || type == TextureImportType::UI;
}

bool TextureImportSettings::defaultMipmaps(TextureImportType type){
    return type != TextureImportType::Sprite && type != TextureImportType::UI;
}

bool TextureImportSettings::defaultAlphaIsTransparency(TextureImportType type){
    return type == TextureImportType::Sprite || type == TextureImportType::UI;
}

TextureImportType TextureImportSettings::resolvedType() const{
    // Pixel Art without a written type is a sprite (S42 R4)
    return type.value_or(resolvedPixelArt() ? TextureImportType::Sprite : TextureImportType::Default);
}

TextureFilterMode TextureImportSettings::resolvedFilterMode(TextureFilterMode projectDefault) const{
    return filterMode.value_or(resolvedPixelArt() ? TextureFilterMode::Point : projectDefault);
}

bool TextureImportSettings::parse(const YAML::Node& node, TextureImportSettings& out, std::string* errorOut){
    out = TextureImportSettings();
    std::string error;
    if (!node || node.IsNull())
        return true;
    if (!node.IsMap()){
        if (errorOut) *errorOut = "textureImporter: expected a map";
        return false;
    }
    Reader r{node, error};
    std::optional<int> version;
    r.integer("version", version, 1, 1000000);
    out.version = version.value_or(currentVersion);
    r.enumeration("type", typeNames, out.type);
    if (node["inferredFrom"]){
        try { out.inferredFrom = node["inferredFrom"].as<std::string>(); } catch (...) { r.fail("inferredFrom", "expected text"); }
    }
    r.enumeration("shape", shapeNames, out.shape);
    r.boolean("srgb", out.srgb);
    r.enumeration("alphaSource", alphaSourceNames, out.alphaSource);
    r.boolean("alphaIsTransparency", out.alphaIsTransparency);
    r.boolean("readable", out.readable);
    r.integer("maxSize", out.maxSize, 32, 16384);
    r.enumeration("resizeFilter", filterNames, out.resizeFilter);
    r.boolean("mipmaps", out.mipmaps);
    r.enumeration("mipFilter", filterNames, out.mipFilter);
    r.integer("mipLimit", out.mipLimit, 0, 16);
    r.boolean("preserveCoverage", out.preserveCoverage);
    r.number("alphaCutoff", out.alphaCutoff, 0.0f, 1.0f);
    r.boolean("flipGreen", out.flipGreen);
    r.enumeration("channel", channelNames, out.channel);
    r.enumeration("quality", qualityNames, out.quality);
    r.enumeration("effort", effortNames, out.effort);
    r.boolean("streamable", out.streamable);
    r.boolean("shipSource", out.shipSource);
    r.boolean("pixelArt", out.pixelArt);
    r.boolean("alphaClip", out.alphaClip);
    r.enumeration("filterMode", filterModeNames, out.filterMode);
    r.integer("pixelsPerUnit", out.pixelsPerUnit, 1, 256);
    if (const YAML::Node platforms = node["platforms"]){
        if (!platforms.IsMap()){
            r.fail("platforms", "expected a map of desktop, mobile, android_etc2, web");
        }else{
            for (const auto& entry : platforms){
                const std::string name = entry.first.as<std::string>();
                TextureFamily family;
                if (!textureFamilyFromName(name, family)){
                    r.fail("platforms." + name, "not one of " + joined(namesOf(familyNames)));
                    continue;
                }
                const YAML::Node block = entry.second;
                if (!block.IsMap()){
                    r.fail("platforms." + name, "expected a map");
                    continue;
                }
                TexturePlatformSettings& p = out.platforms[(size_t)family];
                Reader pr{block, error};
                std::optional<bool> overrides;
                pr.boolean("override", overrides);
                p.override = overrides.value_or(false);
                pr.integer("maxSize", p.maxSize, 32, 16384);
                if (block["format"]){
                    try { p.format = block["format"].as<std::string>(); } catch (...) { pr.fail("format", "expected a format name"); }
                }
                pr.enumeration("quality", qualityNames, p.quality);
                pr.enumeration("effort", effortNames, p.effort);
                pr.number("rdo", p.rdo, 0.0f, 255.0f);
            }
        }
    }
    // keys of a newer editor or of hands that know more: kept as they are
    out.unknown = YAML::Node(YAML::NodeType::Map);
    for (const auto& entry : node){
        const std::string key = entry.first.as<std::string>();
        if (!isKnownKey(key))
            out.unknown[key] = YAML::Clone(entry.second);
    }
    if (!error.empty()){
        if (errorOut) *errorOut = error;
        return false;
    }
    return true;
}

YAML::Node TextureImportSettings::serialize() const{
    YAML::Node node(YAML::NodeType::Map);
    node["version"] = version;
    if (type) node["type"] = nameOf(typeNames, *type);
    if (!inferredFrom.empty()) node["inferredFrom"] = inferredFrom;
    if (shape) node["shape"] = nameOf(shapeNames, *shape);
    if (srgb) node["srgb"] = *srgb;
    if (alphaSource) node["alphaSource"] = nameOf(alphaSourceNames, *alphaSource);
    if (alphaIsTransparency) node["alphaIsTransparency"] = *alphaIsTransparency;
    if (readable) node["readable"] = *readable;
    if (maxSize) node["maxSize"] = *maxSize;
    if (resizeFilter) node["resizeFilter"] = nameOf(filterNames, *resizeFilter);
    if (mipmaps) node["mipmaps"] = *mipmaps;
    if (mipFilter) node["mipFilter"] = nameOf(filterNames, *mipFilter);
    if (mipLimit) node["mipLimit"] = *mipLimit;
    if (preserveCoverage) node["preserveCoverage"] = *preserveCoverage;
    if (alphaCutoff) node["alphaCutoff"] = *alphaCutoff;
    if (flipGreen) node["flipGreen"] = *flipGreen;
    if (channel) node["channel"] = nameOf(channelNames, *channel);
    if (quality) node["quality"] = nameOf(qualityNames, *quality);
    if (effort) node["effort"] = nameOf(effortNames, *effort);
    if (streamable) node["streamable"] = *streamable;
    if (shipSource) node["shipSource"] = *shipSource;
    if (pixelArt) node["pixelArt"] = *pixelArt;
    if (alphaClip) node["alphaClip"] = *alphaClip;
    if (filterMode) node["filterMode"] = nameOf(filterModeNames, *filterMode);
    if (pixelsPerUnit) node["pixelsPerUnit"] = *pixelsPerUnit;
    YAML::Node platformsNode(YAML::NodeType::Map);
    for (size_t f = 0; f < kTextureFamilyCount; f++){
        const TexturePlatformSettings& p = platforms[f];
        if (p.empty())
            continue;
        YAML::Node block(YAML::NodeType::Map);
        block.SetStyle(YAML::EmitterStyle::Flow);
        block["override"] = p.override;
        if (p.maxSize) block["maxSize"] = *p.maxSize;
        if (p.format) block["format"] = *p.format;
        if (p.quality) block["quality"] = nameOf(qualityNames, *p.quality);
        if (p.effort) block["effort"] = nameOf(effortNames, *p.effort);
        if (p.rdo) block["rdo"] = *p.rdo;
        platformsNode[nameOf(familyNames, (TextureFamily)f)] = block;
    }
    if (platformsNode.size() > 0)
        node["platforms"] = platformsNode;
    if (unknown && unknown.IsMap()){
        for (const auto& entry : unknown)
            node[entry.first.as<std::string>()] = YAML::Clone(entry.second);
    }
    return node;
}

bool TextureImportSettings::operator==(const TextureImportSettings& other) const{
    return YAML::Dump(serialize()) == YAML::Dump(other.serialize());
}

bool TextureImportSettings::load(const std::string& absoluteAsset, TextureImportSettings& out, std::string* error){
    const YAML::Node node = SetAssetMetaCmd::read(absoluteAsset, metaKey);
    if (!node || node.IsNull()){
        out = TextureImportSettings();
        return false;
    }
    return parse(node, out, error);
}

ResolvedTextureSettings TextureImportSettings::resolve(TextureFamily family, const TextureProjectSettings& project) const{
    ResolvedTextureSettings r;
    const TexturePlatformSettings& p = platforms[(size_t)family];
    const bool overrides = p.override;
    r.type = resolvedType();
    r.shape = shape.value_or(TextureShape::Tex2D);
    r.srgb = srgb.value_or(defaultSrgb(r.type));
    r.alphaSource = alphaSource.value_or(TextureAlphaSource::Input);
    // Pixel Art (S42 R4): absent fields take point-sampled, unmipmapped, lossless, nearest-resized,
    // binary-alpha defaults; explicit fields and platform overrides still win
    const bool pixel = resolvedPixelArt();
    r.alphaIsTransparency = alphaIsTransparency.value_or(pixel || defaultAlphaIsTransparency(r.type));
    r.readable = readable.value_or(false);
    r.maxSize = (overrides && p.maxSize) ? *p.maxSize : maxSize.value_or(project.defaultMaxSize[(size_t)family]);
    r.resizeFilter = resizeFilter.value_or(pixel ? MipFilter::Nearest : MipFilter::Mitchell);
    r.mipmaps = mipmaps.value_or(!pixel && defaultMipmaps(r.type));
    r.mipFilter = mipFilter.value_or(pixel ? MipFilter::Nearest : MipFilter::Mitchell);
    r.mipLimit = mipLimit.value_or(0);
    r.preserveCoverage = preserveCoverage.value_or(false);
    r.alphaCutoff = alphaCutoff.value_or(0.5f);
    r.alphaClip = alphaClip.value_or(pixel);
    r.flipGreen = flipGreen.value_or(false);
    r.channel = channel.value_or(TextureChannel::R);
    r.quality = (overrides && p.quality) ? *p.quality
                                         : quality.value_or(pixel ? TextureCookQuality::None : project.defaultQuality[(size_t)family]);
    r.effort = (overrides && p.effort) ? *p.effort : effort.value_or(TextureEffort::Normal);
    // sprites and UI never lose mips to the quality setting or the budget (R12)
    const bool layoutType = r.type == TextureImportType::Sprite || r.type == TextureImportType::UI;
    r.streamable = !layoutType && r.mipmaps && streamable.value_or(!pixel);
    r.format = (overrides && p.format) ? *p.format : std::string();
    r.rdo = (overrides && p.rdo) ? *p.rdo : -1.0f;
    r.strategy = project.strategy;
    return r;
}

TextureProcessing ResolvedTextureSettings::processing() const{
    TextureProcessing p;
    p.type = type;
    p.srgb = srgb;
    p.alphaSource = alphaSource;
    p.alphaIsTransparency = alphaIsTransparency;
    p.maxSize = maxSize;
    p.resizeFilter = resizeFilter;
    p.mipmaps = mipmaps;
    p.mipFilter = mipFilter;
    p.mipLimit = mipLimit;
    p.preserveCoverage = preserveCoverage;
    p.alphaCutoff = alphaCutoff;
    p.alphaClip = alphaClip;
    p.flipGreen = flipGreen;
    p.channel = channel;
    p.keepSourceChannels = cpuReadable;
    p.strategy = strategy;
    p.cube = shape == TextureShape::Cube;
    return p;
}

std::string ResolvedTextureSettings::canonical() const{
    // every field, in a fixed order, with fixed number formatting: the key must not move
    // when the code that prints it does
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << "{\"type\":\"" << textureImportTypeName(type) << "\""
        << ",\"shape\":\"" << textureShapeName(shape) << "\""
        << ",\"srgb\":" << (srgb ? 1 : 0)
        << ",\"alphaSource\":\"" << alphaSourceName(alphaSource) << "\""
        << ",\"alphaIsTransparency\":" << (alphaIsTransparency ? 1 : 0)
        << ",\"readable\":" << (readable ? 1 : 0)
        << ",\"maxSize\":" << maxSize
        << ",\"resizeFilter\":\"" << mipFilterName(resizeFilter) << "\""
        << ",\"mipmaps\":" << (mipmaps ? 1 : 0)
        << ",\"mipFilter\":\"" << mipFilterName(mipFilter) << "\""
        << ",\"mipLimit\":" << mipLimit
        << ",\"preserveCoverage\":" << (preserveCoverage ? 1 : 0)
        << ",\"alphaCutoff\":" << formatNumber(alphaCutoff)
        << ",\"flipGreen\":" << (flipGreen ? 1 : 0)
        << ",\"channel\":\"" << textureChannelName(channel) << "\""
        << ",\"quality\":\"" << textureQualityName(quality) << "\""
        << ",\"effort\":\"" << textureEffortName(effort) << "\""
        << ",\"streamable\":" << (streamable ? 1 : 0)
        << ",\"format\":\"" << format << "\""
        << ",\"rdo\":" << formatNumber(rdo)
        << ",\"cpuReadable\":" << (cpuReadable ? 1 : 0)
        << ",\"strategy\":" << (int)strategy;
    // fields added after the key's first version print only when they differ from what every
    // older texture resolves to, so those keep their cook keys (S42 R8)
    if (alphaClip)
        out << ",\"alphaClip\":1";
    out << "}";
    return out.str();
}

const char* editor::texturePlatformGroupName(size_t group){
    static const char* const names[] = {"desktop", "mobile", "web"};
    return group < 3 ? names[group] : "desktop";
}

YAML::Node editor::encodeTextureProjectSettings(const TextureProjectSettings& settings){
    const TextureProjectSettings defaults;
    YAML::Node node(YAML::NodeType::Map);
    YAML::Node families(YAML::NodeType::Map);
    for (size_t f = 0; f < kTextureFamilyCount; f++){
        YAML::Node family(YAML::NodeType::Map);
        if (settings.defaultQuality[f] != defaults.defaultQuality[f])
            family["quality"] = textureQualityName(settings.defaultQuality[f]);
        if (settings.defaultMaxSize[f] != defaults.defaultMaxSize[f])
            family["maxSize"] = settings.defaultMaxSize[f];
        if (family.size())
            families[textureFamilyName((TextureFamily)f)] = family;
    }
    if (families.size())
        node["defaults"] = families;
    if (settings.androidEtc2Fallback != defaults.androidEtc2Fallback)
        node["androidEtc2Fallback"] = settings.androidEtc2Fallback;
    YAML::Node runtime(YAML::NodeType::Map);
    for (size_t g = 0; g < 3; g++){
        YAML::Node group(YAML::NodeType::Map);
        if (settings.textureQuality[g] != defaults.textureQuality[g])
            group["textureQuality"] = settings.textureQuality[g];
        if (settings.memoryBudgetMB[g] != defaults.memoryBudgetMB[g])
            group["memoryBudgetMB"] = settings.memoryBudgetMB[g];
        if (settings.lowMemoryFallback[g] != defaults.lowMemoryFallback[g])
            group["lowMemoryFallback"] = (bool)settings.lowMemoryFallback[g];
        if (group.size())
            runtime[texturePlatformGroupName(g)] = group;
    }
    if (runtime.size())
        node["platforms"] = runtime;
    if (settings.cacheSizeMB != defaults.cacheSizeMB)
        node["cacheSizeMB"] = settings.cacheSizeMB;
    if (settings.globalCacheDir != defaults.globalCacheDir)
        node["globalCacheDir"] = settings.globalCacheDir;
    return node;
}

void editor::decodeTextureProjectSettings(const YAML::Node& node, TextureProjectSettings& settings){
    settings = TextureProjectSettings();
    if (!node || !node.IsMap())
        return;
    auto asInt = [](const YAML::Node& value, int fallback){
        try { return value ? value.as<int>() : fallback; } catch (...) { return fallback; }
    };
    auto asBool = [](const YAML::Node& value, bool fallback){
        try { return value ? value.as<bool>() : fallback; } catch (...) { return fallback; }
    };
    if (const YAML::Node families = node["defaults"]; families && families.IsMap()){
        for (size_t f = 0; f < kTextureFamilyCount; f++){
            const YAML::Node family = families[textureFamilyName((TextureFamily)f)];
            if (!family || !family.IsMap())
                continue;
            if (family["quality"]){
                std::string name;
                try { name = family["quality"].as<std::string>(); } catch (...) { name.clear(); }
                TextureCookQuality quality;
                if (valueOf(qualityNames, name, quality))
                    settings.defaultQuality[f] = quality;
            }
            const int maxSize = asInt(family["maxSize"], settings.defaultMaxSize[f]);
            if (isPowerOfTwoSize(maxSize))
                settings.defaultMaxSize[f] = maxSize;
        }
    }
    settings.androidEtc2Fallback = asBool(node["androidEtc2Fallback"], settings.androidEtc2Fallback);
    if (const YAML::Node runtime = node["platforms"]; runtime && runtime.IsMap()){
        for (size_t g = 0; g < 3; g++){
            const YAML::Node group = runtime[texturePlatformGroupName(g)];
            if (!group || !group.IsMap())
                continue;
            settings.textureQuality[g] = std::clamp(asInt(group["textureQuality"], settings.textureQuality[g]), 0, 3);
            settings.memoryBudgetMB[g] = std::clamp(asInt(group["memoryBudgetMB"], settings.memoryBudgetMB[g]), 0, 1 << 20);
            settings.lowMemoryFallback[g] = asBool(group["lowMemoryFallback"], settings.lowMemoryFallback[g]);
        }
    }
    settings.cacheSizeMB = std::clamp(asInt(node["cacheSizeMB"], settings.cacheSizeMB), 64, 1 << 20);
    if (node["globalCacheDir"]){
        try { settings.globalCacheDir = node["globalCacheDir"].as<std::string>(); } catch (...) { settings.globalCacheDir.clear(); }
    }
}

bool editor::validateTextureSettingsField(const std::string& field, const YAML::Node& value, std::string& error){
    YAML::Node wrapper(YAML::NodeType::Map);
    wrapper[field] = value;
    if (field == "platforms"){
        TextureImportSettings scratch;
        return TextureImportSettings::parse(wrapper, scratch, &error);
    }
    if (!isKnownKey(field) || field == "version"){
        error = field + ": not an import setting";
        return false;
    }
    if (field == "maxSize"){
        try{
            const int size = value.as<int>();
            if (!isPowerOfTwoSize(size)){
                error = "maxSize: must be a power of two, 32...16384";
                return false;
            }
        }catch (...){
            error = "maxSize: expected a whole number";
            return false;
        }
    }
    TextureImportSettings scratch;
    return TextureImportSettings::parse(wrapper, scratch, &error);
}
