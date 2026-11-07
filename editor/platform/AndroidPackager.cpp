// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AndroidPackager.h"
#include "util/NativePlugins.h"

#include "Exporter.h"
#include "Out.h"
#include "util/PlatformRules.h"
#include "io/AtomicFile.h"
#include "platform/AndroidTools.h"
#include "util/ZipArchive.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::platform;
namespace fs = std::filesystem;

namespace {

    bool readText(const fs::path& path, std::string& out) {
        std::ifstream in(path, std::ios::binary);
        if (!in) return false;
        out.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        return true;
    }

    void replaceAll(std::string& text, const std::string& marker, const std::string& value) {
        for (size_t at = text.find(marker); at != std::string::npos; at = text.find(marker, at + value.size())) text.replace(at, marker.size(), value);
    }

    // a tool run; its output in the build log, a failure with its last lines
    bool run(CommandRunner& runner, const std::vector<std::string>& args, const fs::path& dir, std::string& error) {
        std::string tail;
        int code = -1;
        runner.runArgs(args, dir, [&tail](const std::string& line) {
            Out::build("%s", line.c_str());
            tail += line + "\n";
            if (tail.size() > 4000) tail.erase(0, tail.size() - 4000);
        }, &code);
        if (code != 0) {
            error = fs::path(args.front()).filename().string() + " failed (exit " + std::to_string(code) + ")" + (tail.empty() ? "" : ":\n" + tail);
            return false;
        }
        return true;
    }

    // a secret for a tool, through a file only this user reads (never on a command line)
    struct SecretFile {
        fs::path path;
        ~SecretFile() {
            std::error_code ec;
            if (!path.empty()) fs::remove(path, ec);
        }
        bool write(const fs::path& dir, const std::string& name, const std::string& value) {
            path = dir / name;
            std::error_code ec;
            fs::remove(path, ec);
            {
                std::ofstream out(path, std::ios::binary);
                if (!out) return false;
            }
            fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, ec);
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            out << value;
            return static_cast<bool>(out);
        }
    };

    // The key a package is signed with, its passwords in owner-only files for as long as it lives
    struct SigningKey {
        fs::path keystore;
        std::string alias;
        SecretFile storeSecret, keySecret;
    };

    // The project's keystore, or the debug one (made when missing), checked before any packaging: a
    // wrong password or alias fails here with what to change. The passwords go through files, on
    // no command line and in no log.
    bool prepareSigningKey(const AndroidPackage& package, const AndroidToolchain& tools, CommandRunner& runner,
                           std::vector<BuildWarning>& warnings, SigningKey& key, std::string& error) {
        std::error_code ec;
        key.keystore = package.keystore;
        key.alias = package.keystoreAlias;
        std::string storePassword = package.keystorePassword;
        std::string keyPassword = package.keyPassword;
        if (key.keystore.empty()) {
            const char* home = std::getenv("HOME");
#ifdef _WIN32
            if (!home) home = std::getenv("USERPROFILE");
#endif
            key.keystore = fs::path(home ? home : ".") / ".android" / "debug.keystore";
            key.alias = "androiddebugkey";
            storePassword = "android";
            keyPassword = "android";
            if (!package.debug)
                warnings.push_back({"W-ANDROID-DEBUG-KEY", "signed with the debug keystore (" + key.keystore.string() +
                                                               "): set Project Settings > Android > Keystore for a release the stores accept"});
            if (!fs::exists(key.keystore, ec)) {
                fs::create_directories(key.keystore.parent_path(), ec);
                if (!run(runner, {tools.keytool.path.string(), "-J-Duser.language=en", "-genkeypair", "-v", "-keystore", key.keystore.string(),
                                  "-storepass", "android", "-alias", "androiddebugkey", "-keypass", "android", "-keyalg", "RSA", "-keysize",
                                  "2048", "-validity", "10000", "-dname", "CN=Android Debug,O=Android,C=US"},
                         package.workDir, error))
                    return false;
            }
        } else {
            if (!fs::is_regular_file(key.keystore, ec)) {
                error = "the keystore " + key.keystore.string() + " does not exist (Project Settings > Android > Keystore)";
                return false;
            }
            if (storePassword.empty()) {
                const char* env = std::getenv("JASPER_ANDROID_KS_PASS");
                if (env) storePassword = env;
            }
            if (keyPassword.empty()) {
                const char* env = std::getenv("JASPER_ANDROID_KEY_PASS");
                keyPassword = env ? env : storePassword;
            }
            if (storePassword.empty()) {
                error = "the keystore " + key.keystore.string() + " needs its password: enter it in Build Settings, or set JASPER_ANDROID_KS_PASS "
                        "(it is never saved)";
                return false;
            }
            if (key.alias.empty()) {
                error = "the keystore " + key.keystore.string() + " has no alias set (Project Settings > Android > Keystore)";
                return false;
            }
        }
        if (!key.storeSecret.write(package.workDir, ".ks-pass", storePassword) || !key.keySecret.write(package.workDir, ".key-pass", keyPassword)) {
            error = "cannot pass the keystore passwords to the signing tool";
            return false;
        }
        // a certificate request reads the private key, so it needs the store and the key password;
        // it changes nothing, and its output stays out of the build log
        std::string output;
        int code = -1;
        const fs::path request = package.workDir / ".key-check.csr";
        runner.runArgs({tools.keytool.path.string(), "-J-Duser.language=en", "-certreq", "-keystore", key.keystore.string(), "-alias", key.alias,
                        "-storepass:file", key.storeSecret.path.string(), "-keypass:file", key.keySecret.path.string(), "-file", request.string()},
                       package.workDir, [&output](const std::string& line) { output += line + "\n"; }, &code);
        fs::remove(request, ec);
        if (code != 0) {
            error = describeKeystoreFailure(output, key.keystore, key.alias);
            if (error.empty()) error = "keytool cannot read the key " + key.alias + " of " + key.keystore.string() + " (exit " + std::to_string(code) + "):\n" + output;
            return false;
        }
        return true;
    }

    bool copyTree(const fs::path& from, const fs::path& to, std::string& error) {
        std::error_code ec;
        fs::create_directories(to, ec);
        fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
        if (ec) {
            error = "cannot copy " + from.string() + ": " + ec.message();
            return false;
        }
        return true;
    }

}

