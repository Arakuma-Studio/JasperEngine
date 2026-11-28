// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Fog Profile asset (docs/specs/S101-vapor.md R1-R6, AC1, AC2): its canonical JSON, the warning for
// what it holds that this engine does not read, overrides by presence, the built-ins against the
// presets they re-express, and the library a game reads them through.

#include "render/FogProfile.h"
#include "render/FogResolve.h"
#include "render/AtmospherePresets.h"

#include "LogCapture.h"
#include "TempDir.h"

#include <gtest/gtest.h>

#include <cstring>
#include <set>
#include <string>

namespace fog_profile_test {

using namespace jasperengine;
using namespace jasperengine::test;


    // every look field away from its default, so a field the writer or the reader forgot shows
    FogProfileData everyField(){
        FogComponent fog;
        fog.color = Vector3(0.1f, 0.2f, 0.3f);
        fog.density = 0.03f;
        fog.linearStart = 4.0f;
        fog.linearEnd = 90.5f;
        fog.colorMode = FogColorMode::LIT;
        fog.attenuationDistance = 123.25f;
        fog.baseHeight = -2.0f;
        fog.maximumHeight = 77.0f;
        fog.startDistance = 1.5f;
        fog.maxFogDistance = 4321.0f;
        fog.maxOpacity = 0.7f;
        fog.directionalInscattering = 0.9f;
        fog.anisotropy = -0.3f;
        fog.skyFill = false;
        fog.skyFillAngle = 11.0f;
        fog.noiseIntensity = 0.45f;
        fog.noiseScale = 33.0f;
        fog.wind = Vector3(1.0f, -0.5f, 0.25f);
        fog.albedo = Vector3(0.05f, 0.06f, 0.07f);
        fog.ambientMultiplier = 3.0f;
        fog.emissive = Vector3(2.5f, 0.0f, 0.125f);
        FogProfileData data = FogProfileData::fromFog(fog, "Every \"Field\"");
        data.description = "all of them\nset";
        return data;
    }

}

