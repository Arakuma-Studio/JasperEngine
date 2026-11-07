// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ApplePrivacy.h"

#include "util/NativePlugins.h"

#include "tinyxml2.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <map>
#include <regex>
#include <set>

using namespace jasperengine::editor;

namespace {

    namespace fs = std::filesystem;

    const char* const API_PREFIX = "NSPrivacyAccessedAPICategory";
    const char* const DATA_PREFIX = "NSPrivacyCollectedDataType";
    const char* const PURPOSE_PREFIX = "NSPrivacyCollectedDataTypePurpose";

    // Apple's required reason API categories and reasons (developer.apple.com, "Describing use of
    // required reason API", NSPrivacyAccessedAPITypeReasons)
    const std::map<std::string, std::vector<std::string>>& privacyApiReasons() {
        static const std::map<std::string, std::vector<std::string>> reasons = {
            {"ActiveKeyboards", {"3EC4.1", "54BD.1"}},
            {"DiskSpace", {"7D9E.1", "85F4.1", "B728.1", "E174.1"}},
            {"FileTimestamp", {"0A2A.1", "3B52.1", "C617.1", "DDA9.1"}},
            {"SystemBootTime", {"35F9.1", "3D61.1", "8FFB.1"}},
            {"UserDefaults", {"1C8F.1", "AC6B.1", "C56D.1", "CA92.1"}},
        };
        return reasons;
    }

    std::string privacyShortName(const std::string& name, const char* prefix, const std::vector<std::string>& names) {
        const std::string longPrefix = prefix;
        const std::string bare = name.compare(0, longPrefix.size(), longPrefix) == 0 ? name.substr(longPrefix.size()) : name;
        return std::find(names.begin(), names.end(), bare) != names.end() ? bare : std::string();
    }

    void privacySortUnique(std::vector<std::string>& values) {
        std::sort(values.begin(), values.end());
        values.erase(std::unique(values.begin(), values.end()), values.end());
    }

    std::string privacyEscape(const std::string& value) {
        std::string out;
        for (char c : value) {
            switch (c) {
                case '&': out += "&amp;"; break;
                case '<': out += "&lt;"; break;
                case '>': out += "&gt;"; break;
                default: out += c; break;
            }
        }
        return out;
    }

    std::string privacyToUtf8(const fs::path& path) {
        const auto text = path.generic_u8string();
        return std::string(text.begin(), text.end());
    }

    const tinyxml2::XMLElement* privacyValueOf(const tinyxml2::XMLElement* dict, const char* key) {
        for (const tinyxml2::XMLElement* e = dict ? dict->FirstChildElement("key") : nullptr; e; e = e->NextSiblingElement("key")) {
            if (e->GetText() && std::string(e->GetText()) == key) return e->NextSiblingElement();
        }
        return nullptr;
    }

    bool privacyBoolOf(const tinyxml2::XMLElement* value) {
        return value && std::string(value->Name()) == "true";
    }

    std::vector<std::string> privacyStringsOf(const tinyxml2::XMLElement* array) {
        std::vector<std::string> out;
        if (!array || std::string(array->Name()) != "array") return out;
        for (const tinyxml2::XMLElement* e = array->FirstChildElement("string"); e; e = e->NextSiblingElement("string")) {
            if (e->GetText()) out.push_back(e->GetText());
        }
        return out;
    }

    // The PrivacyInfo.xcprivacy of an SDK bundle, wherever in it (a framework's root or
    // Resources/, an xcframework's library folders): the first one in path order
    fs::path privacyBundleManifest(const fs::path& bundle) {
        std::vector<fs::path> found;
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(bundle, fs::directory_options::skip_permission_denied, ec);
             !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (it->path().filename() == "PrivacyInfo.xcprivacy" && it->is_regular_file(ec)) found.push_back(it->path());
        }
        std::sort(found.begin(), found.end());
        return found.empty() ? fs::path() : found.front();
    }

    bool privacyReadText(const fs::path& path, std::string& out) {
        std::ifstream in(path, std::ios::binary);
        if (!in) return false;
        out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        return true;
    }

}