std::string platform::describeKeystoreFailure(const std::string& output, const fs::path& keystore, const std::string& alias) {
    const auto says = [&output](const char* text) { return output.find(text) != std::string::npos; };
    if (says("keystore password was incorrect") || says("password was incorrect"))
        return "the password of the keystore " + keystore.string() + " is wrong: enter the right one in Build Settings, or set JASPER_ANDROID_KS_PASS";
    if (says("Cannot recover key") || says("Get Key failed") || says("UnrecoverableKeyException"))
        return "the password of the key " + alias + " in " + keystore.string() + " is wrong: enter the right one in Build Settings, or set JASPER_ANDROID_KEY_PASS";
    if (says("does not exist") || says("has no key"))
        return "the keystore " + keystore.string() + " has no key named " + alias + " (Project Settings > Android > Key Alias)";
    if (says("keystore format") || says("Unrecognized") || says("not a keystore") || says("Illegal") || says("toDerInputStream"))
        return keystore.string() + " is not a keystore (Project Settings > Android > Keystore)";
    return "";
}

bool platform::writeAndroidManifest(const AndroidPackage& package, const std::string& templateText, std::string& manifest, std::string& error) {
    manifest = templateText;
    for (const char* marker : {"{{VERSION_CODE}}", "{{VERSION_NAME}}", "{{MIN_SDK}}", "{{TARGET_SDK}}", "{{LABEL}}", "{{PERMISSIONS}}",
                               "{{ALLOW_BACKUP}}", "{{ICON}}", "{{THEME}}", "{{ORIENTATION}}", "{{FULLSCREEN}}", "{{KEEP_SCREEN_ON}}",
                               "{{TEXTURE_SUPPORT}}", "{{NATIVE_MANIFEST_XML}}", "{{NATIVE_APPLICATION_XML}}"}) {
        if (manifest.find(marker) == std::string::npos) {
            error = std::string("the player's AndroidManifest.template.xml has no ") + marker + " (a runtime of another version?)";
            return false;
        }
    }
    std::string permissions;
    for (const std::string& permission : package.settings.permissions) {
        const char* name = Exporter::androidPermissionName(permission);
        if (name) permissions += std::string("    <uses-permission android:name=\"android.permission.") + name + "\" />\n";
    }
    // Project Settings > Native Plugins > Android: the permissions by full name, and the raw XML
    // (code-free: a fragment with tools: merge rules makes the build compile, Exporter NativeNeeds)
    for (const std::string& permission : package.settings.extraPermissions) {
        if (permission.find_first_not_of(" \t") == std::string::npos) continue;
        permissions += "    <uses-permission android:name=\"" + Exporter::escapeXml(permission) + "\" />\n";
    }
    const std::pair<const char*, const std::string*> fragments[] = {{"android.manifest_xml", &package.settings.manifestXml},
                                                                    {"android.manifest_application_xml", &package.settings.manifestApplicationXml}};
    for (const auto& [setting, xml] : fragments) {
        const std::string why = NativePlugins::xmlFragmentError(*xml);
        if (!why.empty()) {
            error = std::string("Project Settings > Native Plugins > Android (") + setting + ") is " + why;
            return false;
        }
    }
    auto fragment = [](std::string xml, const char* indent) {
        xml.erase(std::remove(xml.begin(), xml.end(), '\r'), xml.end());
        if (xml.find_first_not_of(" \t\n") == std::string::npos) return std::string();
        std::string out;
        size_t start = 0;
        while (start < xml.size()) {
            const size_t end = xml.find('\n', start);
            const std::string line = xml.substr(start, end == std::string::npos ? std::string::npos : end - start);
            if (line.find_first_not_of(" \t") != std::string::npos) out += indent + line + "\n";
            if (end == std::string::npos) break;
            start = end + 1;
        }
        return out;
    };
    const bool launcher = !package.settings.launcherIcon.empty();
    const bool adaptive = !package.settings.adaptiveIconForeground.empty() && !package.settings.adaptiveIconBackground.empty();
    // the default orientation, or auto-rotation within the allowed ones (the user's rotation lock kept)
    bool unsupportedRotation = false;
    const int requestedOrientation = PlatformRules::androidRequestedOrientation(package.presentation.defaultOrientation,
        package.presentation.effectiveAllowedOrientations(), &unsupportedRotation);
    if (unsupportedRotation) {
        Out::warning("Android cannot restrict auto-rotation to the allowed orientations of Player settings; "
                     "the package allows all four (fullUser)");
    }
    const std::string orientation = PlatformRules::androidManifestOrientation(requestedOrientation);
    replaceAll(manifest, "{{VERSION_CODE}}", std::to_string(package.versionCode));
    replaceAll(manifest, "{{VERSION_NAME}}", Exporter::escapeXml(package.versionName));
    replaceAll(manifest, "{{MIN_SDK}}", std::to_string(package.settings.minSdk));
    replaceAll(manifest, "{{TARGET_SDK}}", std::to_string(package.settings.targetSdk));
    replaceAll(manifest, "{{LABEL}}", Exporter::escapeXml(package.label));
    replaceAll(manifest, "{{PERMISSIONS}}", permissions);
    replaceAll(manifest, "{{ALLOW_BACKUP}}", package.settings.allowBackup ? "true" : "false");
    replaceAll(manifest, "{{ICON}}", (launcher && !adaptive) ? "@drawable/ic_launcher_game" : adaptive ? "@mipmap/ic_launcher_game" : "@mipmap/ic_launcher");
    replaceAll(manifest, "{{THEME}}", package.settings.fullscreen ? "@style/AppTheme" : "@style/AppThemeWindowed");
    replaceAll(manifest, "{{ORIENTATION}}", orientation.empty() ? "" : "\n            android:screenOrientation=\"" + orientation + "\"");
    replaceAll(manifest, "{{FULLSCREEN}}", package.settings.fullscreen ? "true" : "false");
    replaceAll(manifest, "{{KEEP_SCREEN_ON}}", package.presentation.keepScreenOn ? "true" : "false");
    replaceAll(manifest, "{{TEXTURE_SUPPORT}}", package.astc ? "    <supports-gl-texture android:name=\"GL_KHR_texture_compression_astc_ldr\" />\n" : "");
    replaceAll(manifest, "{{NATIVE_MANIFEST_XML}}", fragment(package.settings.manifestXml, "    "));
    replaceAll(manifest, "{{NATIVE_APPLICATION_XML}}", fragment(package.settings.manifestApplicationXml, "        "));
    // compressed libraries are extracted at install (the template says false: stored, loaded from
    // the APK)
    if (compressesNativeLibraries(package))
        replaceAll(manifest, "android:extractNativeLibs=\"false\"", "android:extractNativeLibs=\"true\"");
    return true;
}

