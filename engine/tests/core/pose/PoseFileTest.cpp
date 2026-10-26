// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// .poseanim and .humanoid file helpers (PoseFile.h). They read and write through the engine file
// API, which needs the engine's System: the core tests' (TestSystem); the tests use absolute paths.

#include <gtest/gtest.h>

#include "io/File.h"
#include "pose/HumanBones.h"
#include "pose/ModelRig.h"
#include "pose/PoseFile.h"
#include "PoseTestUtil.h"

#include <fstream>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::pose;

namespace {

    PoseAnimationData sampleData(){
        PoseAnimationData data;
        data.previewModel = "models/hero.gltf";
        data.settings.loop = false;
        data.settings.stepRate = 12.5f;
        PoseKey key;
        key.name = "Contact";
        key.hold = 0.375f;
        key.body.offset = Vector3(0.0625f, 0, -0.5f);
        key.body.angles[posetest::dof("left_upper_arm.raise")] = 8.5f;
        data.poses.push_back(key);
        return data;
    }

    std::string slurp(const std::string& path){
        std::ifstream file(path, std::ios::binary);
        std::stringstream content;
        content << file.rdbuf();
        return content.str();
    }

}

TEST(PoseFile, SaveAndLoadPoseAnimation){
    posetest::ScratchDir dir;
    const std::string path = dir.file("walk.poseanim");
    const PoseAnimationData data = sampleData();
    const PoseAnimationTemplateInfo info{"T", "D", "C"};

    std::string error;
    ASSERT_TRUE(savePoseAnimationFile(path, data, &error, &info)) << error;
    EXPECT_FALSE(std::filesystem::exists(path + ".tmp"));
    EXPECT_EQ(slurp(path), writePoseAnimation(data, &info));

    PoseAnimationData loaded;
    std::vector<std::string> warnings;
    std::optional<PoseAnimationTemplateInfo> loadedInfo;
    ASSERT_TRUE(loadPoseAnimationFile(path, loaded, warnings, &error, &loadedInfo)) << error;
    EXPECT_TRUE(warnings.empty());
    EXPECT_EQ(writePoseAnimation(loaded, &*loadedInfo), writePoseAnimation(data, &info));
    ASSERT_TRUE(loadedInfo.has_value());
    EXPECT_EQ(loadedInfo->name, "T");

    // overwriting replaces the content
    PoseAnimationData other;
    other.previewModel = "other.gltf";
    ASSERT_TRUE(savePoseAnimationFile(path, other, &error)) << error;
    ASSERT_TRUE(loadPoseAnimationFile(path, loaded, warnings, &error)) << error;
    EXPECT_EQ(loaded.previewModel, "other.gltf");
    EXPECT_TRUE(loaded.poses.empty());
}

TEST(PoseFile, LoadPoseAnimationErrors){
    posetest::ScratchDir dir;
    PoseAnimationData data;
    std::vector<std::string> warnings;
    std::string error;
    EXPECT_FALSE(loadPoseAnimationFile(dir.file("missing.poseanim"), data, warnings, &error));
    EXPECT_NE(error.find("missing.poseanim"), std::string::npos) << error;

    const std::string bad = dir.file("bad.poseanim");
    {
        std::ofstream file(bad);
        file << "{\"format\": \"jasperengine.poseanim\", \"version\": 7}";
    }
    EXPECT_FALSE(loadPoseAnimationFile(bad, data, warnings, &error));
    EXPECT_NE(error.find("bad.poseanim"), std::string::npos) << error;
    EXPECT_NE(error.find("version"), std::string::npos) << error;

    // an unwritable target reports an error and leaves no temporary file
    error.clear();
    const std::string unwritable = dir.file("no-such-dir/x.poseanim");
    EXPECT_FALSE(savePoseAnimationFile(unwritable, data, &error));
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(std::filesystem::exists(unwritable + ".tmp"));
}

TEST(PoseFile, ReadTextFileDistinguishesMissingFromEmpty){
    posetest::ScratchDir dir;
    std::string text = "stale";
    std::string error;
    bool notFound = false;
    EXPECT_FALSE(readTextFile(dir.file("none.txt"), text, &error, &notFound));
    EXPECT_TRUE(notFound);
    EXPECT_NE(error.find("none.txt"), std::string::npos) << error;

    ASSERT_TRUE(writeTextFile(dir.file("empty.txt"), "", &error)) << error;
    notFound = true;
    ASSERT_TRUE(readTextFile(dir.file("empty.txt"), text, &error, &notFound)) << error;
    EXPECT_FALSE(notFound);
    EXPECT_EQ(text, "");

    ASSERT_TRUE(writeTextFile(dir.file("data.txt"), "line 1\nline 2\n", &error)) << error;
    ASSERT_TRUE(readTextFile(dir.file("data.txt"), text, &error)) << error;
    EXPECT_EQ(text, "line 1\nline 2\n");
}

TEST(PoseFile, Utf8PathsAreWrittenAndRead){
    posetest::ScratchDir dir;
    const std::string folder = "h\xC3\xA9ro \xE2\x80\x93 animations";
    const std::string path = dir.file(folder + "/walk.poseanim");
    std::filesystem::create_directories(posetest::utf8Path(dir.file(folder)));

    std::string error;
    ASSERT_TRUE(savePoseAnimationFile(path, sampleData(), &error)) << error;
    PoseAnimationData loaded;
    std::vector<std::string> warnings;
    ASSERT_TRUE(loadPoseAnimationFile(path, loaded, warnings, &error)) << error;
    EXPECT_EQ(loaded.previewModel, "models/hero.gltf");
}