bool ApplePrivacy::Manifest::empty() const {
    return !tracking && trackingDomains.empty() && collectedData.empty() && accessedApis.empty();
}

const ApplePrivacy::Manifest& ApplePrivacy::engineManifest() {
    static const Manifest manifest = [] {
        Manifest m;
        m.accessedApis = {{"FileTimestamp", {"C617.1"}}, {"SystemBootTime", {"35F9.1"}}, {"UserDefaults", {"CA92.1"}}};
        return m;
    }();
    return manifest;
}

const std::vector<std::string>& ApplePrivacy::apiTypes() {
    static const std::vector<std::string> types = {"ActiveKeyboards", "DiskSpace", "FileTimestamp", "SystemBootTime", "UserDefaults"};
    return types;
}

const std::vector<std::string>& ApplePrivacy::reasonsOf(const std::string& apiType) {
    static const std::vector<std::string> none;
    auto it = privacyApiReasons().find(apiTypeName(apiType));
    return it == privacyApiReasons().end() ? none : it->second;
}

std::string ApplePrivacy::reasonText(const std::string& reason) {
    static const std::map<std::string, std::string> texts = {
        {"DDA9.1", "shown to the user"}, {"C617.1", "files inside the app's own containers"},
        {"3B52.1", "files the user chose"}, {"0A2A.1", "a third-party SDK's wrapper"},
        {"35F9.1", "time between events in the app"}, {"8FFB.1", "absolute times of UIKit or AVFoundation events"},
        {"3D61.1", "a bug report the user sends"},
        {"85F4.1", "shown to the user"}, {"E174.1", "enough space before writing"}, {"7D9E.1", "a bug report the user sends"},
        {"B728.1", "a health research app"},
        {"3EC4.1", "a custom keyboard app"}, {"54BD.1", "the UI follows the active keyboards"},
        {"CA92.1", "only this app reads what it saves"}, {"1C8F.1", "the app's group shares it"},
        {"C56D.1", "a third-party SDK's wrapper"}, {"AC6B.1", "managed app configuration"},
    };
    auto it = texts.find(reason);
    return it == texts.end() ? std::string() : it->second;
}

const std::vector<std::string>& ApplePrivacy::dataTypes() {
    static const std::vector<std::string> types = {
        "Name", "EmailAddress", "PhoneNumber", "PhysicalAddress", "OtherUserContactInfo", "Health", "Fitness", "PaymentInfo",
        "CreditInfo", "OtherFinancialInfo", "PreciseLocation", "CoarseLocation", "SensitiveInfo", "Contacts",
        "EmailsOrTextMessages", "PhotosorVideos", "AudioData", "GameplayContent", "CustomerSupport", "OtherUserContent",
        "BrowsingHistory", "SearchHistory", "UserID", "DeviceID", "PurchaseHistory", "ProductInteraction", "AdvertisingData",
        "OtherUsageData", "CrashData", "PerformanceData", "OtherDiagnosticData", "EnvironmentScanning", "Hands", "Head",
        "OtherDataTypes"};
    return types;
}

const std::vector<std::string>& ApplePrivacy::purposes() {
    static const std::vector<std::string> names = {"ThirdPartyAdvertising", "DeveloperAdvertising", "Analytics",
                                                   "ProductPersonalization", "AppFunctionality", "Other"};
    return names;
}

std::string ApplePrivacy::apiTypeName(const std::string& name) {
    return privacyShortName(name, API_PREFIX, apiTypes());
}

std::string ApplePrivacy::dataTypeName(const std::string& name) {
    // NSPrivacyCollectedDataTypePurpose... is no data type
    if (name.compare(0, std::string(PURPOSE_PREFIX).size(), PURPOSE_PREFIX) == 0) return {};
    return privacyShortName(name, DATA_PREFIX, dataTypes());
}

