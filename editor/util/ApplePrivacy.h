// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The Apple privacy manifest (PrivacyInfo.xcprivacy) of an exported iOS or macOS app
// (docs/shipping.md "Privacy manifest"). One per app, composed at export from three layers: the
// engine's required-reason API declarations (always there), the project's additions (Project
// Settings > Player > iOS / macOS > Privacy Manifest, `apple.privacy_*`) and the manifests of the
// game's static native plugins (plugins/native/{apple, ios | macos}). Dynamic frameworks are
// embedded with their own manifest and are left alone.
//
// Names are kept short ("FileTimestamp", "DeviceID", "Analytics"); the XML has Apple's long ones
// (NSPrivacyAccessedAPICategoryFileTimestamp, NSPrivacyCollectedDataTypeDeviceID, ...).

#include <filesystem>
#include <string>
#include <vector>

namespace jasperengine::editor::ApplePrivacy {

    namespace fs = std::filesystem;

    struct AccessedApi {
        std::string type;                   // FileTimestamp, SystemBootTime, DiskSpace, ActiveKeyboards, UserDefaults
        std::vector<std::string> reasons;   // "C617.1", sorted
        bool operator==(const AccessedApi&) const = default;
    };

    struct CollectedData {
        std::string type;                   // "DeviceID" (Apple's NSPrivacyCollectedDataType list)
        bool linked = false;                // linked to the user's identity
        bool tracking = false;              // used for tracking
        std::vector<std::string> purposes;  // "ThirdPartyAdvertising", sorted
        bool operator==(const CollectedData&) const = default;
    };

    struct Manifest {
        bool tracking = false;
        std::vector<std::string> trackingDomains;
        std::vector<CollectedData> collectedData;
        std::vector<AccessedApi> accessedApis;
        bool operator==(const Manifest&) const = default;
        bool empty() const;
    };

    // One entry of a composed manifest and the layer it came from
    struct Source {
        std::string what;      // "UserDefaults CA92.1", "tracking", "domain ads.example.com", "DeviceID"
        std::string origin;    // "engine", "project", or the plugin manifest's project-relative path
    };

    struct Composed {
        Manifest manifest;
        std::vector<Source> sources;
        std::vector<std::string> pluginManifests;   // project-relative paths merged
        std::vector<std::string> warnings;
    };

    // What the engine itself does: NSUserDefaults (UserSettings, CA92.1), file timestamps of the
    // app's own files (stat, last_write_time: C617.1), mach_absolute_time for frame and physics
    // timers (35F9.1); no tracking, no collected data (tools/check_apple_privacy.py keeps it true)
    const Manifest& engineManifest();

    const std::vector<std::string>& apiTypes();
    // The reason codes of an API type (empty for an unknown type)
    const std::vector<std::string>& reasonsOf(const std::string& apiType);
    // What a reason code means, in a few words ("" when unknown)
    std::string reasonText(const std::string& reason);
    const std::vector<std::string>& dataTypes();
    const std::vector<std::string>& purposes();

    // A short or long (NSPrivacy...) name as its short form; "" when it is none of the list
    std::string apiTypeName(const std::string& name);
    std::string dataTypeName(const std::string& name);
    std::string purposeName(const std::string& name);

    // A host name a tracking domain may be: letters, digits, '-' and '.', no scheme or path
    bool isDomain(const std::string& text);

    // "" when the manifest only holds names and codes Apple knows, no duplicate type and valid
    // domains; else the first problem, naming the entry
    std::string validate(const Manifest& manifest);

    // The union of two manifests: tracking when either tracks, domains, purposes and reasons
    // joined and sorted, collected data merged by type (linked and tracking when either is)
    Manifest merge(const Manifest& a, const Manifest& b);
    // Sorted lists and entries, short names: what is stored and compared
    Manifest normalized(Manifest manifest);

    // The whole PrivacyInfo.xcprivacy document: deterministic (sorted, tabs, trailing newline)
    std::string toXml(const Manifest& manifest);
    // Reads a PrivacyInfo.xcprivacy (long or short names); false with the reason when it is no
    // plist dictionary; unknown keys and names go into `warnings`
    bool parse(const std::string& xml, Manifest& out, std::vector<std::string>& warnings, std::string& error);

    // The app's manifest for `platform` ("ios" or "macos"): the engine's, the project's
    // additions and the static plugin manifests below plugins/native/apple and
    // plugins/native/<platform> (loose PrivacyInfo.xcprivacy files, those of frameworks whose
    // binary is a static archive and those of resource bundles); the warnings say what was skipped and what is missing
    // (tracking without domains; iOS tracking without NSUserTrackingUsageDescription when
    // `hasTrackingUsageDescription` is false)
    Composed compose(const fs::path& projectPath, const Manifest& project, const std::string& platform,
                     bool hasTrackingUsageDescription = true);

    // NSUserTrackingUsageDescription is among the iOS Info.plist entries' keys or in the raw
    // Info.plist XML (ios.plist_entries, ios.plist_xml)
    bool declaresTrackingUsage(const std::vector<std::string>& plistKeys, const std::string& plistXml);

}
