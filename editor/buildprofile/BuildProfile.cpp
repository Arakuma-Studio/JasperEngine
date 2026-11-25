// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "BuildProfile.h"

#include "Exporter.h"
#include "util/CppIdentifier.h"
#include "Out.h"
#include "Project.h"
#include "Workspace.h"
#include "mcp/McpValues.h"
#include "organisation/AssetLabels.h"
#include "publish/StoreLimits.h"
#include "settings/SettingsRegistry.h"
#include "util/FileUtils.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <fstream>
#include <mutex>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;

namespace {

    const char* const kProfileKnownKeys[] = {"formatVersion", "platform", "productName", "identifier", "versionSuffix", "icon", "store",
                                      "storeTitle", "backend", "development", "outputDir", "scenes", "startScene", "defines",
                                      "excludeLabels", "excludePaths", "overrides"};

    bool isProfileKey(const std::string& key) {
        for (const char* known : kProfileKnownKeys)
            if (key == known) return true;
        return false;
    }

    std::string profileLower(std::string text) {
        for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return text;
    }

    const char* profilePlatformLabel(BuildPlatform platform) {
        switch (platform) {
            case BuildPlatform::Windows: return "Windows";
            case BuildPlatform::Linux: return "Linux";
            case BuildPlatform::MacOS: return "macOS";
            case BuildPlatform::Android: return "Android";
            case BuildPlatform::IOS: return "iOS";
            case BuildPlatform::Web: return "Web";
        }
        return "";
    }

    bool readProfileScalar(const YAML::Node& node, const char* key, std::string& out, std::string& error) {
        const YAML::Node value = node[key];
        if (!value.IsDefined() || value.IsNull()) return true;
        if (!value.IsScalar()) {
            error = std::string(key) + " must be a string";
            return false;
        }
        out = value.Scalar();
        return true;
    }

    bool readProfileList(const YAML::Node& node, const char* key, std::vector<std::string>& out, std::string& error) {
        const YAML::Node value = node[key];
        if (!value.IsDefined() || value.IsNull()) return true;
        if (!value.IsSequence()) {
            error = std::string(key) + " must be a list of strings";
            return false;
        }
        for (const YAML::Node& item : value) {
            if (!item.IsScalar()) {
                error = std::string(key) + " must be a list of strings";
                return false;
            }
            out.push_back(item.Scalar());
        }
        return true;
    }

    void emitProfileList(YAML::Emitter& out, const char* key, const std::vector<std::string>& values) {
        if (values.empty()) return;
        out << YAML::Key << key << YAML::Value << YAML::Flow << YAML::BeginSeq;
        for (const std::string& value : values) out << value;
        out << YAML::EndSeq;
    }

    void emitProfileString(YAML::Emitter& out, const char* key, const std::string& value) {
        if (value.empty()) return;
        out << YAML::Key << key << YAML::Value << YAML::DoubleQuoted << value;
    }

    bool isDefineSymbol(const std::string& name) {
        if (name.empty() || name.size() > 64) return false;
        if (!std::isalpha(static_cast<unsigned char>(name[0])) && name[0] != '_') return false;
        for (char c : name)
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') return false;
        return true;
    }

    // the settings rows an override may name (get_project_settings' keys)
    const SettingRow* profileOverridableRow(const std::string& key) {
        const SettingRow* row = SettingsRegistry::get().find(key);
        if (!row || !row->mcp || row->scope != SettingsScope::Project || !row->set) return nullptr;
        return row;
    }

    std::string settingsCategoryOf(const std::string& key) {
        const size_t dot = key.find('.');
        return dot == std::string::npos ? key : key.substr(0, dot);
    }

    bool setProfileValue(Project& project, const std::string& key, const mcp::Json& value, bool apply, std::string& error) {
        const SettingRow* row = profileOverridableRow(key);
        if (!row) {
            error = "is not a Project Settings key";
            return false;
        }
        SettingsContext ctx;
        ctx.projectPtr = &project;
        return row->set(ctx, value, apply, error);
    }

    std::string joinedNames(const std::vector<std::string>& names) {
        std::string text;
        for (const std::string& name : names) text += (text.empty() ? "" : ", ") + name;
        return text;
    }

    // keys a profile cannot override, with the field that owns what they mean
    std::string profileOwnedKeyRefusal(const std::string& key) {
        if (key == "player.product_name")
            return "player.product_name names the executable and the save folder of every variant: set Product Name (values.product_name) instead";
        if (key == "player.start_scene") return "player.start_scene is the project's: set the profile's Scene List and start scene instead";
        if (key == "player.target_platforms") return "player.target_platforms is the project's: a profile builds its own platform";
        if (key == "player.development_build") return "player.development_build is the profile's Development Build: set values.development instead";
        return std::string();
    }

}

// ---------------------------------------------------------------- file

fs::path BuildProfileFile::directory(const fs::path& projectPath) {
    return projectPath / "EditorSettings" / "BuildProfiles";
}

fs::path BuildProfileFile::pathOf(const fs::path& projectPath, const std::string& name) {
    return directory(projectPath) / FileUtils::pathFromUtf8(name + EXTENSION);
}