namespace fog_profile_test {

using namespace fog_profile_test;

TEST(FogProfileTest, RoundTripCanonical){
    const FogProfileData data = everyField();
    ASSERT_EQ(data.set.count(), FOG_FIELD_COUNT);
    const std::string text = FogProfileIO::write(data);

    FogProfileData read;
    std::string error;
    std::vector<std::string> unknown;
    std::vector<std::string> problems;
    ASSERT_TRUE(FogProfileIO::parse(text, read, &error, &unknown, &problems)) << error;
    EXPECT_TRUE(unknown.empty());
    EXPECT_TRUE(problems.empty());
    EXPECT_EQ(read, data);
    // canonical: what it reads writes the same bytes again
    EXPECT_EQ(FogProfileIO::write(read), text);

    // fields in R2's order, whatever order the file had them in
    size_t last = 0;
    for (size_t i = 0; i < FOG_FIELD_COUNT; i++){
        const size_t at = text.find(std::string("\"") + FogFields::name(static_cast<FogField>(i)) + "\":");
        ASSERT_NE(at, std::string::npos) << FogFields::name(static_cast<FogField>(i));
        EXPECT_GT(at, last) << FogFields::name(static_cast<FogField>(i)) << " out of order";
        last = at;
    }
    const std::string shuffled = R"({"fields": {"wind": [1, -0.5, 0.25], "color": [0.1, 0.2, 0.3]}, "name": "N", "version": 1})";
    FogProfileData two;
    ASSERT_TRUE(FogProfileIO::parse(shuffled, two));
    EXPECT_EQ(FogProfileIO::write(two),
              "{\n  \"version\": 1,\n  \"name\": \"N\",\n  \"fields\": {\n    \"color\": [0.1, 0.2, 0.3],\n"
              "    \"wind\": [1.0, -0.5, 0.25]\n  }\n}\n");

    // floats as their shortest text (0.1, not 0.10000000149011612), the colour mode by name
    EXPECT_NE(text.find("\"density\": 0.03,"), std::string::npos) << text;
    EXPECT_NE(text.find("\"colorMode\": \"lit\""), std::string::npos) << text;
    EXPECT_NE(text.find("\"skyFill\": false"), std::string::npos) << text;

    // a newer version does not read (R6: an empty profile)
    EXPECT_FALSE(FogProfileIO::parse(R"({"version": 2, "fields": {}})", read, &error));
    EXPECT_NE(error.find("newer"), std::string::npos) << error;
    EXPECT_FALSE(FogProfileIO::parse("not json", read, &error));

    // written through AtomicFile, and the file reads back the same
    TempDir dir;
    const std::filesystem::path file = dir.file("FogProfiles/every.fogprofile");
    ASSERT_TRUE(FogProfileIO::save(file, data, &error)) << error;
    EXPECT_EQ(dir.read("FogProfiles/every.fogprofile"), text);
}

TEST(FogProfileTest, UnknownFieldWarnsOnce){
    TempDir dir;
    const std::string path = dir.write("future.fogprofile",
        R"({"version": 1, "name": "Future", "shading": "froxel",
            "fields": {"density": 0.5, "glint": 3, "sparkle": [1, 2, 3], "maxOpacity": "a lot"}})").generic_string();
    FogProfileLibrary::clear();
    LogCapture log;
    std::shared_ptr<const FogProfileData> profile = FogProfileLibrary::get(path);
    ASSERT_TRUE(profile);
    // the fields it knows are read; the others are ignored, named in one warning for the file
    EXPECT_TRUE(profile->isSet(FogField::Density));
    EXPECT_FLOAT_EQ(profile->values.density, 0.5f);
    EXPECT_FALSE(profile->isSet(FogField::MaxOpacity));
    EXPECT_EQ(log.count(D_LOG_WARN, "unknown fields ignored"), 1u);
    EXPECT_EQ(log.count(D_LOG_WARN, "fields.glint"), 1u);
    EXPECT_EQ(log.count(D_LOG_WARN, "fields.sparkle"), 1u);
    EXPECT_EQ(log.count(D_LOG_WARN, "shading"), 1u);
    // a known field of the wrong type is its own warning, and is left unset
    EXPECT_EQ(log.count(D_LOG_WARN, "fields.maxOpacity"), 1u);

    // read once: every later use takes the kept copy, without a warning
    for (int i = 0; i < 3; i++)
        EXPECT_EQ(FogProfileLibrary::get(path), profile);
    EXPECT_EQ(log.count(D_LOG_WARN, "unknown fields ignored"), 1u);

    // a reload (the file watcher) reads it again: one warning for that read
    FogProfileLibrary::reload(path);
    EXPECT_NE(FogProfileLibrary::get(path), nullptr);
    EXPECT_EQ(log.count(D_LOG_WARN, "unknown fields ignored"), 2u);
    FogProfileLibrary::clear();
}