std::string ApplePrivacy::purposeName(const std::string& name) {
    return privacyShortName(name, PURPOSE_PREFIX, purposes());
}

bool ApplePrivacy::isDomain(const std::string& text) {
    if (text.empty() || text.size() > 253 || text.front() == '.' || text.back() == '.' || text.front() == '-') return false;
    bool dot = false;
    for (size_t i = 0; i < text.size(); i++) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c == '.') {
            if (text[i - 1] == '.') return false;
            dot = true;
        } else if (!(std::isalnum(c) || c == '-')) {
            return false;
        }
    }
    return dot;
}

std::string ApplePrivacy::validate(const Manifest& manifest) {
    std::set<std::string> seen;
    for (const std::string& domain : manifest.trackingDomains) {
        if (!isDomain(domain)) return "tracking domain \"" + domain + "\" is not a host name like ads.example.com";
    }
    for (const AccessedApi& api : manifest.accessedApis) {
        const std::string type = apiTypeName(api.type);
        if (type.empty()) {
            std::string list;
            for (const std::string& t : apiTypes()) list += (list.empty() ? "" : ", ") + t;
            return "API type \"" + api.type + "\" must be one of: " + list;
        }
        if (!seen.insert("api:" + type).second) return "API type " + type + " is listed twice";
        if (api.reasons.empty()) return "API type " + type + " needs at least one reason";
        const std::vector<std::string>& valid = reasonsOf(type);
        for (const std::string& reason : api.reasons) {
            if (std::find(valid.begin(), valid.end(), reason) == valid.end()) {
                std::string list;
                for (const std::string& r : valid) list += (list.empty() ? "" : ", ") + r;
                return "reason \"" + reason + "\" is not one of " + type + "'s: " + list;
            }
        }
    }
    for (const CollectedData& data : manifest.collectedData) {
        const std::string type = dataTypeName(data.type);
        if (type.empty()) {
            std::string list;
            for (const std::string& t : dataTypes()) list += (list.empty() ? "" : ", ") + t;
            return "data type \"" + data.type + "\" must be one of: " + list;
        }
        if (!seen.insert("data:" + type).second) return "data type " + type + " is listed twice";
        if (data.purposes.empty()) return "data type " + type + " needs at least one purpose";
        for (const std::string& purpose : data.purposes) {
            if (purposeName(purpose).empty()) {
                std::string list;
                for (const std::string& p : purposes()) list += (list.empty() ? "" : ", ") + p;
                return "purpose \"" + purpose + "\" must be one of: " + list;
            }
        }
    }
    return {};
}

ApplePrivacy::Manifest ApplePrivacy::normalized(Manifest m) {
    privacySortUnique(m.trackingDomains);
    for (AccessedApi& api : m.accessedApis) {
        if (!apiTypeName(api.type).empty()) api.type = apiTypeName(api.type);
        privacySortUnique(api.reasons);
    }
    std::sort(m.accessedApis.begin(), m.accessedApis.end(), [](const AccessedApi& a, const AccessedApi& b) { return a.type < b.type; });
    for (CollectedData& data : m.collectedData) {
        if (!dataTypeName(data.type).empty()) data.type = dataTypeName(data.type);
        for (std::string& purpose : data.purposes) {
            if (!purposeName(purpose).empty()) purpose = purposeName(purpose);
        }
        privacySortUnique(data.purposes);
    }
    std::sort(m.collectedData.begin(), m.collectedData.end(), [](const CollectedData& a, const CollectedData& b) { return a.type < b.type; });
    return m;
}

