// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Portions adapted from OpenEngine Tools/Scripts/publish_game.py (MIT):
// Copyright (c) 2026 Gamecrafters Guild Limited and contributors

#include "PayloadValidator.h"

#include "io/JPak.h"
#include "platform/AndroidTools.h"
#include "player/ContentManifest.h"
#include "player/PlayerModules.h"
#include "players/PlayerRuntimes.h"
#include "publish/BinaryInfo.h"
#include "util/ZipArchive.h"
#include "util/FileUtils.h"
#include "util/ApplePrivacy.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <set>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::publish;
namespace fs = std::filesystem;

namespace {

    bool readText(const fs::path& path, std::string& out) {
        std::ifstream in(path, std::ios::binary);
        if (!in) return false;
        out.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        return true;
    }

    std::string upper(std::string text) {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        return text;
    }

    // what the content of a payload is: loose files under a root, plus a mounted pack
    struct Content {
        std::function<bool(const std::string&)> exists;
        std::function<bool(const std::string&, std::string&)> read;
    };

    // the modules of a player runtime id: its release variant's (the id's last part), the full
    // player's when it names none
    std::vector<std::string> variantModules(const std::string& playerId) {
        for (const PlayerVariant& variant : PlayerRuntimes::variants()) {
            if (variant.suffix.empty()) continue;
            const std::string tail = "-release-" + variant.suffix;
            if (playerId.size() > tail.size() && playerId.compare(playerId.size() - tail.size(), tail.size(), tail) == 0) return variant.modules;
        }
        return PlayerRuntimes::variants().front().modules;
    }

    void checkContent(const Content& content, const std::string& where, std::vector<std::string>& findings) {
        std::string text;
        if (!content.read("content-manifest.json", text)) {
            findings.push_back("missing " + where + "content-manifest.json");
            return;
        }
        player::ContentManifest manifest;
        std::string error;
        if (!player::ContentManifest::parse(text, manifest, error)) {
            findings.push_back(where + "content-manifest.json: " + error);
            return;
        }
        if (manifest.allowPlayerMismatch)
            findings.push_back("built with --allow-player-mismatch (player " + manifest.playerId + " is of another engine version): build it again");
        if (manifest.method == "prebuilt") {
            if (!content.exists("game.json")) findings.push_back("missing " + where + "game.json");
            const std::string start = "scenes/" + std::to_string(manifest.startScene) + ".jscn";
            if (!content.exists(start)) findings.push_back("missing start scene " + where + start);
        }
        for (const std::string& scene : manifest.scenes)
            if (!content.exists(scene)) findings.push_back("missing scene " + where + scene);
        for (const std::string& prefab : manifest.prefabs)
            if (!content.exists(prefab)) findings.push_back("missing prefab " + where + prefab);
        for (const std::string& shader : manifest.shaders)
            if (!content.exists("assets/shaders/" + shader)) findings.push_back("missing shader " + where + "assets/shaders/" + shader);
        for (const std::string& library : manifest.notices)
            if (!content.exists("licenses/" + library + "-LICENSE")) findings.push_back("missing notice " + where + "licenses/" + library + "-LICENSE");
        // S34 R3: a player variant runs only content whose modules it has
        if (manifest.method == "prebuilt" && !manifest.playerId.empty()) {
            const std::vector<std::string> missing = player::missingModules(manifest, variantModules(manifest.playerId));
            for (const std::string& module : missing)
                findings.push_back("the content needs " + player::moduleDisplayName(module) + " (" + module + "), which the player " + manifest.playerId +
                                   " leaves out: build it again");
        }
    }

    void checkBinary(const fs::path& path, const std::string& relative, const std::string& wantArch, BuildPlatform platform,
                     std::vector<std::string>& findings) {
        std::string error;
        const std::optional<BinaryInfo> info = inspectBinary(path, &error);
        if (!info) {
            findings.push_back(relative + ": " + error);
            return;
        }
        BinaryInfo::Format want = BinaryInfo::Unknown;
        switch (platform) {
            case BuildPlatform::Windows: want = BinaryInfo::Pe; break;
            case BuildPlatform::Linux:
            case BuildPlatform::Android: want = BinaryInfo::Elf; break;
            case BuildPlatform::MacOS:
            case BuildPlatform::IOS: want = BinaryInfo::MachO; break;
            case BuildPlatform::Web: want = BinaryInfo::Wasm; break;
        }
        const bool mach = want == BinaryInfo::MachO && (info->format == BinaryInfo::MachO || info->format == BinaryInfo::Fat);
        if (info->format != want && !mach) {
            findings.push_back(relative + " is " + formatName(info->format) + ", a " + buildPlatformName(platform) + " build needs " + formatName(want));
            return;
        }
        if (wantArch == "universal") {
            // a universal macOS binary holds every slice
            for (const char* slice : {"x64", "arm64"}) {
                if (std::find(info->arches.begin(), info->arches.end(), slice) == info->arches.end())
                    findings.push_back(relative + " is missing its " + std::string(slice == std::string("x64") ? "x86_64" : "arm64") + " slice");
            }
        } else if (std::find(info->arches.begin(), info->arches.end(), wantArch) == info->arches.end()) {
            std::string arches;
            for (const std::string& arch : info->arches) arches += (arches.empty() ? "" : "+") + arch;
            findings.push_back(relative + " is " + arches + ", the target is " + wantArch);
        }
        if (mach) {
            for (const std::string& arch : info->unsignedSlices) {
                if (arch == "arm64")
                    findings.push_back(relative + ": its arm64 slice is unsigned (no LC_CODE_SIGNATURE): it does not launch; sign it (codesign or rcodesign)");
            }
        }
    }