bool BuildProfileFile::isValidName(const std::string& name, std::string* why) {
    auto fail = [why](const std::string& message) {
        if (why) *why = message;
        return false;
    };
    if (name.empty() || name.size() > 64) return fail("a Build Profile name is 1 to 64 characters");
    for (char c : name) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != ' ' && c != '_' && c != '.' && c != '-')
            return fail("a Build Profile name is letters, digits, spaces, '_', '.' and '-' (\"" + name + "\")");
    }
    if (name.front() == '.' || name.front() == ' ' || name.back() == ' ' || name.back() == '.')
        return fail("a Build Profile name does not start with '.' or a space, nor end with one (\"" + name + "\")");
    return true;
}

bool BuildProfileFile::parse(const std::string& text, BuildProfile& out, std::string& error, bool* newer) {
    if (newer) *newer = false;
    YAML::Node root;
    try {
        root = YAML::Load(text);
    } catch (const YAML::Exception& e) {
        error = std::string("not YAML: ") + e.what();
        return false;
    }
    if (!root.IsMap()) {
        error = "not a Build Profile (a YAML map with formatVersion and platform)";
        return false;
    }
    BuildProfile profile;
    profile.name = out.name;
    try {
        const YAML::Node version = root["formatVersion"];
        if (!version.IsDefined() || !version.IsScalar()) {
            error = "formatVersion is missing";
            return false;
        }
        int format = 0;
        try {
            format = version.as<int>();
        } catch (const YAML::Exception&) {
            error = "formatVersion must be a number";
            return false;
        }
        if (format > BuildProfile::FORMAT_VERSION) {
            if (newer) *newer = true;
            error = "made by a newer editor: update to edit or build it (formatVersion " + std::to_string(format) + ")";
            return false;
        }
        if (format < 1) {
            error = "formatVersion must be 1";
            return false;
        }
        std::string platform;
        if (!readProfileScalar(root, "platform", platform, error)) return false;
        if (platform.empty()) {
            error = "platform is missing (windows, linux, macos, android, ios or web)";
            return false;
        }
        if (!parseBuildPlatform(platform, profile.platform)) {
            error = "platform \"" + platform + "\" is not windows, linux, macos, android, ios or web";
            return false;
        }
        if (!readProfileScalar(root, "productName", profile.productName, error) || !readProfileScalar(root, "identifier", profile.identifier, error) ||
            !readProfileScalar(root, "versionSuffix", profile.versionSuffix, error) || !readProfileScalar(root, "icon", profile.icon, error) ||
            !readProfileScalar(root, "store", profile.store, error) || !readProfileScalar(root, "storeTitle", profile.storeTitle, error) ||
            !readProfileScalar(root, "backend", profile.backend, error) || !readProfileScalar(root, "outputDir", profile.outputDir, error) ||
            !readProfileScalar(root, "startScene", profile.startScene, error))
            return false;
        if (const YAML::Node development = root["development"]; development.IsDefined() && !development.IsNull()) {
            try {
                profile.development = development.as<bool>();
            } catch (const YAML::Exception&) {
                error = "development must be true or false";
                return false;
            }
        }
        if (const YAML::Node scenes = root["scenes"]; scenes.IsDefined() && !scenes.IsNull()) {
            if (!scenes.IsSequence()) {
                error = "scenes must be a list of {guid, path}";
                return false;
            }
            std::vector<BuildProfileScene> list;
            for (const YAML::Node& item : scenes) {
                if (!item.IsMap() || !item["guid"].IsScalar()) {
                    error = "scenes must be a list of {guid, path}";
                    return false;
                }
                BuildProfileScene scene;
                scene.guid = item["guid"].Scalar();
                if (item["path"].IsScalar()) scene.path = item["path"].Scalar();
                list.push_back(std::move(scene));
            }
            profile.scenes = std::move(list);
        }
        if (!readProfileList(root, "defines", profile.defines, error) || !readProfileList(root, "excludeLabels", profile.excludeLabels, error) ||
            !readProfileList(root, "excludePaths", profile.excludePaths, error))
            return false;
        if (const YAML::Node overrides = root["overrides"]; overrides.IsDefined() && !overrides.IsNull()) {
            if (!overrides.IsMap()) {
                error = "overrides must be a map of settings keys to values";
                return false;
            }
            for (const auto& entry : overrides) profile.overrides[entry.first.Scalar()] = mcp::McpValues::yamlToJson(entry.second);
        }
        profile.unknown = YAML::Node(YAML::NodeType::Map);
        for (const auto& entry : root) {
            const std::string key = entry.first.Scalar();
            if (!isProfileKey(key)) profile.unknown[key] = YAML::Clone(entry.second);
        }
    } catch (const YAML::Exception& e) {
        error = e.what();
        return false;
    }
    out = std::move(profile);
    return true;
}

bool BuildProfileFile::read(const fs::path& file, BuildProfile& out, std::string& error, bool* newer) {
    std::ifstream stream(file, std::ios::binary);
    if (!stream) {
        error = "cannot be read";
        if (newer) *newer = false;
        return false;
    }
    std::ostringstream text;
    text << stream.rdbuf();
    out.name = FileUtils::pathToUtf8(file.stem());
    return parse(text.str(), out, error, newer);
}