ApplePrivacy::Manifest ApplePrivacy::merge(const Manifest& a, const Manifest& b) {
    Manifest out = normalized(a);
    const Manifest other = normalized(b);
    out.tracking = out.tracking || other.tracking;
    out.trackingDomains.insert(out.trackingDomains.end(), other.trackingDomains.begin(), other.trackingDomains.end());
    for (const AccessedApi& api : other.accessedApis) {
        auto same = std::find_if(out.accessedApis.begin(), out.accessedApis.end(), [&](const AccessedApi& x) { return x.type == api.type; });
        if (same == out.accessedApis.end()) out.accessedApis.push_back(api);
        else same->reasons.insert(same->reasons.end(), api.reasons.begin(), api.reasons.end());
    }
    for (const CollectedData& data : other.collectedData) {
        auto same = std::find_if(out.collectedData.begin(), out.collectedData.end(), [&](const CollectedData& x) { return x.type == data.type; });
        if (same == out.collectedData.end()) {
            out.collectedData.push_back(data);
        } else {
            same->linked = same->linked || data.linked;
            same->tracking = same->tracking || data.tracking;
            same->purposes.insert(same->purposes.end(), data.purposes.begin(), data.purposes.end());
        }
    }
    return normalized(out);
}

std::string ApplePrivacy::toXml(const Manifest& input) {
    const Manifest m = normalized(input);
    std::string xml = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                      "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
                      "<plist version=\"1.0\">\n<dict>\n";
    // keys in alphabetical order, as Xcode writes them
    xml += "\t<key>NSPrivacyAccessedAPITypes</key>\n";
    if (m.accessedApis.empty()) {
        xml += "\t<array/>\n";
    } else {
        xml += "\t<array>\n";
        for (const AccessedApi& api : m.accessedApis) {
            xml += "\t\t<dict>\n\t\t\t<key>NSPrivacyAccessedAPIType</key>\n\t\t\t<string>" + std::string(API_PREFIX) + privacyEscape(api.type) +
                   "</string>\n\t\t\t<key>NSPrivacyAccessedAPITypeReasons</key>\n\t\t\t<array>\n";
            for (const std::string& reason : api.reasons) xml += "\t\t\t\t<string>" + privacyEscape(reason) + "</string>\n";
            xml += "\t\t\t</array>\n\t\t</dict>\n";
        }
        xml += "\t</array>\n";
    }
    xml += "\t<key>NSPrivacyCollectedDataTypes</key>\n";
    if (m.collectedData.empty()) {
        xml += "\t<array/>\n";
    } else {
        xml += "\t<array>\n";
        for (const CollectedData& data : m.collectedData) {
            xml += "\t\t<dict>\n\t\t\t<key>NSPrivacyCollectedDataType</key>\n\t\t\t<string>" + std::string(DATA_PREFIX) + privacyEscape(data.type) + "</string>\n";
            xml += std::string("\t\t\t<key>NSPrivacyCollectedDataTypeLinked</key>\n\t\t\t") + (data.linked ? "<true/>" : "<false/>") + "\n";
            xml += "\t\t\t<key>NSPrivacyCollectedDataTypePurposes</key>\n\t\t\t<array>\n";
            for (const std::string& purpose : data.purposes) {
                xml += "\t\t\t\t<string>" + std::string(PURPOSE_PREFIX) + privacyEscape(purpose) + "</string>\n";
            }
            xml += "\t\t\t</array>\n";
            xml += std::string("\t\t\t<key>NSPrivacyCollectedDataTypeTracking</key>\n\t\t\t") + (data.tracking ? "<true/>" : "<false/>") + "\n";
            xml += "\t\t</dict>\n";
        }
        xml += "\t</array>\n";
    }
    xml += std::string("\t<key>NSPrivacyTracking</key>\n\t") + (m.tracking ? "<true/>" : "<false/>") + "\n";
    xml += "\t<key>NSPrivacyTrackingDomains</key>\n";
    if (m.trackingDomains.empty()) {
        xml += "\t<array/>\n";
    } else {
        xml += "\t<array>\n";
        for (const std::string& domain : m.trackingDomains) xml += "\t\t<string>" + privacyEscape(domain) + "</string>\n";
        xml += "\t</array>\n";
    }
    xml += "</dict>\n</plist>\n";
    return xml;
}