bool platform::compressesNativeLibraries(const AndroidPackage& package) {
    // as the Android Gradle plugin packages them: an APK for devices before Android 6 (API 23,
    // which first loads libraries from the APK) compresses them, about a third of their size in
    // the file; from minSdk 23, and in a bundle (bundletool makes each device's APK), they stay
    // stored and are never extracted
    return package.format != AndroidFormat::Aab && package.settings.minSdk < 23;
}

std::vector<std::string> platform::selectedAndroidAbis(const AndroidProjectSettings& settings) {
    std::vector<std::string> abis;
    if (settings.abiArm64V8a) abis.push_back("arm64-v8a");
    if (settings.abiArmeabiV7a) abis.push_back("armeabi-v7a");
    if (settings.abiX86) abis.push_back("x86");
    if (settings.abiX86_64) abis.push_back("x86_64");
    return abis;
}

std::vector<std::pair<std::string, uint64_t>> platform::androidLibrarySizes(const fs::path& runtimeAndroidDir,
                                                                            const std::vector<std::string>& abis) {
    std::vector<std::pair<std::string, uint64_t>> sizes;
    std::error_code ec;
    for (const std::string& abi : abis) {
        uint64_t bytes = 0;
        for (const auto& entry : fs::directory_iterator(runtimeAndroidDir / "lib" / abi, ec)) {
            if (entry.path().extension() != ".so") continue;
            const uintmax_t size = entry.file_size(ec);
            if (!ec) bytes += size;
        }
        sizes.emplace_back(abi, bytes);
    }
    return sizes;
}