std::string BuildProfileFile::encode(const BuildProfile& profile) {
    YAML::Emitter out;
    out << YAML::BeginMap;
    out << YAML::Key << "formatVersion" << YAML::Value << BuildProfile::FORMAT_VERSION;
    out << YAML::Key << "platform" << YAML::Value << buildPlatformName(profile.platform);
    emitProfileString(out, "productName", profile.productName);
    emitProfileString(out, "identifier", profile.identifier);
    emitProfileString(out, "versionSuffix", profile.versionSuffix);
    emitProfileString(out, "icon", profile.icon);
    emitProfileString(out, "store", profile.store);
    emitProfileString(out, "storeTitle", profile.storeTitle);
    emitProfileString(out, "backend", profile.backend);
    if (profile.development) out << YAML::Key << "development" << YAML::Value << *profile.development;
    emitProfileString(out, "outputDir", profile.outputDir);
    if (profile.scenes) {
        out << YAML::Key << "scenes" << YAML::Value << YAML::BeginSeq;
        for (const BuildProfileScene& scene : *profile.scenes) {
            out << YAML::Flow << YAML::BeginMap << YAML::Key << "guid" << YAML::Value << scene.guid;
            if (!scene.path.empty()) out << YAML::Key << "path" << YAML::Value << scene.path;
            out << YAML::EndMap;
        }
        out << YAML::EndSeq;
    }
    emitProfileString(out, "startScene", profile.startScene);
    emitProfileList(out, "defines", profile.defines);
    emitProfileList(out, "excludeLabels", profile.excludeLabels);
    emitProfileList(out, "excludePaths", profile.excludePaths);
    if (!profile.overrides.empty()) {
        out << YAML::Key << "overrides" << YAML::Value << YAML::BeginMap;
        for (const auto& [key, value] : profile.overrides) out << YAML::Key << key << YAML::Value << mcp::McpValues::jsonToYaml(value);
        out << YAML::EndMap;
    }
    if (profile.unknown.IsMap()) {
        for (const auto& entry : profile.unknown) out << YAML::Key << entry.first << YAML::Value << entry.second;
    }
    out << YAML::EndMap;
    return std::string(out.c_str()) + "\n";
}

io::WriteResult BuildProfileFile::write(const fs::path& file, const BuildProfile& profile) {
    return io::writeFileAtomic(file, encode(profile));
}

// ---------------------------------------------------------------- the project's profiles

std::vector<BuildProfileEntry> buildprofiles::list(const fs::path& projectPath) {
    std::vector<BuildProfileEntry> entries;
    std::error_code ec;
    const fs::path dir = BuildProfileFile::directory(projectPath);
    if (projectPath.empty() || !fs::is_directory(dir, ec)) return entries;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->path().extension() != BuildProfileFile::EXTENSION || !it->is_regular_file(ec)) continue;
        BuildProfileEntry entry;
        entry.file = it->path();
        entry.name = FileUtils::pathToUtf8(it->path().stem());
        entry.readable = BuildProfileFile::read(entry.file, entry.profile, entry.error, &entry.newer);
        entries.push_back(std::move(entry));
    }
    std::sort(entries.begin(), entries.end(), [](const BuildProfileEntry& a, const BuildProfileEntry& b) { return profileLower(a.name) < profileLower(b.name); });
    return entries;
}

namespace {
    std::atomic<uint64_t> profileRevision{1};
}

uint64_t buildprofiles::revision() {
    return profileRevision.load();
}

void buildprofiles::touch() {
    profileRevision.fetch_add(1);
}

namespace {

    // what the profiles folder and build.yaml hold now, by name, time and size
    std::string profileStamp(const fs::path& projectPath) {
        std::string stamp;
        std::error_code ec;
        auto add = [&stamp](const fs::path& file, std::error_code& error) {
            const auto time = fs::last_write_time(file, error);
            const auto size = fs::file_size(file, error);
            stamp += FileUtils::pathToUtf8(file.filename()) + ":" + std::to_string(static_cast<long long>(time.time_since_epoch().count())) + ":" + std::to_string(size) + "\n";
        };
        const fs::path dir = BuildProfileFile::directory(projectPath);
        if (fs::is_directory(dir, ec)) {
            std::vector<fs::path> files;
            for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) files.push_back(it->path());
            std::sort(files.begin(), files.end());
            for (const fs::path& file : files) add(file, ec);
        }
        const fs::path active = buildprofiles::activeFile(projectPath);
        if (fs::is_regular_file(active, ec)) add(active, ec);
        return stamp;
    }

}

const buildprofiles::View& buildprofiles::view(const fs::path& projectPath) {
    static View current;
    static std::string stamp;
    static std::chrono::steady_clock::time_point checked;
    const auto now = std::chrono::steady_clock::now();
    if (current.project != projectPath || now - checked > std::chrono::seconds(1)) {
        checked = now;
        std::string latest = profileStamp(projectPath);
        if (current.project != projectPath || latest != stamp) {
            stamp = std::move(latest);
            touch();
        }
    }
    if (current.project != projectPath || current.revision != revision()) {
        current.project = projectPath;
        current.revision = revision();
        current.entries = list(projectPath);
        current.activeName = activeName(projectPath);
        current.active.reset();
        if (const BuildProfileEntry* entry = find(current.entries, current.activeName); entry && entry->readable)
            current.active = std::make_shared<const BuildProfile>(entry->profile);
    }
    return current;
}