    bool isExecutableName(const fs::path& path, BuildPlatform platform) {
        const std::string ext = path.extension().string();
        switch (platform) {
            case BuildPlatform::Windows: return ext == ".exe" || ext == ".dll";
            case BuildPlatform::Linux: return ext == ".so" || path.filename().string().find(".so.") != std::string::npos;
            case BuildPlatform::MacOS: return ext == ".dylib";
            case BuildPlatform::Web: return ext == ".wasm";
            default: return false;
        }
    }

    // an executable file of the build that is a binary (not a script): ELF or Mach-O magic
    bool isExecutableBinary(const fs::path& path, BuildPlatform platform) {
        if (platform != BuildPlatform::Linux && platform != BuildPlatform::MacOS) return false;
        std::error_code ec;
        if ((fs::status(path, ec).permissions() & fs::perms::owner_exec) == fs::perms::none) return false;
        std::ifstream in(path, std::ios::binary);
        unsigned char magic[4] = {};
        if (!in.read(reinterpret_cast<char*>(magic), 4)) return false;
        const bool elf = magic[0] == 0x7f && magic[1] == 'E' && magic[2] == 'L' && magic[3] == 'F';
        const uint32_t word = magic[0] | (magic[1] << 8) | (magic[2] << 16) | (uint32_t(magic[3]) << 24);
        const bool macho = word == 0xFEEDFACEu || word == 0xFEEDFACFu || word == 0xCEFAEDFEu || word == 0xCFFAEDFEu ||
                           word == 0xBEBAFECAu || word == 0xCAFEBABEu;
        return elf || macho;
    }