TEST(FogProfileTest, AbsentFieldIsNotOverride){
    FogProfileData profile;
    ASSERT_TRUE(FogProfileIO::parse(R"({"version": 1, "name": "Thin", "fields": {"attenuationDistance": 50, "skyFill": false}})", profile));
    EXPECT_EQ(profile.set.count(), 2u);
    EXPECT_TRUE(profile.isSet(FogField::AttenuationDistance));
    EXPECT_TRUE(profile.isSet(FogField::SkyFill));
    EXPECT_FALSE(profile.isSet(FogField::Color));

    // a field the profile does not set is the Fog's own, whatever the profile's values hold
    FogComponent fog;
    fog.color = Vector3(0.9f, 0.1f, 0.1f);
    fog.attenuationDistance = 999.0f;
    fog.skyFill = true;
    FogResolve resolve;
    resolveFog(fog, &profile, nullptr, {}, nullptr, resolve);
    EXPECT_EQ(resolve.resolved.color, fog.color);
    EXPECT_EQ(resolve.sourceOf(FogField::Color).layer, FogLayer::Fog);
    EXPECT_FLOAT_EQ(resolve.resolved.attenuationDistance, 50.0f);
    EXPECT_EQ(resolve.sourceOf(FogField::AttenuationDistance).layer, FogLayer::Profile);
    EXPECT_FALSE(resolve.resolved.skyFill);

    // an empty profile writes an empty fields object and changes nothing
    FogProfileData empty;
    empty.name = "Nothing";
    FogProfileData reread;
    ASSERT_TRUE(FogProfileIO::parse(FogProfileIO::write(empty), reread));
    EXPECT_TRUE(reread.set.none());
    resolveFog(fog, &reread, nullptr, {}, nullptr, resolve);
    for (size_t i = 0; i < FOG_FIELD_COUNT; i++)
        EXPECT_TRUE(FogFields::equal(resolve.resolved, fog, static_cast<FogField>(i))) << FogFields::name(static_cast<FogField>(i));

    // clearing a field removes the override
    profile.clear(FogField::AttenuationDistance);
    EXPECT_FALSE(profile.isSet(FogField::AttenuationDistance));
    EXPECT_EQ(FogProfileIO::write(profile).find("attenuationDistance"), std::string::npos);

    // a missing file is an empty profile (R6): the library names why
    FogProfileLibrary::clear();
    LogCapture log;
    EXPECT_EQ(FogProfileLibrary::get("FogProfiles/missing.fogprofile"), nullptr);
    std::string reason;
    EXPECT_TRUE(FogProfileLibrary::isMissing("FogProfiles/missing.fogprofile", &reason));
    EXPECT_FALSE(reason.empty());
    EXPECT_EQ(log.count(D_LOG_WARN, "missing.fogprofile"), 1u);
    FogProfileLibrary::clear();
}

TEST(FogProfileTest, BuiltinsMatchLegacyPresets){
    // the five presets are built-ins of the same ids, and four new looks follow
    std::set<std::string> ids;
    for (const FogProfileBuiltin& builtin : FogProfiles::builtins())
        ids.insert(builtin.id);
    for (const AtmospherePreset& preset : AtmospherePresets::getFogPresets())
        EXPECT_EQ(ids.count(preset.id), 1u) << preset.id;
    for (const char* id : {"bright_haze", "murky", "infernal", "toxic"})
        EXPECT_EQ(ids.count(id), 1u) << id;
    EXPECT_EQ(ids.size(), 9u);

    // A Height fog linked to a legacy built-in resolves to exactly what applyFogPreset writes,
    // from a fog with every field away from what the preset sets
    for (const AtmospherePreset& preset : AtmospherePresets::getFogPresets()){
        FogComponent fog;
        fog.type = FogType::HEIGHT;
        fog.color = Vector3(0.3f, 0.9f, 0.1f);
        fog.attenuationDistance = 7.0f;
        fog.baseHeight = 55.0f;
        fog.maximumHeight = 66.0f;
        fog.startDistance = 77.0f;
        fog.maxFogDistance = 88.0f;
        fog.maxOpacity = 0.11f;
        fog.directionalInscattering = 3.0f;
        fog.anisotropy = -0.9f;
        fog.skyFill = false;
        fog.skyFillAngle = 45.0f;
        fog.colorMode = FogColorMode::SKY;
        fog.volumetric = true;
        fog.noiseIntensity = 0.5f;

        FogComponent expected = fog;
        ASSERT_TRUE(AtmospherePresets::applyFogPreset(preset.id, expected)) << preset.id;

        const FogProfileBuiltin* builtin = FogProfiles::findBuiltin(preset.id);
        ASSERT_NE(builtin, nullptr) << preset.id;
        EXPECT_EQ(builtin->data.name, preset.name);
        EXPECT_EQ(builtin->data.description, preset.description);
        EXPECT_EQ(FogProfiles::findBuiltin("builtin:" + preset.id), builtin);
        FogResolve resolve;
        resolveFog(fog, &builtin->data, nullptr, {}, nullptr, resolve);
        for (size_t i = 0; i < FOG_FIELD_COUNT; i++){
            const FogField field = static_cast<FogField>(i);
            EXPECT_TRUE(FogFields::equal(resolve.resolved, expected, field)) << preset.id << "." << FogFields::name(field);
        }
        // the fields beyond the look are the Fog's own, as the preset leaves them
        EXPECT_EQ(resolve.resolved.volumetric, expected.volumetric);
        EXPECT_EQ(resolve.resolved.type, FogType::HEIGHT);

        // the copy the editor writes reads back as the built-in, bit for bit
        FogProfileData copy;
        ASSERT_TRUE(FogProfileIO::parse(FogProfileIO::write(builtin->data), copy)) << preset.id;
        EXPECT_EQ(copy, builtin->data) << preset.id;

        // and the library serves it without a file
        std::shared_ptr<const FogProfileData> served = FogProfileLibrary::get(FogProfiles::builtinKey(preset.id));
        ASSERT_TRUE(served) << preset.id;
        EXPECT_EQ(*served, builtin->data);
    }
    // the new looks are Lit, with noise and wind
    for (const char* id : {"bright_haze", "murky", "infernal", "toxic"}){
        const FogProfileBuiltin* builtin = FogProfiles::findBuiltin(id);
        ASSERT_NE(builtin, nullptr);
        EXPECT_TRUE(builtin->data.isSet(FogField::ColorMode)) << id;
        EXPECT_EQ(builtin->data.values.colorMode, FogColorMode::LIT) << id;
        EXPECT_GT(builtin->data.values.noiseIntensity, 0.0f) << id;
        EXPECT_GT(builtin->data.values.wind.length(), 0.0f) << id;
        EXPECT_EQ(FogProfiles::builtinCopyPath(id), std::string("FogProfiles/") + id + ".fogprofile");
    }
    EXPECT_EQ(FogProfiles::findBuiltin("builtin:nope"), nullptr);
    EXPECT_EQ(FogProfileLibrary::get("builtin:nope"), nullptr);
}

