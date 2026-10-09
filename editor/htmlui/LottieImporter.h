// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "RemoteImporter.h"

#include <functional>
#include <string>
#include <vector>

namespace jasperengine::editor::htmlui {

    // Lottie animations (phase-2 §3.13.3): a .json is saved as
    // lottie/<sha1-12>-<name>/animation.json with its external image assets ("u" + "p")
    // downloaded into images/ next to it and the JSON rewritten to them. A .lottie (dotLottie
    // v1 or v2, read with the engine's DotLottie/ZipArchive: stored and deflated entries) gives
    // its active (or first) animation and the images it uses, written the same way. A
    // LottieFiles page (lottiefiles.com/…) is searched for the animation file it shows. License:
    // LottieFiles' "Lottie Simple License (verify on lottiefiles.com)", HTMLUI-IMP-0008.
    class LottieImporter {
    public:
        explicit LottieImporter(RemoteImporter& importer);

        ImportedResource runJson(const std::string& url, const std::string& finalUrl, const std::string& body, const ImportLocation& at);
        ImportedResource runDotLottie(const std::string& url, const std::string& finalUrl, const std::string& body, const ImportLocation& at);
        ImportedResource runPage(const std::string& url, const std::string& finalUrl, const std::string& body, const ImportLocation& at);

        // a JSON object with a "layers" array and a version or frame rate
        static bool looksLikeLottie(const std::string& json);
        // a page of lottiefiles.com (not one of its asset hosts)
        static bool isLottieFilesPage(const std::string& url);
        // https URLs of .json/.lottie files on lottiefiles.com hosts or lottie.host in a page, in order
        static std::vector<std::string> findAnimationUrls(const std::string& html);
        // lottie/<sha1-12>-<name> under the import root
        static std::string directory(const RemoteImporter& importer, const std::string& url);

    private:
        using ImageSource = std::function<bool(const std::string& assetUrl, const std::string& path, std::string& data)>;
        ImportedResource write(const std::string& url, const std::string& animationJson, const ImageSource& images,
                               const ImportLocation& at);

        RemoteImporter& importer;
    };

}
