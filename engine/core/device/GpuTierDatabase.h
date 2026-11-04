// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef GPUTIERDATABASE_H
#define GPUTIERDATABASE_H

#include "Export.h"
#include "device/DeviceTypes.h"

#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine {

    // What a renderer string is matched against: the platform's rules ("android", "apple", "pc",
    // or "web", which reads them all: a browser runs on any of them) and, where the platform gives
    // them, the PCI ids
    struct GpuQuery {
        std::string renderer;
        std::string platform;
        uint32_t vendorId = 0;
        uint32_t deviceId = 0;
    };

    struct GpuMatch {
        bool matched = false;
        DeviceTier tier = DeviceTier::High;
        // the rule kind that matched: "device" (the PCI ids), "name", "family" or "vendor"
        std::string kind;
        // the rule's normalized name, pattern or vendor
        std::string rule;
        // the vendor the renderer string names ("qualcomm", "nvidia", "software"...), "" none
        std::string vendor;
    };

    // The GPU database (engine/core/device/gpu_tiers.json, converted from The Forge's gpu.data by
    // tools/device/convert_forge_gpu_data.py, which documents the format): ordered rules from a
    // renderer name to a tier. Matching normalizes the name (normalize) and tries the PCI ids, the
    // longest name the string contains between word boundaries, the families in file order, then
    // the vendor's default. Unknown GPUs match nothing: DeviceDetect picks their tier.
    class JASPERENGINE_API GpuTierDatabase {
    public:
        // the built-in database, parsed on first use (gpu_tiers.json as GpuTiersData.cpp embeds it)
        static const GpuTierDatabase& builtIn();
        static std::string embeddedJson();

        bool load(const std::string& json, std::string& error);
        GpuMatch match(const GpuQuery& query) const;
        size_t getRuleCount() const { return rules.size(); }

        // lower case, "(tm)" and "(r)" dropped, every character but a-z and 0-9 a space, spaces
        // collapsed and trimmed: "Adreno (TM) 650" -> "adreno 650"
        static std::string normalize(const std::string& text);
        // a family pattern (normalized text with \d, [a-z0-9 ranges] and .) from a word start of
        // `normalized` to a word end
        static bool familyMatches(const std::string& pattern, const std::string& normalized);

    private:
        struct Rule {
            std::string platform;
            enum class Kind { Name, Family, Vendor } kind = Kind::Name;
            std::string vendor;
            std::string text;   // the name, the pattern ("" for a vendor rule)
            std::vector<uint32_t> devices;
            DeviceTier tier = DeviceTier::High;
        };
        struct Vendor {
            std::string name;
            uint32_t id = 0;
            std::vector<std::string> tokens;
        };
        std::vector<Rule> rules;
        std::vector<Vendor> vendors;

        static bool platformApplies(const std::string& rulePlatform, const std::string& queryPlatform);
        std::string vendorOf(const std::string& normalized, uint32_t vendorId) const;
        uint32_t vendorIdOf(const std::string& vendor) const;
    };

}

#endif // GPUTIERDATABASE_H