    void checkAndroid(const fs::path& apk, const std::string& relative, const ValidationTarget& target, std::vector<std::string>& findings) {
        ZipReader zip;
        std::string error;
        if (!zip.open(apk, error)) {
            findings.push_back(relative + ": " + error);
            return;
        }
        const bool bundle = apk.extension() == ".aab";
        const std::string assets = bundle ? "base/assets/game/" : "assets/game/";
        const std::string libs = bundle ? "base/lib/" : "lib/";
        std::set<std::string> names;
        for (const ZipEntryInfo& entry : zip.entries()) names.insert(entry.name);
        size_t storedLibraries = 0, compressedLibraries = 0;
        for (const ZipEntryInfo& entry : zip.entries()) {
            if (entry.name.rfind(libs, 0) != 0 || fs::path(entry.name).extension() != ".so") continue;
            const std::string abi = entry.name.substr(libs.size(), entry.name.find('/', libs.size()) - libs.size());
            std::vector<uint8_t> bytes;
            if (!zip.read(entry.name, bytes, error)) {
                findings.push_back(relative + "!" + entry.name + ": " + error);
                continue;
            }
            const std::optional<BinaryInfo> info = inspectBinary(bytes, &error);
            const std::string want = abi == "arm64-v8a" ? "arm64" : abi == "armeabi-v7a" ? "arm" : abi == "x86" ? "x86" : abi == "x86_64" ? "x64" : "";
            if (!info || info->format != BinaryInfo::Elf) {
                findings.push_back(relative + "!" + entry.name + " is not an ELF library");
            } else if (want.empty() || std::find(info->arches.begin(), info->arches.end(), want) == info->arches.end()) {
                findings.push_back(relative + "!" + entry.name + " is " + (info->arches.empty() ? "?" : info->arches.front()) + ", its directory is " + abi);
            } else if ((abi == "arm64-v8a" || abi == "x86_64") && info->minLoadAlignment < 16384) {
                // 16 KB pages are 64-bit only (Android 15): armeabi-v7a and x86 keep 4 KB
                findings.push_back(relative + "!" + entry.name + " is aligned to " + std::to_string(info->minLoadAlignment) +
                                   " bytes, 16 KB page devices need 16384 (-Wl,-z,max-page-size=16384)");
            }
            (entry.stored ? storedLibraries : compressedLibraries)++;
        }
        // an APK's are stored (loaded from the APK: extractNativeLibs false) or compressed (extracted
        // at install, minSdk < 23), never mixed; a bundle's are bundletool's (it compresses every
        // entry of the .aab, and its config decides how each device's APK holds them)
        if (!bundle && storedLibraries > 0 && compressedLibraries > 0)
            findings.push_back(relative + ": " + std::to_string(compressedLibraries) + " native librar" +
                               (compressedLibraries == 1 ? "y is" : "ies are") + " compressed and " + std::to_string(storedLibraries) +
                               " stored (a package stores them all, or compresses them all for minSdk < 23)");
        Content content{
            [&](const std::string& path) { return names.count(assets + path) > 0; },
            [&](const std::string& path, std::string& out) {
                std::vector<uint8_t> bytes;
                std::string why;
                if (!zip.read(assets + path, bytes, why)) return false;
                out.assign(bytes.begin(), bytes.end());
                return true;
            }};
        checkContent(content, relative + "!" + assets, findings);
        if (bundle) {
            // bundles have no apksigner: jarsigner's signature block is what Google Play looks for
            const bool isSigned = std::any_of(names.begin(), names.end(), [](const std::string& name) {
                if (name.rfind("META-INF/", 0) != 0) return false;
                const std::string extension = fs::path(name).extension().string();
                return extension == ".RSA" || extension == ".EC" || extension == ".DSA";
            });
            if (!isSigned) findings.push_back(relative + ": the bundle is not signed (no signature block in META-INF)");
        } else {
            std::string output;
            bool verified = false;
            if (target.verifyApk) {
                verified = target.verifyApk(apk, output);
            } else {
                std::string why;
                verified = platform::AndroidTools::verifyApk(apk, output, why);
                if (!why.empty()) output = why;
            }
            if (!verified) findings.push_back(relative + ": apksigner verify failed: " + output);
        }
    }

}

TargetOs publish::targetOs(BuildPlatform platform) {
    switch (platform) {
        case BuildPlatform::Windows: return TargetOs::Windows;
        case BuildPlatform::Linux: return TargetOs::Linux;
        case BuildPlatform::MacOS: return TargetOs::MacOS;
        case BuildPlatform::Android: return TargetOs::Android;
        case BuildPlatform::IOS: return TargetOs::IOS;
        case BuildPlatform::Web: return TargetOs::Web;
    }
    return TargetOs::Linux;
}

