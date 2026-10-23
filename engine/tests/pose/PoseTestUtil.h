// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Helpers shared by the pose IO tests: parse wrappers that keep the warnings and the error, a
// warning search, the DOF index of a name and a scratch directory removed on scope exit.

#ifndef POSE_TEST_UTIL_H
#define POSE_TEST_UTIL_H

#include <gtest/gtest.h>

#include "pose/HumanoidMapIO.h"
#include "pose/PoseAnimationIO.h"
#include "pose/PoseDofs.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace posetest {

    struct ParsedPose {
        jasperengine::pose::PoseAnimationData data;
        std::vector<std::string> warnings;
        std::string error;
        bool ok = false;
        std::optional<jasperengine::pose::PoseAnimationTemplateInfo> info;
    };

    inline ParsedPose parsePose(const std::string& text){
        ParsedPose result;
        result.ok = jasperengine::pose::parsePoseAnimation(text, result.data, result.warnings, &result.error,
                                                           &result.info);
        return result;
    }

    struct ParsedHumanoid {
        jasperengine::pose::HumanoidOverrides overrides;
        std::vector<std::string> warnings;
        std::string error;
        bool ok = false;
    };

    inline ParsedHumanoid parseHumanoid(const std::string& text){
        ParsedHumanoid result;
        result.ok = jasperengine::pose::parseHumanoidOverrides(text, result.overrides, result.warnings, &result.error);
        return result;
    }

    inline bool anyWarningContains(const std::vector<std::string>& warnings, const std::string& part){
        return std::any_of(warnings.begin(), warnings.end(),
                           [&](const std::string& w){ return w.find(part) != std::string::npos; });
    }

    // dof index of "left_upper_arm.raise"
    inline int dof(const char* name){
        int index = -1;
        EXPECT_TRUE(jasperengine::pose::dofFromName(name, index)) << name;
        return index;
    }

    // A std::filesystem path from a UTF-8 string
    inline std::filesystem::path utf8Path(const std::string& text){
#if defined(__cpp_lib_char8_t)
        return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
#else
        return std::filesystem::u8path(text);
#endif
    }

    // A fresh directory under the system temp directory, removed (with its content) on destruction
    class ScratchDir {
    public:
        ScratchDir(){
            static int counter = 0;
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            path_ = std::filesystem::temp_directory_path() /
                    ("pose-test-" + std::to_string(stamp) + "-" + std::to_string(counter++));
            std::filesystem::create_directories(path_);
        }
        ~ScratchDir(){
            std::error_code ignored;
            std::filesystem::remove_all(path_, ignored);
        }
        ScratchDir(const ScratchDir&) = delete;
        ScratchDir& operator=(const ScratchDir&) = delete;

        const std::filesystem::path& path() const { return path_; }
        std::string file(const std::string& name) const { return (path_ / name).string(); }

    private:
        std::filesystem::path path_;
    };

}

#endif // POSE_TEST_UTIL_H