bool ApplePrivacy::parse(const std::string& xml, Manifest& out, std::vector<std::string>& warnings, std::string& error) {
    out = Manifest();
    tinyxml2::XMLDocument doc;
    if (doc.Parse(xml.c_str(), xml.size()) != tinyxml2::XML_SUCCESS) {
        error = std::string("not XML: ") + (doc.ErrorStr() ? doc.ErrorStr() : "");
        return false;
    }
    const tinyxml2::XMLElement* plist = doc.FirstChildElement("plist");
    const tinyxml2::XMLElement* dict = plist ? plist->FirstChildElement("dict") : nullptr;
    if (!dict) {
        error = "no <plist><dict>";
        return false;
    }
    static const std::set<std::string> known = {"NSPrivacyTracking", "NSPrivacyTrackingDomains", "NSPrivacyCollectedDataTypes",
                                                "NSPrivacyAccessedAPITypes"};
    for (const tinyxml2::XMLElement* key = dict->FirstChildElement("key"); key; key = key->NextSiblingElement("key")) {
        const std::string name = key->GetText() ? key->GetText() : "";
        if (!known.count(name)) warnings.push_back("unknown key " + name + " (left out)");
    }
    out.tracking = privacyBoolOf(privacyValueOf(dict, "NSPrivacyTracking"));
    out.trackingDomains = privacyStringsOf(privacyValueOf(dict, "NSPrivacyTrackingDomains"));
    if (const tinyxml2::XMLElement* apis = privacyValueOf(dict, "NSPrivacyAccessedAPITypes")) {
        for (const tinyxml2::XMLElement* item = apis->FirstChildElement("dict"); item; item = item->NextSiblingElement("dict")) {
            const tinyxml2::XMLElement* type = privacyValueOf(item, "NSPrivacyAccessedAPIType");
            const std::string name = type && type->GetText() ? type->GetText() : "";
            AccessedApi api;
            api.type = apiTypeName(name);
            api.reasons = privacyStringsOf(privacyValueOf(item, "NSPrivacyAccessedAPITypeReasons"));
            if (api.type.empty()) {
                warnings.push_back("unknown API type " + name + " (left out)");
                continue;
            }
            const std::vector<std::string>& valid = reasonsOf(api.type);
            std::vector<std::string> kept;
            for (const std::string& reason : api.reasons) {
                if (std::find(valid.begin(), valid.end(), reason) != valid.end()) kept.push_back(reason);
                else warnings.push_back("unknown reason " + reason + " of " + api.type + " (left out)");
            }
            api.reasons = kept;
            if (!api.reasons.empty()) out.accessedApis.push_back(api);
        }
    }
    if (const tinyxml2::XMLElement* collected = privacyValueOf(dict, "NSPrivacyCollectedDataTypes")) {
        for (const tinyxml2::XMLElement* item = collected->FirstChildElement("dict"); item; item = item->NextSiblingElement("dict")) {
            const tinyxml2::XMLElement* type = privacyValueOf(item, "NSPrivacyCollectedDataType");
            const std::string name = type && type->GetText() ? type->GetText() : "";
            CollectedData data;
            data.type = dataTypeName(name);
            if (data.type.empty()) {
                warnings.push_back("unknown data type " + name + " (left out)");
                continue;
            }
            data.linked = privacyBoolOf(privacyValueOf(item, "NSPrivacyCollectedDataTypeLinked"));
            data.tracking = privacyBoolOf(privacyValueOf(item, "NSPrivacyCollectedDataTypeTracking"));
            for (const std::string& purpose : privacyStringsOf(privacyValueOf(item, "NSPrivacyCollectedDataTypePurposes"))) {
                if (purposeName(purpose).empty()) warnings.push_back("unknown purpose " + purpose + " (left out)");
                else data.purposes.push_back(purposeName(purpose));
            }
            out.collectedData.push_back(data);
        }
    }
    out = normalized(out);
    return true;
}

