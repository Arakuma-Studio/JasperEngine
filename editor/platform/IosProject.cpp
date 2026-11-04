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

#include <algorithm>
#include <cstdlib>
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

XcodeChoice platform::chooseXcode(const std::string& developerDirVariable, const std::string& activeDeveloperDir,
                                  const std::vector<fs::path>& applications) {
    XcodeChoice choice;
    if (!developerDirVariable.empty()) {
        choice.developerDir = developerDirVariable;
        return choice;
    }
    if (!activeDeveloperDir.empty() && activeDeveloperDir.find("CommandLineTools") == std::string::npos) {
        choice.developerDir = activeDeveloperDir;
        return choice;
    }
    // Xcode.app first, then the newest looking of Xcode-16.app, Xcode_26.app...
    std::vector<fs::path> sorted = applications;
    std::sort(sorted.begin(), sorted.end(), [](const fs::path& a, const fs::path& b) {
        const bool plainA = a.filename() == "Xcode.app", plainB = b.filename() == "Xcode.app";
        return plainA != plainB ? plainA : b.filename() < a.filename();
    });
    if (!sorted.empty()) {
        choice.developerDir = sorted.front() / "Contents" / "Developer";
        choice.throughEnvironment = true;
        return choice;
    }
    choice.missing = activeDeveloperDir.empty() ? "no Xcode is installed"
                                                : "the active developer directory is the Command Line Tools (" + activeDeveloperDir +
                                                      "), and no Xcode is in /Applications; install Xcode from the App Store";
    return choice;
}

XcodeChoice platform::findXcode() {
#ifdef __APPLE__
    const char* variable = std::getenv("DEVELOPER_DIR");
    std::vector<fs::path> applications;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator("/Applications", ec)) {
        const std::string name = entry.path().filename().string();
        if (name.rfind("Xcode", 0) == 0 && entry.path().extension() == ".app" &&
            fs::exists(entry.path() / "Contents" / "Developer" / "usr" / "bin" / "xcodebuild", ec))
            applications.push_back(entry.path());
    }
    return chooseXcode(variable ? variable : "", CommandRunner::capture({"xcode-select", "-p"}, 5000), applications);
#else
    XcodeChoice choice;
    choice.missing = "a Mac with Xcode";
    return choice;
#endif
}

std::string platform::describeXcodeFailure(const std::string& log, const std::string& teamId) {
    const auto says = [&log](const char* text) { return log.find(text) != std::string::npos; };
    if (says("requires a development team"))
        return "signing needs a team: set Project Settings > iOS > Team ID (or build for the simulator, which needs none)";
    if (says("No Account for Team") || says("No Accounts"))
        return "Xcode has no account for the team " + teamId + ": add one in Xcode > Settings > Accounts, or correct Project Settings > iOS > Team ID";
    if (says("No profiles for") || says("requires a provisioning profile") || says("No signing certificate") || says("doesn't include signing certificate") ||
        says("no signing identity") || says("No provisioning profile"))
        return "no certificate or provisioning profile to sign with for the team " + teamId +
               ": sign in with that team in Xcode > Settings > Accounts and let it manage signing";
    if (says("agreed to the Xcode") || says("Xcode license"))
        return "the Xcode licence is not accepted yet: open Xcode once, or run sudo xcodebuild -license accept";
    if (says("is not installed. Please download") || says("is not installed. To use"))
        return "this Xcode has not installed the iOS platform yet: add it in Xcode > Settings > Components";
    if (says("requires Xcode, but active developer directory"))
        return "the active developer directory is the Command Line Tools, which cannot build for iOS: install Xcode, or set DEVELOPER_DIR to it";
    return "";
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

bool platform::buildIosProject(const IosProjectSettings& settings, CommandRunner& runner, const std::function<bool()>& cancelled,
                               std::vector<fs::path>& artifacts, std::vector<std::string>& notes, std::vector<BuildWarning>& warnings,
                               std::string& error) {
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
    artifacts.push_back(project);

    const XcodeChoice xcode = findXcode();
    if (!xcode.found()) {
        notes.push_back("iOS needs a Mac with Xcode (" + xcode.missing + "): open " + project.string() + " on a Mac with Xcode to build and sign it");
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
    CommandRunner::Environment environment;
    if (xcode.throughEnvironment) {
        environment = CommandRunner::currentEnvironment();
        environment.emplace_back("DEVELOPER_DIR", xcode.developerDir.string());
    }
    const std::string program = ExternalTools::xcodebuild().path.string();
    auto xcodebuild = [&](const std::vector<std::string>& args) {
        std::vector<std::string> argv = {program};
        argv.insert(argv.end(), args.begin(), args.end());
        return runner.run(argv, settings.projectDir, collect, xcode.throughEnvironment ? &environment : nullptr);
    };
    auto failed = [&](const std::string& what) {
        const std::string reason = describeXcodeFailure(log, settings.teamId);
        error = cancelled() ? "Build cancelled" : reason.empty() ? what + " failed:\n" + log : what + " failed: " + reason + "\n\n" + log;
        return false;
    };
    const std::vector<std::string> common = {"-project", project.string(), "-scheme", "JasperEnginePlayer", "-configuration", "Release"};
    auto with = [&common](std::vector<std::string> args) {
        args.insert(args.begin(), common.begin(), common.end());
        return args;
    };
    const fs::path derived = settings.projectDir / "build";
    if (settings.simulator) {
        if (!xcodebuild(with({"-sdk", "iphonesimulator", "-derivedDataPath", derived.string(), "CODE_SIGNING_ALLOWED=NO", "build"})))
            return failed("xcodebuild");
        artifacts.insert(artifacts.begin(), derived / "Build" / "Products" / "Release-iphonesimulator" / (settings.appName + ".app"));
        return true;
    }
    if (settings.teamId.empty()) {
        // no team, no signature: as far as a device build can go, and the project is still the result
        if (!xcodebuild(with({"-sdk", "iphoneos", "-derivedDataPath", derived.string(), "CODE_SIGNING_ALLOWED=NO", "build"})))
            return failed("xcodebuild");
        artifacts.insert(artifacts.begin(), derived / "Build" / "Products" / "Release-iphoneos" / (settings.appName + ".app"));
        warnings.push_back({"W-IOS-NO-TEAM", "the device build is compiled but not signed, so it installs on no device: set Project Settings > iOS > Team ID "
                                             "for a signed .ipa, or build for the simulator"});
        return true;
    }
    const fs::path archive = settings.projectDir / (settings.appName + ".xcarchive");
    if (!xcodebuild(with({"-destination", "generic/platform=iOS", "-archivePath", archive.string(), "-allowProvisioningUpdates", "archive",
                          "DEVELOPMENT_TEAM=" + settings.teamId})))
        return failed("xcodebuild archive");
    const fs::path options = settings.projectDir / "ExportOptions.plist";
    io::writeFileAtomic(options, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
                                 "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n<plist version=\"1.0\">\n<dict>\n\t<key>method</key>\n\t<string>" +
                                 settings.exportMethod + "</string>\n\t<key>teamID</key>\n\t<string>" + settings.teamId + "</string>\n</dict>\n</plist>\n");
    const fs::path exportDir = settings.projectDir / "export";
    if (!xcodebuild({"-exportArchive", "-archivePath", archive.string(), "-exportOptionsPlist", options.string(), "-exportPath", exportDir.string(),
                     "-allowProvisioningUpdates"}))
        return failed("xcodebuild -exportArchive");
    artifacts.insert(artifacts.begin(), exportDir / (settings.appName + ".ipa"));
    return true;
}