bool platform::buildAndroidPackage(const AndroidPackage& package, CommandRunner& runner, const std::function<bool()>& cancelled,
                                   std::vector<BuildWarning>& warnings, std::string& error) {
    const bool bundle = package.format == AndroidFormat::Aab;
    const AndroidToolchain tools = AndroidTools::discover(package.settings.targetSdk);
    if (!tools.ok()) {
        error = "the Android tools are missing (a prebuilt Android build needs no NDK, CMake or Gradle, but these):";
        for (const std::string& missing : tools.missing) error += "\n  - " + missing;
        return false;
    }
    std::error_code ec;
    for (const char* file : {"AndroidManifest.template.xml", "res-flat.zip", "stable-ids.txt"}) {
        if (!fs::is_regular_file(package.runtimeDir / file, ec)) {
            error = "the Android player runtime has no " + std::string(file) + " (" + package.runtimeDir.string() + ")";
            return false;
        }
    }
    const std::vector<std::string> abis = package.abis.empty() ? selectedAndroidAbis(package.settings) : package.abis;
    if (abis.empty()) {
        error = "no Android architecture is selected (Project Settings > Android > Architectures)";
        return false;
    }
    fs::remove_all(package.workDir, ec);
    fs::create_directories(package.workDir, ec);

    // before any work: the signing key, and for a bundle the bundletool jar
    SigningKey key;
    if (!prepareSigningKey(package, tools, runner, warnings, key, error)) return false;
    fs::path bundletool = package.bundletool;
    if (bundle && bundletool.empty()) {
        bundletool = AndroidTools::bundletoolJar(cancelled, error);
        if (bundletool.empty()) {
            if (error == "cancelled") error = "Build cancelled";
            return false;
        }
    }

    // 1. the manifest
    std::string templateText, manifest;
    if (!readText(package.runtimeDir / "AndroidManifest.template.xml", templateText)) {
        error = "cannot read the player's AndroidManifest.template.xml";
        return false;
    }
    if (!writeAndroidManifest(package, templateText, manifest, error)) return false;
    const fs::path manifestPath = package.workDir / "AndroidManifest.xml";
    if (!io::writeFileAtomic(manifestPath, manifest).ok()) {
        error = "cannot write " + manifestPath.string();
        return false;
    }

    // 2. the icons: compiled into flat files of their own
    std::vector<std::string> flats;
    auto compileIcon = [&](const fs::path& source, const std::string& resourcePath) -> bool {
        const fs::path file = source.is_absolute() ? source : package.projectPath / source;
        if (!fs::is_regular_file(file, ec)) {
            error = "Android icon not found: " + file.string();
            return false;
        }
        const fs::path target = package.workDir / "res" / resourcePath;
        fs::create_directories(target.parent_path(), ec);
        fs::copy_file(file, target, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            error = "cannot stage the icon " + file.string();
            return false;
        }
        const fs::path flatDir = package.workDir / "flat";
        fs::create_directories(flatDir, ec);
        if (!run(runner, {tools.aapt2.path.string(), "compile", target.string(), "-o", flatDir.string()}, package.workDir, error)) return false;
        return true;
    };
    const AndroidProjectSettings& settings = package.settings;
    const bool adaptive = !settings.adaptiveIconForeground.empty() && !settings.adaptiveIconBackground.empty();
    if (!settings.launcherIcon.empty() && !adaptive) {
        if (!compileIcon(settings.launcherIcon, "drawable/ic_launcher_game.png")) return false;
    }
    if (adaptive) {
        if (!compileIcon(settings.adaptiveIconForeground, "drawable/ic_launcher_game_foreground.png")) return false;
        if (!compileIcon(settings.adaptiveIconBackground, "drawable/ic_launcher_game_background.png")) return false;
        const fs::path xml = package.workDir / "res" / "mipmap-anydpi-v26" / "ic_launcher_game.xml";
        fs::create_directories(xml.parent_path(), ec);
        io::writeFileAtomic(xml, "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                                 "<adaptive-icon xmlns:android=\"http://schemas.android.com/apk/res/android\">\n"
                                 "    <background android:drawable=\"@drawable/ic_launcher_game_background\" />\n"
                                 "    <foreground android:drawable=\"@drawable/ic_launcher_game_foreground\" />\n"
                                 "</adaptive-icon>\n");
        if (!run(runner, {tools.aapt2.path.string(), "compile", xml.string(), "-o", (package.workDir / "flat").string()}, package.workDir, error))
            return false;
    }
    for (const auto& entry : fs::directory_iterator(package.workDir / "flat", ec)) flats.push_back(entry.path().string());
    if (cancelled()) {
        error = "Build cancelled";
        return false;
    }

    // 3. the link: the player's resources with their stable ids, the game's content as assets/game
    const fs::path assets = package.workDir / "assets";
    if (!copyTree(package.contentDir, assets / "game", error)) return false;
    const fs::path linked = package.workDir / (bundle ? "base-proto.zip" : "base.apk");
    std::vector<std::string> link = {tools.aapt2.path.string(), "link", "-I", tools.androidJar.string(), "--manifest", manifestPath.string(),
                                     "--stable-ids", (package.runtimeDir / "stable-ids.txt").string(), "--rename-manifest-package",
                                     package.applicationId, "--min-sdk-version", std::to_string(settings.minSdk), "--target-sdk-version",
                                     std::to_string(settings.targetSdk), "--version-code", std::to_string(package.versionCode),
                                     "--version-name", package.versionName, "--auto-add-overlay", "-A", assets.string(), "-o",
                                     linked.string()};
    if (bundle) link.push_back("--proto-format");
    link.push_back((package.runtimeDir / "res-flat.zip").string());
    for (const std::string& flat : flats) {
        link.push_back("-R");
        link.push_back(flat);
    }
    if (!run(runner, link, package.workDir, error)) return false;
    if (cancelled()) {
        error = "Build cancelled";
        return false;
    }

    // 4. the code: the dex files, and the libraries of the selected ABIs (stored, not compressed)
    ZipReader linkedZip;
    if (!linkedZip.open(linked, error)) return false;
    const fs::path assembled = package.workDir / (bundle ? "base.zip" : "unaligned.apk");
    ZipWriter writer;
    if (!writer.open(assembled, error)) return false;
    for (const ZipEntryInfo& entry : linkedZip.entries()) {
        if (entry.directory) continue;
        std::vector<uint8_t> bytes;
        if (!linkedZip.read(entry.name, bytes, error)) return false;
        std::string name = entry.name;
        // a bundle module keeps its manifest under manifest/
        if (bundle && name == "AndroidManifest.xml") name = "manifest/AndroidManifest.xml";
        const bool store = entry.stored || name == "resources.arsc";
        if (!writer.add(name, bytes.data(), bytes.size(), store, error)) return false;
    }
    for (const auto& entry : fs::directory_iterator(package.runtimeDir, ec)) {
        const std::string name = entry.path().filename().string();
        if (name.rfind("classes", 0) != 0 || entry.path().extension() != ".dex") continue;
        if (!writer.addFile(bundle ? "dex/" + name : name, entry.path(), false, error)) return false;
    }
    size_t libraries = 0;
    const bool compressLibraries = compressesNativeLibraries(package);
    for (const std::string& abi : abis) {
        const fs::path dir = package.runtimeDir / "lib" / abi;
        if (!fs::is_directory(dir, ec)) {
            error = "the Android player runtime has no " + abi + " libraries (lib/" + abi + ")";
            return false;
        }
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            if (entry.path().extension() != ".so") continue;
            if (!writer.addFile("lib/" + abi + "/" + entry.path().filename().string(), entry.path(), !compressLibraries, error)) return false;
            libraries++;
        }
    }
    if (!writer.finish(error)) return false;
    if (libraries == 0) {
        error = "the Android player runtime has no native libraries";
        return false;
    }

    if (bundle) {
        // 5. a bundle: bundletool builds it, jarsigner signs it
        const fs::path unsignedBundle = package.workDir / "unsigned.aab";
        if (!run(runner, {tools.java.path.string(), "-jar", bundletool.string(), "build-bundle", "--modules=" + assembled.string(),
                          "--output=" + unsignedBundle.string(), "--overwrite"},
                 package.workDir, error))
            return false;
        // (no -sigalg: the one that fits the key, which may be EC)
        if (!run(runner, {tools.jarsigner.path.string(), "-keystore", key.keystore.string(), "-storepass:file", key.storeSecret.path.string(),
                          "-keypass:file", key.keySecret.path.string(), "-digestalg", "SHA-256", "-signedjar",
                          package.output.string(), unsignedBundle.string(), key.alias},
                 package.workDir, error))
            return false;
        return true;
    }

    // 5. an APK: aligned (native libraries on 16 KB pages), signed, verified
    const fs::path aligned = package.workDir / "aligned.apk";
    if (!run(runner, {tools.zipalign.path.string(), "-P", "16", "-f", "4", assembled.string(), aligned.string()}, package.workDir, error))
        return false;
    std::vector<std::string> sign = {tools.apksigner.path.string(), "sign", "--ks", key.keystore.string(), "--ks-key-alias", key.alias,
                                     "--ks-pass", "file:" + key.storeSecret.path.string(), "--key-pass", "file:" + key.keySecret.path.string(),
                                     "--v2-signing-enabled", "true", "--v3-signing-enabled", "true"};
    // devices before Android 7 (API 24) read only v1 signatures
    sign.push_back("--v1-signing-enabled");
    sign.push_back(settings.minSdk < 24 ? "true" : "false");
    sign.push_back("--out");
    sign.push_back(package.output.string());
    sign.push_back(aligned.string());
    if (!run(runner, sign, package.workDir, error)) return false;
    fs::remove(package.output.string() + ".idsig", ec);
    if (!run(runner, {tools.apksigner.path.string(), "verify", "--verbose", package.output.string()}, package.workDir, error)) return false;
    return true;
}
