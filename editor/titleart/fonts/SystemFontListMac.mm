// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The macOS lister of SystemFontList (docs/specs/S82-title-designer.md R58): CoreText's font files
// and their faces. Core Foundation only (no Objective-C objects), so it builds with or without ARC.

#include "SystemFontList.h"

#include "FontTables.h"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreText/CoreText.h>

#include <climits>
#include <fstream>
#include <set>

namespace jasperengine::editor::titlefonts {

    namespace {

        std::string toUtf8(CFStringRef text) {
            if (!text) return "";
            const CFIndex length = CFStringGetLength(text);
            const CFIndex capacity = CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8) + 1;
            std::string out((size_t)capacity, '\0');
            if (!CFStringGetCString(text, out.data(), capacity, kCFStringEncodingUTF8)) return "";
            out.resize(std::char_traits<char>::length(out.c_str()));
            return out;
        }

        std::string stringAttribute(CTFontDescriptorRef descriptor, CFStringRef attribute) {
            CFTypeRef value = CTFontDescriptorCopyAttribute(descriptor, attribute);
            std::string out;
            if (value && CFGetTypeID(value) == CFStringGetTypeID()) out = toUtf8((CFStringRef)value);
            if (value) CFRelease(value);
            return out;
        }

        int formatAttribute(CTFontDescriptorRef descriptor) {
            CFTypeRef value = CTFontDescriptorCopyAttribute(descriptor, kCTFontFormatAttribute);
            int format = kCTFontFormatUnrecognized;
            if (value && CFGetTypeID(value) == CFNumberGetTypeID()) CFNumberGetValue((CFNumberRef)value, kCFNumberIntType, &format);
            if (value) CFRelease(value);
            return format;
        }

        uint32_t read32(std::ifstream& in, uint64_t offset) {
            uint8_t b[4] = {0, 0, 0, 0};
            in.seekg((std::streamoff)offset);
            in.read(reinterpret_cast<char*>(b), 4);
            return in ? (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3] : 0;
        }

        // each face's PostScript name of a collection, read with seeks (system collections are
        // tens of megabytes: only the header, the directories and the name tables are read)
        std::vector<std::string> collectionPostScriptNames(const std::string& path) {
            std::vector<std::string> names;
            std::ifstream in(path, std::ios::binary);
            if (!in || read32(in, 0) != sfntTag("ttcf")) return names;
            const uint32_t count = read32(in, 8);
            for (uint32_t face = 0; face < count && face < 4096; face++) {
                const uint32_t directory = read32(in, 12 + (uint64_t)face * 4);
                in.seekg((std::streamoff)directory + 4);
                uint8_t countBytes[2] = {0, 0};
                in.read(reinterpret_cast<char*>(countBytes), 2);
                const uint32_t tables = in ? (uint32_t)(countBytes[0] << 8 | countBytes[1]) : 0;
                std::string name;
                for (uint32_t t = 0; t < tables; t++) {
                    const uint64_t record = (uint64_t)directory + 12 + (uint64_t)t * 16;
                    if (read32(in, record) != sfntTag("name")) continue;
                    const uint32_t offset = read32(in, record + 8), length = read32(in, record + 12);
                    if (length == 0 || length > (16u << 20)) break;
                    std::vector<uint8_t> table(length);
                    in.seekg((std::streamoff)offset);
                    in.read(reinterpret_cast<char*>(table.data()), (std::streamsize)length);
                    if (in) name = nameTableString(table.data(), table.size(), 6);
                    break;
                }
                in.clear();
                names.push_back(name);
            }
            return names;
        }

    }

    std::vector<SystemFontFace> listCoreTextFonts() {
        std::vector<SystemFontFace> out;
        CFArrayRef urls = CTFontManagerCopyAvailableFontURLs();
        if (!urls) return out;
        std::set<std::pair<std::string, int>> seen;
        const CFIndex urlCount = CFArrayGetCount(urls);
        for (CFIndex u = 0; u < urlCount; u++) {
            CFURLRef url = (CFURLRef)CFArrayGetValueAtIndex(urls, u);
            char path[PATH_MAX];
            if (!url || !CFURLGetFileSystemRepresentation(url, true, reinterpret_cast<UInt8*>(path), sizeof(path))) continue;
            CFArrayRef descriptors = CTFontManagerCreateFontDescriptorsFromURL(url);
            if (!descriptors) continue;
            const CFIndex count = CFArrayGetCount(descriptors);
            // a collection's faces by PostScript name; a single font is face 0
            std::vector<std::string> facePostScript;
            if (count > 1) facePostScript = collectionPostScriptNames(path);
            for (CFIndex d = 0; d < count; d++) {
                CTFontDescriptorRef descriptor = (CTFontDescriptorRef)CFArrayGetValueAtIndex(descriptors, d);
                const int format = formatAttribute(descriptor);
                SystemFontFace face;
                if (format == kCTFontFormatOpenTypePostScript) face.format = SystemFontFormat::Cff;
                else if (format == kCTFontFormatOpenTypeTrueType || format == kCTFontFormatTrueType) face.format = SystemFontFormat::TrueType;
                else continue;  // Type 1, bitmap, unrecognized
                face.path = path;
                face.family = stringAttribute(descriptor, kCTFontFamilyNameAttribute);
                face.style = stringAttribute(descriptor, kCTFontStyleNameAttribute);
                face.faceIndex = (int)d;
                if (!facePostScript.empty()) {
                    const std::string postScript = stringAttribute(descriptor, kCTFontNameAttribute);
                    for (size_t i = 0; i < facePostScript.size(); i++) {
                        if (facePostScript[i] == postScript) {
                            face.faceIndex = (int)i;
                            break;
                        }
                    }
                }
                if (face.family.empty() || !seen.insert({face.path, face.faceIndex}).second) continue;
                out.push_back(std::move(face));
            }
            CFRelease(descriptors);
        }
        CFRelease(urls);
        return out;
    }

}