TEST(FogProfileTest, LibraryServesEditsOnTheNextLookUp){
    // R5: a set (the editor's write) or a reload (the watcher) is what the next look-up returns
    FogProfileLibrary::clear();
    TempDir dir;
    const std::string path = dir.write("edit.fogprofile", R"({"version": 1, "name": "A", "fields": {"density": 0.1}})").generic_string();
    std::shared_ptr<const FogProfileData> first = FogProfileLibrary::get(path);
    ASSERT_TRUE(first);
    const uint32_t version = FogProfileLibrary::version(path);
    EXPECT_NE(version, 0u);

    FogProfileData edited = *first;
    edited.values.density = 0.2f;
    FogProfileLibrary::set(path, edited);
    EXPECT_FLOAT_EQ(FogProfileLibrary::get(path)->values.density, 0.2f);
    EXPECT_NE(FogProfileLibrary::version(path), version);

    dir.write("edit.fogprofile", R"({"version": 1, "name": "A", "fields": {"density": 0.3}})");
    FogProfileLibrary::reload(path);
    EXPECT_FLOAT_EQ(FogProfileLibrary::get(path)->values.density, 0.3f);

    // the same file by another spelling of its path
    EXPECT_EQ(FogProfileLibrary::get(dir.str() + "/./edit.fogprofile"), FogProfileLibrary::get(path));

    // runtime profiles live until the runtime resets
    const std::string key = FogProfileLibrary::addRuntime(std::make_shared<FogProfileData>(edited));
    EXPECT_TRUE(FogProfileLibrary::isRuntimeKey(key));
    EXPECT_TRUE(FogProfileLibrary::get(key));
    FogProfileLibrary::resetRuntime();
    EXPECT_FALSE(FogProfileLibrary::get(key));
    FogProfileLibrary::clear();
}

}  // namespace fog_profile_test