std::set<std::string> buildprofiles::overriddenKeys(const BuildProfile& profile) {
    std::set<std::string> keys;
    for (const auto& [key, value] : profile.overrides) keys.insert(key);
    if (!profile.productName.empty())
        for (const std::string& key : productNameKeys(profile.platform)) keys.insert(key);
    if (!profile.identifier.empty())
        for (const std::string& key : identifierKeys(profile.platform)) keys.insert(key);
    if (!profile.icon.empty())
        for (const std::string& key : iconKeys(profile.platform)) keys.insert(key);
    if (!profile.versionSuffix.empty()) keys.insert("android.version_name");
    if (profile.development) keys.insert("player.development_build");
    return keys;
}

const BuildProfileEntry* buildprofiles::find(const std::vector<BuildProfileEntry>& entries, const std::string& name) {
    for (const BuildProfileEntry& entry : entries)
        if (profileLower(entry.name) == profileLower(name)) return &entry;
    return nullptr;
}

bool buildprofiles::load(const fs::path& projectPath, const std::string& name, BuildProfile& out, std::string& error) {
    const std::vector<BuildProfileEntry> entries = list(projectPath);
    const BuildProfileEntry* entry = find(entries, name);
    if (!entry) {
        std::vector<std::string> names;
        for (const BuildProfileEntry& e : entries) names.push_back(e.name);
        error = "no Build Profile '" + name + "'" + (names.empty() ? std::string(" (the project has none: edit_build_profile create makes one)")
                                                                   : " (the project has " + joinedNames(names) + ")");
        return false;
    }
    if (!entry->readable) {
        error = "Build Profile '" + entry->name + "' " + entry->error;
        return false;
    }
    out = entry->profile;
    return true;
}

fs::path buildprofiles::activeFile(const fs::path& projectPath) {
    return Workspace::getUserDirectory(projectPath) / "build.yaml";
}

std::string buildprofiles::activeName(const fs::path& projectPath) {
    std::error_code ec;
    const fs::path file = activeFile(projectPath);
    if (projectPath.empty() || !fs::is_regular_file(file, ec)) return std::string();
    try {
        const YAML::Node root = YAML::LoadFile(FileUtils::pathToUtf8(file));
        if (root.IsMap() && root["activeBuildProfile"].IsScalar()) return root["activeBuildProfile"].Scalar();
    } catch (const YAML::Exception&) {
    }
    return std::string();
}

bool buildprofiles::setActiveName(const fs::path& projectPath, const std::string& name, std::string& error) {
    const fs::path file = activeFile(projectPath);
    YAML::Node root(YAML::NodeType::Map);
    std::error_code ec;
    if (fs::is_regular_file(file, ec)) {
        try {
            const YAML::Node existing = YAML::LoadFile(FileUtils::pathToUtf8(file));
            if (existing.IsMap()) root = existing;
        } catch (const YAML::Exception&) {
        }
    }
    if (name.empty()) root.remove("activeBuildProfile");
    else root["activeBuildProfile"] = name;
    YAML::Emitter out;
    out << root;
    const io::WriteResult written = io::writeFileAtomic(file, std::string(out.c_str()) + "\n");
    if (!written.ok()) {
        error = io::describeWriteError(written);
        return false;
    }
    touch();
    return true;
}

std::shared_ptr<const BuildProfile> buildprofiles::active(const fs::path& projectPath) {
    const std::string name = activeName(projectPath);
    if (name.empty()) return nullptr;
    BuildProfile profile;
    std::string error;
    if (!load(projectPath, name, profile, error)) {
        // once per name and project: Play falls back to none
        static std::mutex mutex;
        static std::set<std::string> warned;
        std::lock_guard<std::mutex> lock(mutex);
        if (warned.insert(FileUtils::pathToUtf8(projectPath) + "\n" + name).second)
            Out::warning("The active Build Profile '%s' cannot be used (%s): Play and Build Settings use none", name.c_str(), error.c_str());
        return nullptr;
    }
    return std::make_shared<const BuildProfile>(std::move(profile));
}

// ---------------------------------------------------------------- typed fields

std::vector<std::string> buildprofiles::productNameKeys(BuildPlatform platform) {
    switch (platform) {
        case BuildPlatform::Android: return {"android.application_name"};
        case BuildPlatform::IOS: return {"ios.application_name"};
        case BuildPlatform::MacOS: return {"macos.application_name"};
        case BuildPlatform::Windows: return {"windows.product_name"};
        case BuildPlatform::Linux: return {"linux.application_name"};
        case BuildPlatform::Web: return {"web.application_name"};
    }
    return {};
}

std::vector<std::string> buildprofiles::identifierKeys(BuildPlatform platform) {
    switch (platform) {
        case BuildPlatform::Android: return {"android.package_name"};
        case BuildPlatform::IOS: return {"ios.bundle_identifier"};
        case BuildPlatform::MacOS: return {"macos.bundle_identifier"};
        default: return {"player.identifier"};
    }
}