std::string publish::windowsNameProblem(const std::string& relativePath) {
    static const std::set<std::string> reserved = {"CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7",
                                                   "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};
    size_t start = 0;
    while (start <= relativePath.size()) {
        const size_t end = relativePath.find('/', start);
        const std::string part = relativePath.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!part.empty()) {
            const std::string stem = upper(part.substr(0, part.find('.')));
            if (reserved.count(stem)) return "\"" + part + "\" is a name Windows reserves";
            if (part.back() == '.' || part.back() == ' ') return "\"" + part + "\" ends in a dot or a space";
            if (part.find_first_of(":<>\"|?*") != std::string::npos) return "\"" + part + "\" has a character Windows refuses";
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return "";
}

bool publish::validatePayload(const fs::path& root, const ValidationTarget& target, std::vector<std::string>& findings) {
    const size_t before = findings.size();
    std::error_code ec;
    if (!fs::is_directory(root, ec)) {
        findings.push_back(root.string() + " is not a directory");
        return false;
    }
    const TreeManifestResult tree = treeManifest(root, targetOs(target.platform));
    findings.insert(findings.end(), tree.problems.begin(), tree.problems.end());

    for (const auto& [relative, entry] : tree.entries) {
        const fs::path path = root / editor::FileUtils::pathFromUtf8(relative);
        if (fs::path(relative).filename() == "steam_appid.txt")
            findings.push_back(relative + ": steam_appid.txt makes Steam treat the build as a development one: remove it");
        if (target.platform == BuildPlatform::Windows) {
            const std::string problem = windowsNameProblem(relative);
            if (!problem.empty()) findings.push_back(relative + ": " + problem);
            if (relative.size() >= 240) findings.push_back(relative + ": " + std::to_string(relative.size()) + " characters, Windows paths stay under 240");
        }
        if (!entry.symlink.empty()) continue;
        if (target.platform == BuildPlatform::Android && (path.extension() == ".apk" || path.extension() == ".aab")) {
            checkAndroid(path, relative, target, findings);
            continue;
        }
        // the libraries, and every binary executable (the main one is checked below)
        if (relative != target.executable && (isExecutableName(path, target.platform) || isExecutableBinary(path, target.platform)))
            checkBinary(path, relative, target.arch, target.platform, findings);
    }

    // the main executable: present, right format, executable
    std::string executable = target.executable;
    if (executable.empty()) {
        for (const auto& [relative, entry] : tree.entries) {
            const fs::path p(relative);
            if (target.platform == BuildPlatform::Windows && p.extension() == ".exe" && p.parent_path().empty()) executable = relative;
            if (target.platform == BuildPlatform::Linux && p.parent_path().empty() && fs::is_regular_file(root / p, ec) &&
                (fs::status(root / p, ec).permissions() & fs::perms::owner_exec) != fs::perms::none && p.extension().empty())
                executable = relative;
            if (target.platform == BuildPlatform::MacOS && relative.find(".app/Contents/MacOS/") != std::string::npos) executable = relative;
        }
    }
    if (isDesktopPlatform(target.platform)) {
        if (executable.empty() || !tree.entries.count(executable)) {
            findings.push_back("no executable in the build" + (executable.empty() ? std::string() : " (" + executable + ")"));
        } else {
            checkBinary(root / editor::FileUtils::pathFromUtf8(executable), executable, target.arch, target.platform, findings);
            if (target.platform != BuildPlatform::Windows &&
                (fs::status(root / editor::FileUtils::pathFromUtf8(executable), ec).permissions() & fs::perms::owner_exec) == fs::perms::none)
                findings.push_back(executable + " has no exec bit");
        }
    }

    // the content, where each platform keeps it
    fs::path contentRoot = root;
    if (target.platform == BuildPlatform::MacOS) {
        for (const auto& entry : fs::directory_iterator(root, ec))
            if (entry.path().extension() == ".app") contentRoot = entry.path() / "Contents" / "Resources";
    } else if (target.platform == BuildPlatform::IOS) {
        for (const auto& entry : fs::directory_iterator(root, ec))
            if (fs::is_directory(entry.path() / "Content", ec)) contentRoot = entry.path() / "Content";
    }
    // an Apple app bundle (macOS .app, the iOS project) ships its privacy manifest: App Store
    // Connect refuses apps that use required-reason APIs without one
    if ((target.platform == BuildPlatform::MacOS || target.platform == BuildPlatform::IOS) && contentRoot != root) {
        const fs::path manifest = target.platform == BuildPlatform::MacOS ? contentRoot / "PrivacyInfo.xcprivacy"
                                                                          : contentRoot.parent_path() / "PrivacyInfo.xcprivacy";
        const std::string where = editor::FileUtils::pathToGenericUtf8(manifest.lexically_relative(root));
        std::ifstream in(manifest, std::ios::binary);
        if (!in) {
            findings.push_back("no " + where + " (the Apple privacy manifest)");
        } else {
            const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            editor::ApplePrivacy::Manifest parsed;
            std::vector<std::string> warnings;
            std::string error;
            if (!editor::ApplePrivacy::parse(text, parsed, warnings, error)) findings.push_back(where + " does not parse: " + error);
        }
    }
    if (target.platform != BuildPlatform::Android) {
        JPak pack;
        const fs::path packPath = target.platform == BuildPlatform::Web ? root / "content.jpak" : contentRoot / "content.jpak";
        std::string error;
        const bool packed = fs::is_regular_file(packPath, ec);
        if (packed && !pack.openFile(packPath, &error)) findings.push_back("content.jpak: " + error);
        Content content{
            [&](const std::string& path) {
                return fs::is_regular_file(contentRoot / editor::FileUtils::pathFromUtf8(path), ec) || (target.platform == BuildPlatform::Web && fs::is_regular_file(root / editor::FileUtils::pathFromUtf8(path), ec)) ||
                       (pack.isOpen() && pack.find(path));
            },
            [&](const std::string& path, std::string& out) {
                if (readText(contentRoot / editor::FileUtils::pathFromUtf8(path), out)) return true;
                if (target.platform == BuildPlatform::Web && readText(root / editor::FileUtils::pathFromUtf8(path), out)) return true;
                std::vector<uint8_t> bytes;
                if (pack.isOpen() && pack.read(path, bytes)) {
                    out.assign(bytes.begin(), bytes.end());
                    return true;
                }
                return false;
            }};
        checkContent(content, "", findings);
    }
    return findings.size() == before;
}
