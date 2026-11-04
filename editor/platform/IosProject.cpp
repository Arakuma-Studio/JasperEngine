// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "IosProject.h"

#include "Exporter.h"
#include "Out.h"
#include "io/AtomicFile.h"
#include "platform/ExternalTools.h"

#include "stb_image.h"
#include "stb_image_resize2.h"
#include "stb_image_write.h"

#include <fstream>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::platform;
namespace fs = std::filesystem;

namespace {

    void replaceAll(std::string& text, const std::string& marker, const std::string& value) {
        for (size_t at = text.find(marker); at != std::string::npos; at = text.find(marker, at + value.size())) text.replace(at, marker.size(), value);
    }

    bool rewrite(const fs::path& path, const IosProjectSettings& settings, std::string& error) {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            error = "the iOS template has no " + path.filename().string();
            return false;
        }
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        in.close();
        if (!io::writeFileAtomic(path, fillIosTemplate(text, settings)).ok()) {
            error = "cannot write " + path.string();
            return false;
        }
        return true;
    }

}

std::string platform::fillIosTemplate(std::string text, const IosProjectSettings& s) {
    replaceAll(text, "{{PRODUCT_NAME}}", s.appName);
    replaceAll(text, "{{DISPLAY_NAME}}", Exporter::escapeXml(s.displayName));
    replaceAll(text, "{{BUNDLE_ID}}", s.bundleIdentifier);
    replaceAll(text, "{{VERSION}}", s.version);
    replaceAll(text, "{{BUILD}}", s.build);
    replaceAll(text, "{{TEAM_ID}}", s.teamId);
    replaceAll(text, "{{STATUS_BAR_HIDDEN}}", s.hideStatusBar ? "<true/>" : "<false/>");
    return text;
}

bool platform::stageIosProject(const IosProjectSettings& settings, std::string& error) {
    std::error_code ec;
    const fs::path templateDir = settings.runtimeDir / "template";
    if (!fs::is_directory(templateDir, ec) || !fs::exists(settings.runtimeDir / "JasperEnginePlayer.xcframework", ec)) {
        error = "the iOS player runtime has no template/ or JasperEnginePlayer.xcframework (" + settings.runtimeDir.string() + ")";
        return false;
    }
    fs::create_directories(settings.projectDir, ec);
    fs::copy(templateDir, settings.projectDir, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    fs::copy(settings.runtimeDir / "JasperEnginePlayer.xcframework", settings.projectDir / "JasperEnginePlayer.xcframework",
             fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    if (ec) {
        error = "cannot stage the Xcode project: " + ec.message();
        return false;
    }
    const fs::path project = settings.projectDir / "JasperEnginePlayer.xcodeproj";
    if (!rewrite(project / "project.pbxproj", settings, error) || !rewrite(settings.projectDir / "Info.plist", settings, error) ||
        !rewrite(project / "xcshareddata" / "xcschemes" / "JasperEnginePlayer.xcscheme", settings, error))
        return false;

    // the icon: one 1024 px image (Xcode makes the other sizes)
    if (!settings.iconPng.empty() && fs::is_regular_file(settings.iconPng, ec)) {
        int width = 0, height = 0, channels = 0;
        unsigned char* pixels = stbi_load(settings.iconPng.string().c_str(), &width, &height, &channels, 4);
        if (pixels) {
            std::vector<unsigned char> scaled(1024 * 1024 * 4);
            stbir_resize_uint8_srgb(pixels, width, height, 0, scaled.data(), 1024, 1024, 0, STBIR_RGBA);
            // App Store icons have no alpha
            for (size_t i = 3; i < scaled.size(); i += 4) scaled[i] = 255;
            const fs::path set = settings.projectDir / "Assets.xcassets" / "AppIcon.appiconset";
            fs::create_directories(set, ec);
            stbi_write_png((set / "AppIcon-1024.png").string().c_str(), 1024, 1024, 4, scaled.data(), 1024 * 4);
            stbi_image_free(pixels);
            io::writeFileAtomic(set / "Contents.json",
                                "{\n  \"images\" : [\n    {\n      \"filename\" : \"AppIcon-1024.png\",\n      \"idiom\" : \"universal\",\n"
                                "      \"platform\" : \"ios\",\n      \"size\" : \"1024x1024\"\n    }\n  ],\n  \"info\" : {\n    \"author\" : \"xcode\",\n"
                                "    \"version\" : 1\n  }\n}\n");
        }
    }
    return true;
}

bool platform::buildIosProject(const IosProjectSettings& settings, CommandRunner& runner, const std::function<bool()>& cancelled,
                               std::vector<fs::path>& artifacts, std::vector<std::string>& notes, std::string& error) {
    const fs::path project = settings.projectDir / "JasperEnginePlayer.xcodeproj";
    artifacts.push_back(project);

    const ToolLocation xcodebuild = ExternalTools::xcodebuild();
#ifdef __APPLE__
    const bool mac = true;
#else
    const bool mac = false;
#endif
    if (!mac || !xcodebuild.found()) {
        notes.push_back("iOS needs a Mac with Xcode: open " + project.string() + " on a Mac with Xcode to build and sign it");
        return true;
    }
    if (cancelled()) {
        error = "Build cancelled";
        return false;
    }
    std::string log;
    auto collect = [&log](const std::string& line) {
        Out::build("%s", line.c_str());
        log += line + "\n";
        if (log.size() > 8000) log.erase(0, log.size() - 8000);
    };
    int code = -1;
    const fs::path derived = settings.projectDir / "build";
    if (settings.simulator) {
        runner.runArgs({xcodebuild.path.string(), "-project", project.string(), "-scheme", "JasperEnginePlayer", "-sdk", "iphonesimulator",
                        "-configuration", "Release", "-derivedDataPath", derived.string(), "CODE_SIGNING_ALLOWED=NO", "build"},
                       settings.projectDir, collect, &code);
        if (code != 0) {
            error = "xcodebuild failed:\n" + log;
            return false;
        }
        artifacts.insert(artifacts.begin(), derived / "Build" / "Products" / "Release-iphonesimulator" / (settings.appName + ".app"));
        return true;
    }
    const fs::path archive = settings.projectDir / (settings.appName + ".xcarchive");
    runner.runArgs({xcodebuild.path.string(), "-project", project.string(), "-scheme", "JasperEnginePlayer", "-configuration", "Release",
                    "-archivePath", archive.string(), "archive", "DEVELOPMENT_TEAM=" + settings.teamId},
                   settings.projectDir, collect, &code);
    if (code != 0) {
        error = "xcodebuild archive failed (a device build needs the team id in Project Settings > iOS):\n" + log;
        return false;
    }
    const fs::path options = settings.projectDir / "ExportOptions.plist";
    io::writeFileAtomic(options, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
                                 "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n<plist version=\"1.0\">\n<dict>\n\t<key>method</key>\n\t<string>" +
                                 settings.exportMethod + "</string>\n\t<key>teamID</key>\n\t<string>" + settings.teamId + "</string>\n</dict>\n</plist>\n");
    const fs::path exportDir = settings.projectDir / "export";
    runner.runArgs({xcodebuild.path.string(), "-exportArchive", "-archivePath", archive.string(), "-exportOptionsPlist", options.string(),
                    "-exportPath", exportDir.string()},
                   settings.projectDir, collect, &code);
    if (code != 0) {
        error = "xcodebuild -exportArchive failed:\n" + log;
        return false;
    }
    artifacts.insert(artifacts.begin(), exportDir / (settings.appName + ".ipa"));
    return true;
}