std::vector<std::string> buildprofiles::iconKeys(BuildPlatform platform) {
    switch (platform) {
        case BuildPlatform::Android: return {"android.launcher_icon"};
        case BuildPlatform::IOS: return {"ios.icon"};
        case BuildPlatform::MacOS: return {"macos.icon"};
        case BuildPlatform::Web: return {"web.favicon", "player.window_icon"};
        default: return {"player.window_icon"};
    }
}

std::string buildprofiles::keyMeaning(const std::string& key) {
    if (key == "android.application_name") return "the Android label";
    if (key == "ios.application_name") return "the iOS display name";
    if (key == "macos.application_name") return "the macOS bundle name";
    if (key == "windows.product_name") return "the Windows product name";
    if (key == "linux.application_name") return "the Linux application name";
    if (key == "web.application_name") return "the web page's name";
    if (key == "android.package_name") return "the Android application id";
    if (key == "ios.bundle_identifier") return "the iOS bundle identifier";
    if (key == "macos.bundle_identifier") return "the macOS bundle identifier";
    if (key == "player.identifier") return "the application identifier";
    if (key == "android.launcher_icon") return "the Android launcher icon";
    if (key == "ios.icon") return "the iOS icon";
    if (key == "macos.icon") return "the macOS icon";
    if (key == "web.favicon") return "the web favicon";
    if (key == "player.window_icon") return "the window icon";
    return key;
}

std::vector<std::string> buildprofiles::allowedCategories(BuildPlatform platform) {
    return {"player", "graphics", "quality", buildPlatformName(platform)};
}

const std::vector<std::string>& buildprofiles::storeNames() {
    static const std::vector<std::string> names = {"none", "google-play", "app-store", "steam", "itch", "web"};
    return names;
}

std::map<std::string, buildprofiles::ResolvedValue> buildprofiles::resolveIdentity(const Project& project, const BuildProfile& profile) {
    std::map<std::string, ResolvedValue> resolved;
    const std::string projectName = project.getName();
    const ApplicationSettings& app = project.getApplicationSettings();
    auto chain = [](std::initializer_list<std::pair<std::string, std::string>> values) {
        for (const auto& [value, source] : values)
            if (!value.empty()) return ResolvedValue{value, source};
        return ResolvedValue{};
    };
    const std::string nameKey = productNameKeys(profile.platform).front();
    const std::string idKey = identifierKeys(profile.platform).front();
    const std::string iconKey = iconKeys(profile.platform).front();
    const std::string fallbackId = "com.jasperengine." + toIdentifier(projectName.empty() ? std::string("JasperEngine") : projectName);
    std::string platformName, platformId, platformVersion, platformIcon;
    switch (profile.platform) {
        case BuildPlatform::Android: {
            const AndroidProjectSettings& s = project.getAndroidProjectSettings();
            platformName = s.applicationName;
            platformId = s.packageName;
            platformVersion = s.versionName;
            platformIcon = FileUtils::pathToGenericUtf8(s.launcherIcon);
            break;
        }
        case BuildPlatform::IOS: {
            const IOSProjectSettings& s = project.getIOSProjectSettings();
            platformName = s.applicationName;
            platformId = s.bundleIdentifier;
            platformVersion = s.versionName;
            platformIcon = FileUtils::pathToGenericUtf8(s.icon);
            break;
        }
        case BuildPlatform::MacOS: {
            const MacOSProjectSettings& s = project.getMacOSProjectSettings();
            platformName = s.applicationName;
            platformId = s.bundleIdentifier;
            platformVersion = s.versionName;
            platformIcon = FileUtils::pathToGenericUtf8(s.icon);
            break;
        }
        case BuildPlatform::Windows: platformName = project.getWindowsProjectSettings().productName; break;
        case BuildPlatform::Linux: platformName = project.getLinuxProjectSettings().applicationName; break;
        case BuildPlatform::Web:
            platformName = project.getWebProjectSettings().applicationName;
            platformIcon = FileUtils::pathToGenericUtf8(project.getWebProjectSettings().favicon);
            break;
    }
    // the chains the platform's build follows (section 3: macOS, iOS and Windows skip the shared Player values)
    const bool sharedChain = profile.platform == BuildPlatform::Android || profile.platform == BuildPlatform::Linux ||
                             profile.platform == BuildPlatform::Web;
    resolved["product_name"] = sharedChain ? chain({{profile.productName, "profile"}, {platformName, nameKey}, {app.name, "player.application_name"},
                                                    {projectName, "Project Name"}})
                                           : chain({{profile.productName, "profile"}, {platformName, nameKey}, {projectName, "Project Name"}});
    if (profile.platform == BuildPlatform::MacOS || profile.platform == BuildPlatform::IOS)
        resolved["identifier"] = chain({{profile.identifier, "profile"}, {platformId, idKey}, {fallbackId, "default"}});
    else if (profile.platform == BuildPlatform::Android)
        resolved["identifier"] = chain({{profile.identifier, "profile"}, {platformId, idKey}, {app.identifier, "player.identifier"},
                                        {ApplicationSettings{}.identifier, "default"}});
    else
        resolved["identifier"] = chain({{profile.identifier, "profile"}, {app.identifier, "player.identifier"}, {ApplicationSettings{}.identifier, "default"}});
    const std::string androidVersion = project.getApplicationVersion(project.getAndroidProjectSettings().versionName);
    ResolvedValue version = profile.platform == BuildPlatform::MacOS || profile.platform == BuildPlatform::IOS
        ? chain({{platformVersion, std::string(buildPlatformName(profile.platform)) + ".version_name"}, {"1.0", "default"}})
        : chain({{project.getAndroidProjectSettings().versionName, "android.version_name"}, {app.version, "player.version"}, {androidVersion, "default"}});
    if (!profile.versionSuffix.empty() && profile.platform != BuildPlatform::MacOS && profile.platform != BuildPlatform::IOS &&
        profile.platform != BuildPlatform::Windows) {
        version.value += profile.versionSuffix;
        version.source += " + profile";
    }
    resolved["version"] = version;
    resolved["icon"] = chain({{profile.icon, "profile"}, {platformIcon, iconKey}, {FileUtils::pathToGenericUtf8(project.getWindowIcon()), "player.window_icon"}});
    return resolved;
}

