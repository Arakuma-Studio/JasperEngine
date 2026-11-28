// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "StoreLimits.h"

#include <cctype>

using namespace jasperengine::editor;

namespace {

    // a dotted id of two or more parts, each accepted by `part`
    bool storeDottedId(const std::string& value, bool (*part)(const std::string&)) {
        if (value.empty()) return false;
        size_t parts = 0, start = 0;
        while (true) {
            const size_t dot = value.find('.', start);
            const std::string piece = value.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
            if (!part(piece)) return false;
            parts++;
            if (dot == std::string::npos) break;
            start = dot + 1;
        }
        return parts >= 2;
    }

    // Google Play's application id: Java identifiers of letters, digits and '_', each starting with a letter
    bool storeAndroidPart(const std::string& piece) {
        if (piece.empty() || !std::isalpha(static_cast<unsigned char>(piece[0]))) return false;
        for (char c : piece)
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') return false;
        return true;
    }

    // an Apple bundle id: letters, digits and '-'
    bool storeApplePart(const std::string& piece) {
        if (piece.empty()) return false;
        for (char c : piece)
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-') return false;
        return true;
    }

    // CFBundleShortVersionString: one to three numbers
    bool storeNumericShortVersion(const std::string& value) {
        if (value.empty()) return false;
        size_t parts = 1;
        bool digit = false;
        for (char c : value) {
            if (c == '.') {
                if (!digit) return false;
                parts++;
                digit = false;
            } else if (std::isdigit(static_cast<unsigned char>(c))) {
                digit = true;
            } else {
                return false;
            }
        }
        return digit && parts <= 3;
    }

}

const std::vector<publish::StoreLimitRow>& publish::storeLimitRows() {
    static const std::vector<StoreLimitRow> rows = {
        // Google Play Console, store listing: "App name" up to 30 characters
        {"google-play", "store_title", 30, "W-STORE-TITLE-LENGTH", "Google Play Console store listing, App name"},
        // App Store Connect, app information: "Name" up to 30 characters
        {"app-store", "store_title", 30, "W-STORE-TITLE-LENGTH", "App Store Connect app information, Name"},
        // launchers truncate home-screen labels past about 12 characters (Android and iOS grids)
        {"google-play", "label", 12, "W-LABEL-TRUNCATED", "Android launcher grid label width"},
        {"app-store", "label", 12, "W-LABEL-TRUNCATED", "iOS home screen label width"},
    };
    return rows;
}

bool publish::isStoreName(const std::string& name) {
    return name.empty() || name == "none" || name == "google-play" || name == "app-store" || name == "steam" || name == "itch" ||
           name == "web";
}

size_t publish::codePoints(std::string_view utf8) {
    size_t count = 0;
    for (unsigned char c : utf8)
        if ((c & 0xC0) != 0x80) count++;
    return count;
}

std::vector<publish::StoreFinding> publish::checkStoreLimits(const StoreLimitInput& input) {
    std::vector<StoreFinding> findings;
    for (const StoreLimitRow& row : storeLimitRows()) {
        if (input.store != row.store) continue;
        const std::string checked = row.checked;
        const std::string& value = checked == "store_title" ? input.storeTitle : input.label;
        const size_t length = codePoints(value);
        if (length <= row.limit) continue;
        if (checked == "store_title")
            findings.push_back({row.code, "Store Title \"" + value + "\" is " + std::to_string(length) + " characters; " + input.store + " allows " +
                                              std::to_string(row.limit) + ": shorten it", true});
        else
            findings.push_back({row.code, "the home-screen label \"" + value + "\" is " + std::to_string(length) + " characters; launchers " +
                                              "truncate past " + std::to_string(row.limit) + ": set a shorter Product Name", false});
    }
    if (input.store == "google-play" && !storeDottedId(input.identifier, storeAndroidPart))
        findings.push_back({"E-STORE-IDENTIFIER", "the application id \"" + input.identifier + "\" breaks Google Play's rules (two or more "
                                                  "dot-separated parts of letters, digits and '_', each starting with a letter): set Identifier", true});
    if (input.store == "app-store") {
        if (!storeDottedId(input.identifier, storeApplePart))
            findings.push_back({"E-STORE-IDENTIFIER", "the bundle id \"" + input.identifier + "\" breaks the App Store's rules (dot-separated "
                                                      "parts of letters, digits and '-'): set Identifier", true});
        if (!storeNumericShortVersion(input.version))
            findings.push_back({"E-STORE-VERSION", "the version \"" + input.version + "\" is not the App Store's numeric short version "
                                                   "(1 to 3 numbers, e.g. 1.2.0): set ios.version_name or player.version", true});
    }
    return findings;
}
