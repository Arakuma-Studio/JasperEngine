// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "MacBundle.h"

#include "Exporter.h"
#include "Out.h"
#include "io/AtomicFile.h"
#include "platform/ExternalTools.h"
#include "util/CommandRunner.h"

#include "stb_image.h"
#include "stb_image_resize2.h"
#include "stb_image_write.h"

#include <cstdlib>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::platform;
namespace fs = std::filesystem;

std::string platform::infoPlist(const MacBundleSettings& s) {
    auto entry = [](const std::string& key, const std::string& value) {
        return "\t<key>" + key + "</key>\n\t<string>" + Exporter::escapeXml(value) + "</string>\n";
    };
    std::string plist = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
                        "<plist version=\"1.0\">\n<dict>\n";
    plist += entry("CFBundleDevelopmentRegion", "en");
    plist += entry("CFBundleExecutable", s.executable);
    plist += entry("CFBundleIdentifier", s.bundleIdentifier);
    plist += entry("CFBundleInfoDictionaryVersion", "6.0");
    plist += entry("CFBundleName", s.bundleName);
    plist += entry("CFBundleDisplayName", s.bundleName);
    plist += entry("CFBundlePackageType", "APPL");
    plist += entry("CFBundleShortVersionString", s.version);
    plist += entry("CFBundleVersion", s.build);
    plist += entry("CFBundleIconFile", "AppIcon");
    plist += entry("LSMinimumSystemVersion", s.minimumSystemVersion);
    plist += entry("LSApplicationCategoryType", "public.app-category.games");
    plist += entry("NSPrincipalClass", "NSApplication");
    plist += std::string("\t<key>NSHighResolutionCapable</key>\n\t") + (s.highDpi ? "<true/>" : "<false/>") + "\n";
    plist += "</dict>\n</plist>\n";
    return plist;
}

bool platform::writeIcns(const fs::path& png, std::vector<uint8_t>& icns, std::string& error) {
    int width = 0, height = 0, channels = 0;
    unsigned char* pixels = stbi_load(png.string().c_str(), &width, &height, &channels, 4);
    if (!pixels) {
        error = "cannot read the icon " + png.string();
        return false;
    }
    auto put32 = [](std::vector<uint8_t>& out, uint32_t v) {
        for (int i = 3; i >= 0; i--) out.push_back(static_cast<uint8_t>(v >> (i * 8)));
    };
    std::vector<uint8_t> body;
    const std::pair<const char*, int> entries[] = {{"ic07", 128}, {"ic08", 256}, {"ic09", 512}, {"ic10", 1024}};
    for (const auto& [type, size] : entries) {
        std::vector<unsigned char> scaled(static_cast<size_t>(size) * size * 4);
        stbir_resize_uint8_srgb(pixels, width, height, 0, scaled.data(), size, size, 0, STBIR_RGBA);
        std::vector<uint8_t> encoded;
        stbi_write_png_to_func([](void* context, void* data, int length) {
            auto* out = static_cast<std::vector<uint8_t>*>(context);
            out->insert(out->end(), static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + length);
        }, &encoded, size, size, 4, scaled.data(), size * 4);
        body.insert(body.end(), type, type + 4);
        put32(body, static_cast<uint32_t>(encoded.size() + 8));
        body.insert(body.end(), encoded.begin(), encoded.end());
    }
    stbi_image_free(pixels);
    icns.clear();
    const char magic[4] = {'i', 'c', 'n', 's'};
    icns.insert(icns.end(), magic, magic + 4);
    put32(icns, static_cast<uint32_t>(body.size() + 8));
    icns.insert(icns.end(), body.begin(), body.end());
    return true;
}

bool platform::finishMacBundle(const fs::path& bundle, const MacBundleSettings& settings, std::vector<BuildWarning>& warnings,
                               std::string& error) {
    std::error_code ec;
    const fs::path macos = bundle / "Contents" / "MacOS";
    const fs::path templateExe = macos / "jasperengine-player";
    if (fs::exists(templateExe, ec)) fs::rename(templateExe, macos / settings.executable, ec);
    if (ec || !fs::is_regular_file(macos / settings.executable, ec)) {
        error = "the macOS player has no Contents/MacOS/jasperengine-player";
        return false;
    }
    fs::permissions(macos / settings.executable, fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec, fs::perm_options::add, ec);
    std::string plist = infoPlist(settings);
    if (settings.customizePlist) settings.customizePlist(plist);
    if (!io::writeFileAtomic(bundle / "Contents" / "Info.plist", plist).ok()) {
        error = "cannot write Info.plist";
        return false;
    }
    if (!settings.privacyManifest.empty()) {
        fs::create_directories(bundle / "Contents" / "Resources", ec);
        if (!io::writeFileAtomic(bundle / "Contents" / "Resources" / "PrivacyInfo.xcprivacy", settings.privacyManifest).ok()) {
            error = "cannot write PrivacyInfo.xcprivacy";
            return false;
        }
    }
    if (!settings.iconPng.empty() && fs::is_regular_file(settings.iconPng, ec)) {
        std::vector<uint8_t> icns;
        if (!writeIcns(settings.iconPng, icns, error)) return false;
        if (!io::writeFileAtomic(bundle / "Contents" / "Resources" / "AppIcon.icns",
                                 std::string_view(reinterpret_cast<const char*>(icns.data()), icns.size())).ok()) {
            error = "cannot write AppIcon.icns";
            return false;
        }
    }

    // the signature (after every file of the bundle is in place)
    CommandRunner runner;
    int code = -1;
    std::string log;
    auto collect = [&log](const std::string& line) {
        Out::build("%s", line.c_str());
        log += line + "\n";
    };
    const char* identity = std::getenv("JASPER_MACOS_SIGN_IDENTITY");
    const std::string signIdentity = identity && *identity ? identity : settings.signIdentity;
#ifdef __APPLE__
    const ToolLocation codesign = ExternalTools::codesign();
    if (codesign.found()) {
        runner.runArgs({codesign.path.string(), "--force", "--deep", "--options", "runtime", "--sign", signIdentity, bundle.string()},
                       bundle.parent_path(), collect, &code);
        if (code != 0) {
            error = "codesign failed:\n" + log;
            return false;
        }
        return true;
    }
#endif
    const ToolLocation rcodesign = ExternalTools::rcodesign();
    if (rcodesign.found()) {
        runner.runArgs({rcodesign.path.string(), "sign", bundle.string()}, bundle.parent_path(), collect, &code);
        if (code != 0) {
            error = "rcodesign failed:\n" + log;
            return false;
        }
        return true;
    }
    warnings.push_back({"W-MAC-UNSIGNED", bundle.filename().string() + " is not signed (no codesign here, no rcodesign on PATH): an "
                                          "unsigned arm64 Mac app does not launch, and publish verify refuses it. Build it on a Mac, or "
                                          "install rcodesign"});
    return true;
}