std::vector<BuildWarning> buildprofiles::storeFindings(const Project& project, const BuildProfile& profile) {
    std::vector<BuildWarning> warnings;
    if (profile.store.empty() || profile.store == "none") return warnings;
    const std::map<std::string, ResolvedValue> identity = resolveIdentity(project, profile);
    publish::StoreLimitInput input;
    input.store = profile.store;
    input.storeTitle = profile.storeTitle;
    input.label = identity.at("product_name").value;
    input.identifier = identity.at("identifier").value;
    input.version = identity.at("version").value;
    for (const publish::StoreFinding& finding : publish::checkStoreLimits(input)) warnings.push_back({finding.code, finding.message});
    return warnings;
}

bool buildprofiles::playProfile(const Project& project, PlayProfile& out) {
    const std::shared_ptr<const BuildProfile> profile = active(project.getProjectPath());
    if (!profile) return false;
    out = PlayProfile();
    out.name = profile->name;
    out.appName = profile->productName.empty() ? project.getApplicationName() : profile->productName;
    out.identifier = profile->identifier.empty() ? project.getApplicationIdentifier() : profile->identifier;
    out.version = project.getApplicationVersion() + profile->versionSuffix;
    out.defines = profile->defines;
    if (profile->scenes) {
        out.hasScenes = true;
        for (const BuildProfileScene& scene : *profile->scenes) out.sceneGuids.insert(Guid::parse(scene.guid).str());
    }
    return true;
}

// ---------------------------------------------------------------- validation