TEST(PoseFile, EngineFileCloseIsIdempotent){
    // File::close() then the destructor (or a second close()) closed the FILE* twice (undefined
    // behaviour, a double free in glibc): close() nulls the handle
    posetest::ScratchDir dir;
    const std::string path = dir.file("close.bin");
    {
        File file;
        ASSERT_EQ(file.open(path.c_str(), true), FileErrors::FILEDATA_OK);
        unsigned char bytes[3] = {1, 2, 3};
        EXPECT_EQ(file.write(bytes, 3), 3u);
        file.close();
        EXPECT_EQ(file.getFilePtr(), nullptr);
        file.close();
        EXPECT_EQ(file.getFilePtr(), nullptr);
    }   // the destructor runs on a closed file
    EXPECT_EQ(slurp(path), std::string("\x01\x02\x03"));
    {
        File file(path.c_str());
        ASSERT_NE(file.getFilePtr(), nullptr);
        EXPECT_EQ(file.length(), 3u);
        file.close();
    }
    {
        File invalid(static_cast<const char*>(nullptr));   // open() fails before setting the handle
        EXPECT_EQ(invalid.getFilePtr(), nullptr);
    }
}

TEST(PoseFile, SaveAndLoadHumanoidOverrides){
    posetest::ScratchDir dir;
    const std::string path = dir.file(humanoidSidecarPath("hero.gltf"));

    HumanoidOverrides overrides;
    overrides.bones[boneIndex(HumanBone::Hips)] = "Root";
    overrides.bones[boneIndex(HumanBone::RightToes)] = "";
    std::string error;
    ASSERT_TRUE(saveHumanoidOverridesFile(path, overrides, &error)) << error;
    EXPECT_EQ(slurp(path), writeHumanoidOverrides(overrides));

    HumanoidOverrides loaded;
    std::vector<std::string> warnings;
    bool notFound = true;
    ASSERT_TRUE(loadHumanoidOverridesFile(path, loaded, warnings, &error, &notFound)) << error;
    EXPECT_FALSE(notFound);
    EXPECT_TRUE(warnings.empty());
    EXPECT_EQ(loaded, overrides);
}

TEST(PoseFile, MissingSidecarIsAllAutoNotAnError){
    posetest::ScratchDir dir;
    HumanoidOverrides loaded;
    loaded.bones[boneIndex(HumanBone::Head)] = "stale";
    std::vector<std::string> warnings;
    std::string error = "stale";
    bool notFound = false;
    EXPECT_TRUE(loadHumanoidOverridesFile(dir.file("none.humanoid"), loaded, warnings, &error, &notFound));
    EXPECT_TRUE(notFound);
    EXPECT_TRUE(loaded.empty());
    EXPECT_TRUE(error.empty());
    EXPECT_TRUE(warnings.empty());

    // the out-parameter is optional
    loaded.bones[boneIndex(HumanBone::Head)] = "stale";
    EXPECT_TRUE(loadHumanoidOverridesFile(dir.file("none.humanoid"), loaded, warnings, &error));
    EXPECT_TRUE(loaded.empty());
}

// the runtime's sidecar stamp (ModelRig): missing = all auto, unreadable = an error (as the
// editor), not a silent auto-detect
TEST(PoseFile, SidecarStampFailsOnAnUnreadableSidecar){
    posetest::ScratchDir dir;
    std::string stamp = "stale";
    std::string error = "stale";
    EXPECT_TRUE(humanoidSidecarStamp(dir.file("none.gltf"), stamp, &error));
    EXPECT_TRUE(stamp.empty());
    EXPECT_TRUE(error.empty());

    {
        std::ofstream sidecar(dir.file("hero.gltf.humanoid"));
        sidecar << "{}";
    }
    EXPECT_TRUE(humanoidSidecarStamp(dir.file("hero.gltf"), stamp, &error)) << error;
    EXPECT_EQ(stamp, "{}");

    // a directory where the sidecar should be: it exists and cannot be read
    std::filesystem::create_directories(dir.file("robot.gltf.humanoid"));
    error.clear();
    EXPECT_FALSE(humanoidSidecarStamp(dir.file("robot.gltf"), stamp, &error));
    EXPECT_TRUE(stamp.empty());
    EXPECT_NE(error.find("robot.gltf.humanoid"), std::string::npos) << error;
}

TEST(PoseFile, CorruptSidecarFailsAndIsNotReportedAsMissing){
    posetest::ScratchDir dir;
    HumanoidOverrides loaded;
    std::vector<std::string> warnings;
    std::string error;
    bool notFound = true;

    {
        std::ofstream bad(dir.file("bad.humanoid"));
        bad << "{\"format\": \"jasperengine.humanoid\", \"version\": 5}";
    }
    EXPECT_FALSE(loadHumanoidOverridesFile(dir.file("bad.humanoid"), loaded, warnings, &error, &notFound));
    EXPECT_FALSE(notFound);
    EXPECT_NE(error.find("bad.humanoid"), std::string::npos) << error;
    EXPECT_NE(error.find("version"), std::string::npos) << error;

    {
        std::ofstream garbage(dir.file("garbage.humanoid"));
        garbage << "this is not json";
    }
    error.clear();
    EXPECT_FALSE(loadHumanoidOverridesFile(dir.file("garbage.humanoid"), loaded, warnings, &error, &notFound));
    EXPECT_FALSE(notFound);
    EXPECT_FALSE(error.empty());
}