ApplePrivacy::Composed ApplePrivacy::compose(const fs::path& projectPath, const Manifest& project, const std::string& platform,
                                             bool hasTrackingUsageDescription) {
    Composed composed;
    auto record = [&composed](const Manifest& layer, const std::string& origin) {
        if (layer.tracking) composed.sources.push_back({"tracking", origin});
        for (const std::string& domain : layer.trackingDomains) composed.sources.push_back({"domain " + domain, origin});
        for (const AccessedApi& api : layer.accessedApis) {
            for (const std::string& reason : api.reasons) composed.sources.push_back({api.type + " " + reason, origin});
        }
        for (const CollectedData& data : layer.collectedData) composed.sources.push_back({data.type, origin});
    };
    composed.manifest = normalized(engineManifest());
    record(composed.manifest, "engine");
    const Manifest additions = normalized(project);
    record(additions, "project");
    composed.manifest = merge(composed.manifest, additions);

    // the plugin manifests: loose files, and those of static SDKs and resource bundles
    const NativePlugins::Scan scan = NativePlugins::scan(projectPath);
    std::vector<std::pair<std::string, fs::path>> manifests;   // project-relative name, file
    for (const std::string& folder : {std::string("apple"), platform}) {
        auto files = scan.files.find(folder);
        if (files == scan.files.end()) continue;
        for (const std::string& file : files->second) {
            const fs::path path = scan.root / folder / fs::path(file);
            const std::string relative = "plugins/native/" + folder + "/" + file;
            const std::string name = fs::path(file).filename().string();
            std::string ext = fs::path(file).extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (name == "PrivacyInfo.xcprivacy") {
                manifests.push_back({relative, path});
            } else if (ext == ".framework" || ext == ".xcframework" || ext == ".bundle") {
                // a dynamic framework is embedded and keeps its own; a static SDK's is in the
                // framework or in its resource bundle (Foo_Privacy.bundle), copied into the app
                if (ext != ".bundle") {
                    const fs::path binary = ext == ".framework" ? NativePlugins::frameworkBinary(path) : NativePlugins::xcframeworkBinary(path);
                    if (binary.empty() || !NativePlugins::isStaticArchive(binary)) continue;
                }
                const fs::path inside = privacyBundleManifest(path);
                if (!inside.empty()) manifests.push_back({"plugins/native/" + folder + "/" + privacyToUtf8(fs::path(file) / inside.lexically_relative(path)), inside});
            }
        }
    }
    std::sort(manifests.begin(), manifests.end());
    for (const auto& [relative, path] : manifests) {
        std::string text, error;
        std::vector<std::string> warnings;
        Manifest layer;
        if (!privacyReadText(path, text) || !parse(text, layer, warnings, error)) {
            composed.warnings.push_back(relative + " does not parse (" + (error.empty() ? std::string("unreadable") : error) + "): left out");
            continue;
        }
        for (const std::string& warning : warnings) composed.warnings.push_back(relative + ": " + warning);
        composed.pluginManifests.push_back(relative);
        record(layer, relative);
        composed.manifest = merge(composed.manifest, layer);
    }

    if (composed.manifest.tracking && composed.manifest.trackingDomains.empty()) {
        composed.warnings.push_back("W-PRIVACY-DOMAINS: the privacy manifest declares tracking but no tracking domain "
                                    "(Project Settings > Player > iOS > Privacy Manifest > Tracking Domains)");
    }
    if (platform == "ios" && composed.manifest.tracking && !hasTrackingUsageDescription) {
        composed.warnings.push_back("W-PRIVACY-ATT: the privacy manifest declares tracking but ios.plist_entries has no "
                                    "NSUserTrackingUsageDescription, which App Tracking Transparency needs");
    }
    return composed;
}

bool ApplePrivacy::declaresTrackingUsage(const std::vector<std::string>& plistKeys, const std::string& plistXml) {
    if (std::find(plistKeys.begin(), plistKeys.end(), "NSUserTrackingUsageDescription") != plistKeys.end()) return true;
    static const std::regex key(R"(<key>\s*NSUserTrackingUsageDescription\s*</key>)");
    return std::regex_search(plistXml, key);
}