std::vector<std::string> editor::validateProfile(Project& project, const BuildProfile& profile) {
    std::vector<std::string> findings;
    std::string why;
    if (!BuildProfileFile::isValidName(profile.name, &why)) findings.push_back(why);
    const std::string platform = buildPlatformName(profile.platform);

    if (!profile.backend.empty()) {
        const std::vector<std::string> backends = platformBackends(profile.platform);
        const std::string backend = normalizeBackend(profile.backend);
        if (backend.empty() || std::find(backends.begin(), backends.end(), backend) == backends.end())
            findings.push_back("Graphics API " + profile.backend + " has no " + platform + " player: use " + joinedNames(backends) + " (or \"\" for " +
                               backends.front() + ")");
    }
    if (std::find(buildprofiles::storeNames().begin(), buildprofiles::storeNames().end(), profile.store) == buildprofiles::storeNames().end() &&
        !profile.store.empty())
        findings.push_back("Store " + profile.store + " is not one of " + joinedNames(buildprofiles::storeNames()));
    if (!profile.outputDir.empty()) {
        fs::path dir = FileUtils::pathFromUtf8(profile.outputDir);
        if (dir.is_relative()) dir = project.getProjectPath() / dir;
        const std::string refusal = Exporter::outputFolderRefusal(&project, dir.lexically_normal());
        if (!refusal.empty()) findings.push_back("Output Folder: " + refusal);
    }
    if (profile.versionSuffix.size() > 32) findings.push_back("Version Suffix is at most 32 characters");
    for (char c : profile.versionSuffix)
        if (static_cast<unsigned char>(c) < 0x20 || c == ' ') {
            findings.push_back("Version Suffix has no spaces or control characters (\"-demo\")");
            break;
        }

    // typed fields: the settings' own validators, and no override of the same key
    auto typed = [&](const char* label, const std::string& value, const std::vector<std::string>& keys) {
        if (value.empty()) return;
        for (const std::string& key : keys) {
            if (profile.overrides.count(key)) {
                findings.push_back(std::string(label) + " and the override " + key + " both set " + buildprofiles::keyMeaning(key) + ": keep one");
                continue;
            }
            std::string error;
            if (!setProfileValue(project, key, mcp::Json(value), false, error)) findings.push_back(std::string(label) + " (" + key + "): " + error);
        }
    };
    typed("Product Name", profile.productName, buildprofiles::productNameKeys(profile.platform));
    typed("Identifier", profile.identifier, buildprofiles::identifierKeys(profile.platform));
    typed("Icon", profile.icon, buildprofiles::iconKeys(profile.platform));

    // overrides (R13)
    const std::vector<std::string> allowed = buildprofiles::allowedCategories(profile.platform);
    for (const auto& [key, value] : profile.overrides) {
        if (const std::string refusal = profileOwnedKeyRefusal(key); !refusal.empty()) {
            findings.push_back(refusal);
            continue;
        }
        if (!profileOverridableRow(key)) {
            findings.push_back("override " + key + " is not a Project Settings key (get_project_settings lists them)");
            continue;
        }
        const std::string category = settingsCategoryOf(key);
        if (std::find(allowed.begin(), allowed.end(), category) == allowed.end()) {
            static const std::vector<std::string> platforms = {"windows", "linux", "macos", "android", "ios", "web"};
            if (std::find(platforms.begin(), platforms.end(), category) != platforms.end())
                findings.push_back(key + " is " + (category == "android" || category == "ios" ? "an " : "a ") + profilePlatformLabel([&] {
                                       BuildPlatform other = BuildPlatform::Linux;
                                       parseBuildPlatform(category, other);
                                       return other;
                                   }()) + " key; this profile builds for " + profilePlatformLabel(profile.platform) + ": use " + platform +
                                   ".* or player.* keys");
            else
                findings.push_back(key + " is a " + category + " setting, which a Build Profile cannot change: overrides take " + joinedNames(allowed) +
                                   " keys");
            continue;
        }
        std::string error;
        if (!setProfileValue(project, key, value, false, error)) findings.push_back("override " + key + ": " + error);
    }

    // Scene List (R4)
    if (profile.scenes) {
        if (profile.scenes->empty()) findings.push_back("the Scene List is empty: add a scene, or turn Override Scenes In Build off");
        std::set<std::string> listed;
        for (const BuildProfileScene& scene : *profile.scenes) {
            const fs::path path = project.getAssetDatabase().getPath(Guid::parse(scene.guid));
            const Guid guid = Guid::parse(scene.guid);
            if (path.empty() || path.extension() != ".scene") {
                findings.push_back("scene " + (scene.path.empty() ? scene.guid : scene.path) + " is no longer in the project: remove it from the Scene List");
            } else if (std::none_of(project.getScenes().begin(), project.getScenes().end(), [&](const SceneProject& known) {
                           return !known.filepath.empty() && project.getAssetDatabase().getAssetGuid(known.filepath) == guid;
                       })) {
                findings.push_back("scene " + FileUtils::pathToGenericUtf8(path) +
                                   " is not in the project's Scenes In Build: add it there (edit_build_scenes) to list it");
            }
            if (!listed.insert(scene.guid).second) findings.push_back("scene " + (scene.path.empty() ? scene.guid : scene.path) + " is listed twice");
        }
        if (!profile.startScene.empty() && !listed.count(profile.startScene))
            findings.push_back("the start scene is not in the Scene List: pick one of its scenes");
    } else if (!profile.startScene.empty()) {
        findings.push_back("a start scene needs the profile's own Scene List (Override Scenes In Build)");
    }

    // Scripting Define Symbols (R6)
    if (profile.defines.size() > 64) findings.push_back("at most 64 Scripting Define Symbols");
    std::set<std::string> defines;
    for (const std::string& define : profile.defines) {
        if (!isDefineSymbol(define))
            findings.push_back("Scripting Define Symbol \"" + define + "\" must be a letter or '_' then letters, digits or '_' (at most 64)");
        else if (!defines.insert(define).second)
            findings.push_back("Scripting Define Symbol " + define + " is listed twice");
    }

    // content (R10, R11)
    for (const std::string& label : profile.excludeLabels) {
        std::string normalized, labelWhy;
        if (!AssetLabels::normalizeLabel(label, normalized, &labelWhy)) findings.push_back("Exclude Label \"" + label + "\": " + labelWhy);
    }
    for (const std::string& path : profile.excludePaths) {
        if (const std::string refusal = BuildContentFilter::pathRefusal(path); !refusal.empty()) findings.push_back(refusal);
    }
    return findings;
}

// ---------------------------------------------------------------- resolution

std::vector<std::string> editor::profileSceneGuids(const BuildProfile& profile) {
    std::vector<std::string> guids;
    if (profile.scenes)
        for (const BuildProfileScene& scene : *profile.scenes) guids.push_back(scene.guid);
    return guids;
}

std::shared_ptr<Project> editor::resolveBuildSettings(Project& project, const BuildProfile* profile, std::vector<BuildWarning>& warnings,
                                                      std::string& error) {
    if (!profile) return std::shared_ptr<Project>(&project, [](Project*) {});
    const std::vector<std::string> findings = validateProfile(project, *profile);
    if (!findings.empty()) {
        error = "Build Profile '" + profile->name + "': " + findings.front() +
                (findings.size() > 1 ? " (and " + std::to_string(findings.size() - 1) + " more: edit_build_profile get)" : "");
        return nullptr;
    }
    std::shared_ptr<Project> snapshot = Project::makeSettingsSnapshot(project);
    auto apply = [&](const std::string& key, const mcp::Json& value) {
        std::string why;
        if (setProfileValue(*snapshot, key, value, true, why)) return true;
        error = "Build Profile '" + profile->name + "': " + key + ": " + why;
        return false;
    };
    if (!profile->productName.empty()) {
        for (const std::string& key : buildprofiles::productNameKeys(profile->platform))
            if (!apply(key, profile->productName)) return nullptr;
        // a desktop window shows the profile's name unless the project titles it itself
        if (project.getWindowTitle().empty() && !profile->overrides.count("player.window_title") && !apply("player.window_title", profile->productName))
            return nullptr;
    }
    if (!profile->identifier.empty())
        for (const std::string& key : buildprofiles::identifierKeys(profile->platform))
            if (!apply(key, profile->identifier)) return nullptr;
    if (!profile->icon.empty()) {
        for (const std::string& key : buildprofiles::iconKeys(profile->platform))
            if (!apply(key, profile->icon)) return nullptr;
        // the launcher icon replaces the adaptive pair, which Android prefers when set
        if (profile->platform == BuildPlatform::Android &&
            (!apply("android.adaptive_icon_foreground", "") || !apply("android.adaptive_icon_background", "")))
            return nullptr;
    }
    if (!profile->versionSuffix.empty()) {
        // the text version game.json and the Android package carry
        const std::string version = snapshot->getApplicationVersion(snapshot->getAndroidProjectSettings().versionName) + profile->versionSuffix;
        if (!apply("android.version_name", version)) return nullptr;
        if (profile->platform == BuildPlatform::MacOS || profile->platform == BuildPlatform::IOS || profile->platform == BuildPlatform::Windows)
            warnings.push_back({"W-PROFILE-SUFFIX-NUMERIC", "Version Suffix \"" + profile->versionSuffix + "\" is not applied to the " +
                                                                profilePlatformLabel(profile->platform) + " " +
                                                                (profile->platform == BuildPlatform::Windows ? "file version" : "bundle version") +
                                                                " (numbers only); game.json's version carries it"});
    }
    if (profile->development) snapshot->setDevelopmentBuild(*profile->development);
    for (const auto& [key, value] : profile->overrides)
        if (!apply(key, value)) return nullptr;
    for (const BuildWarning& finding : buildprofiles::storeFindings(*snapshot, *profile)) warnings.push_back(finding);
    return snapshot;
}

// ---------------------------------------------------------------- content

std::string BuildContentFilter::pathRefusal(const std::string& path) {
    if (path.empty()) return "an Exclude Path is empty";
    const fs::path p = FileUtils::pathFromUtf8(path);
    if (p.is_absolute() || p.has_root_name() || path[0] == '/' || path[0] == '\\')
        return "Exclude Path " + path + " must be project-relative (music/full/)";
    for (const fs::path& part : p.lexically_normal()) {
        if (part == "..") return "Exclude Path " + path + " leaves the project: name a folder or file inside it";
    }
    const std::string first = FileUtils::pathToUtf8(*p.begin());
    if (first == "." || Project::isNonAssetTopLevelDir(first))
        return "Exclude Path " + path + " names " + first + ", which holds no assets (nothing there ships)";
    return std::string();
}

BuildContentFilter::BuildContentFilter(const BuildProfile* source, const std::map<std::string, std::vector<std::string>>& labelsByPath) {
    if (!source) return;
    profile = source->name;
    for (const std::string& path : source->excludePaths) {
        std::string normalized = FileUtils::pathToGenericUtf8(FileUtils::pathFromUtf8(path).lexically_normal());
        if (normalized.empty()) continue;
        if (path.back() == '/' || path.back() == '\\') {
            if (normalized.back() != '/') normalized += '/';
            prefixes.insert(normalized);
        } else {
            files.insert(normalized);
            // a path naming a folder without the slash leaves it out too
            prefixes.insert(normalized + "/");
        }
    }
    if (source->excludeLabels.empty()) return;
    for (const auto& [path, labels] : labelsByPath) {
        for (const std::string& label : labels) {
            const auto excluded = std::find_if(source->excludeLabels.begin(), source->excludeLabels.end(),
                                               [&label](const std::string& wanted) { return AssetLabels::sameLabel(label, wanted); });
            if (excluded != source->excludeLabels.end()) {
                byLabel[path] = *excluded;
                break;
            }
        }
    }
}

bool BuildContentFilter::excludes(std::string_view projectRelativePath, std::string* why) const {
    if (empty()) return false;
    std::string path(projectRelativePath);
    std::replace(path.begin(), path.end(), '\\', '/');
    while (path.rfind("./", 0) == 0) path.erase(0, 2);
    if (const auto label = byLabel.find(path); label != byLabel.end()) {
        if (why) *why = "label " + label->second;
        return true;
    }
    if (files.count(path)) {
        if (why) *why = "path " + path;
        return true;
    }
    // each folder of the path, shortest first
    for (size_t slash = path.find('/'); slash != std::string::npos; slash = path.find('/', slash + 1)) {
        const std::string_view prefix(path.data(), slash + 1);
        if (const auto found = prefixes.find(prefix); found != prefixes.end()) {
            if (why) *why = "path " + *found;
            return true;
        }
    }
    if (prefixes.count(path + "/")) {
        if (why) *why = "path " + path + "/";
        return true;
    }
    return false;
}

std::map<std::string, std::vector<std::string>> editor::labelledAssetPaths(const Project& project) {
    std::map<std::string, std::vector<std::string>> paths;
    for (const auto& [guid, labels] : project.getAssetLabels().all()) {
        const fs::path path = project.getAssetDatabase().getPath(guid);
        if (!path.empty()) paths[FileUtils::pathToGenericUtf8(path)] = labels;
    }
    return paths;
}
