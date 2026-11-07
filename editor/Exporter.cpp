// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "util/SafeWrite.h"
#include "component/Joint2DComponent.h"
#include "component/Joint3DComponent.h"
#include "component/Rigidbody2DComponent.h"
#include "component/RigidbodyComponent.h"
#include "Exporter.h"
#include "io/watch/LinkedFolders.h"

#include "audio/SoundEventDocument.h"
#include "util/ProjectChecks.h"

#include "audio/AudioCredits.h"
#include "audio/AudioImport.h"
#include "audio/AudioImportRegistry.h"
#include "shader/ShaderWarmup.h"
#include "shader/ShaderVariantCollector.h"
#include "player/PlayerModules.h"
#include "util/CMakeText.h"
#include "util/EditorScripts.h"
#include "util/ExportFeatureScan.h"
#include "yaml-cpp/yaml.h"
#include "util/Util.h"
#include <regex>
#include "util/NativeExportAndroid.h"
#include "util/NativePlugins.h"
#include "util/ApplePrivacy.h"
#include "util/PngEditorChunks.h"
#include "navigation/NavigationFindings.h"
#include "asset/AssetFileWrite.h"
#include "asset/AssetMap.h"
#include "asset/AssetTypes.h"
#include "util/PngWriter.h"
#include "util/AtomicFileStream.h"
#include "EditorHost.h"
#include "Factory.h"
#include "Generator.h"
#include "Out.h"
#include "App.h"
#include "util/Sha256.h"
#include "Stream.h"
#include "util/Base64.h"
#include "util/FbxImporter.h"
#include "util/CubeLut.h"
#include "io/AtomicFile.h"
#include "util/FileUtils.h"
#include "util/Util.h"
#include "util/ShellEnv.h"
#include "util/MsBuildProgress.h"
#include "util/ShaderHeaderBuilder.h"
#include "pool/ShaderPool.h"
#include "subsystem/RenderSystem.h"
#include "shader/ShaderManifest.h"
#include "shader/ShaderVariantCompiler.h"
#include "render/shadow/ShadowTiers.h"
#include "htmlui/HtmlUIDependencies.h"
#include "htmlui/compat/CompatApi.h"
#include "htmlui/compat/ua/UserAgentStyle.h"
#include "htmlui/compat/runtime/LucideIcons.h"
#include "component/UIDocumentComponent.h"
#include "texture/CookedTextures.h"
#include "texture/TextureExport.h"
#include "model/ModelExport.h"
#include "pixelart/aseprite/AsepriteImporter.h"
#include "sprite/SpriteAtlasBuilder.h"
#include "sprite/SpriteSheetBuilder.h"
#include "sprite/SpriteSheetSources.h"
#include "asset/DepEdge.h"
#include "asset/ImportedAssets.h"
#include "sprite/SpriteSheetPool.h"
#include "util/StringUtils.h"
#include "resources/icons/jasperengine-logo_png.h"

#include "stb_image.h"
#include "tinyxml2.h"
#include "stb_image_resize2.h"
#include "stb_image_write.h"

#include <algorithm>
#include <chrono>
#include <thread>
#include <array>
#include <set>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <future>
#include <optional>
#include <iterator>
#include <limits>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <vector>


namespace {
    // fs::remove_all that never follows a link: the entry removed beneath its own (resolved)
    // folder, a link in the tree removed as a link (util/SafeWrite.h). ec set on failure.
    void removeAllNoFollow(const std::filesystem::path& path, std::error_code& ec) {
        ec.clear();
        std::error_code status;
        if (!std::filesystem::exists(std::filesystem::symlink_status(path, status))) return;
        const std::filesystem::path root = jasperengine::editor::safewrite::existingFolderOf(path.parent_path());
        std::string error;
        if (root.empty() || !jasperengine::editor::safewrite::removeTreeBeneath(root, jasperengine::editor::safewrite::entryLocation(path), error)) {
            ec = std::make_error_code(std::errc::operation_not_permitted);
            jasperengine::editor::safewrite::warn(error.empty() ? "can't remove " + path.string() : error);
        }
    }
}

using namespace jasperengine;

namespace {

    constexpr float BUILD_PROGRESS_START = 0.6f;
    constexpr float BUILD_PROGRESS_END = 0.95f;
    constexpr float MSBUILD_COMPILE_END = 0.9f;
    constexpr char RESOURCE_PACK_MAGIC[] = {'D', 'X', 'P', 'K', '1'};
    constexpr const char* RESOURCE_PACK_FILENAME = "resources.pak";
    constexpr const char* RESOURCE_PACK_TEMP_FILENAME = "resources.pak.tmp";
    // The reader seeks with off_t, still 32-bit on the armeabi-v7a and x86 ABIs
    constexpr uint64_t MAX_RESOURCE_PACK_SIZE = INT32_MAX;

    struct ResourcePackBuildEntry {
        std::string path;      // key inside the pack, relative to the export root
        fs::path source;
        uint64_t size = 0;
        uint64_t offset = 0;
        uint8_t key = 0;
        uint32_t shift = 0;
    };

    // Parses make-style "[ 47%]" and ninja-style "[123/456]" build-line prefixes
    // into a 0..1 fraction so the compile step can drive the progress bar.
    bool parseBuildProgress(const std::string& line, float& fraction) {
        if (line.empty() || line[0] != '[') return false;
        size_t close = line.find(']');
        if (close == std::string::npos || close > 16) return false;
        std::string inner = line.substr(1, close - 1);
        try {
            size_t slash = inner.find('/');
            if (slash != std::string::npos) {
                int current = std::stoi(inner.substr(0, slash));
                int total = std::stoi(inner.substr(slash + 1));
                if (total <= 0 || current < 0 || current > total) return false;
                fraction = (float)current / (float)total;
                return true;
            }
            size_t percent = inner.find('%');
            if (percent == std::string::npos) return false;
            int value = std::stoi(inner.substr(0, percent));
            if (value < 0 || value > 100) return false;
            fraction = value / 100.0f;
            return true;
        } catch (...) {
            return false;
        }
    }

    void writeU8(std::ostream& out, uint8_t value) {
        out.put(static_cast<char>(value));
    }

    void writeU16(std::ostream& out, uint16_t value) {
        out.put(static_cast<char>(value & 0xff));
        out.put(static_cast<char>((value >> 8) & 0xff));
    }

    void writeU32(std::ostream& out, uint32_t value) {
        for (int i = 0; i < 4; i++) {
            out.put(static_cast<char>((value >> (i * 8)) & 0xff));
        }
    }

    void writeU64(std::ostream& out, uint64_t value) {
        for (int i = 0; i < 8; i++) {
            out.put(static_cast<char>((value >> (i * 8)) & 0xff));
        }
    }

    // Undone by ResourcePack::deobfuscate. Not encryption: the key is in the header.
    void obfuscate(std::vector<unsigned char>& data, uint8_t key, uint32_t shift) {
        if (data.empty()) return;

        shift %= data.size();
        if (shift > 0) {
            std::rotate(data.begin(), data.end() - shift, data.end());
        }

        for (unsigned char& byte : data) {
            byte ^= key;
        }
    }

    bool isReservedPackName(std::string name) {
        std::transform(name.begin(), name.end(), name.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        return name == RESOURCE_PACK_FILENAME || name == RESOURCE_PACK_TEMP_FILENAME;
    }

    // From what the header already carries, so both passes agree without the bytes
    void deriveObfuscation(ResourcePackBuildEntry& entry) {
        const size_t hash = std::hash<std::string>{}(entry.path) ^ (static_cast<size_t>(entry.size) << 1);
        entry.key = static_cast<uint8_t>((hash % 255) + 1);
        entry.shift = entry.size == 0 ? 0 : static_cast<uint32_t>(hash % entry.size);
    }

    bool readWholeFile(const fs::path& path, std::vector<unsigned char>& data) {
        std::ifstream in(path, std::ios::binary);
        if (!in) return false;
        in.seekg(0, std::ios::end);
        std::streamoff size = in.tellg();
        if (size < 0) return false;
        in.seekg(0, std::ios::beg);
        data.resize(static_cast<size_t>(size));
        return data.empty() || static_cast<bool>(in.read(reinterpret_cast<char*>(data.data()), size));
    }

    void replaceAll(std::string& value, const std::string& from, const std::string& to) {
        if (from.empty()) return;

        size_t pos = 0;
        while ((pos = value.find(from, pos)) != std::string::npos) {
            value.replace(pos, from.size(), to);
            pos += to.size();
        }
    }

    // Windows CI checkouts and the shipped editor artifact use CRLF. Template
    // patches match LF-only snippets, so normalize before searching/replacing.
    void normalizeNewlines(std::string& text) {
        std::string out;
        out.reserve(text.size());
        for (size_t i = 0; i < text.size(); ++i) {
            if (text[i] == '\r') {
                out.push_back('\n');
                if (i + 1 < text.size() && text[i + 1] == '\n') {
                    ++i;
                }
            } else {
                out.push_back(text[i]);
            }
        }
        text.swap(out);
    }

    std::string escapeGradleString(const std::string& value) {
        std::string out;
        out.reserve(value.size());
        for (char c : value) {
            if (c == '\\' || c == '"' || c == '$') {
                out += '\\';
                out += c;
            } else if (c == '\n') {
                out += "\\n";
            } else if (c == '\r') {
                out += "\\r";
            } else if (c == '\t') {
                out += "\\t";
            } else {
                out += c;
            }
        }
        return out;
    }

    // Bytes other than ASCII letters, digits, . and - become _XX: safe in a define, still unique
    std::string toStorageId(const std::string& value) {
        static const char hex[] = "0123456789ABCDEF";
        std::string out;
        for (unsigned char c : value) {
            if (c < 0x80 && (std::isalnum(c) || c == '.' || c == '-')) {
                out += static_cast<char>(c);
            } else {
                out += '_';
                out += hex[c >> 4];
                out += hex[c & 0xF];
            }
        }
        return out;
    }

    std::string escapeXmlAttribute(const std::string& value) {
        std::string out;
        out.reserve(value.size());
        for (char c : value) {
            switch (c) {
                case '&': out += "&amp;"; break;
                case '<': out += "&lt;"; break;
                case '>': out += "&gt;"; break;
                case '"': out += "&quot;"; break;
                case '\'': out += "&apos;"; break;
                default: out += c; break;
            }
        }
        return out;
    }

    // Android parses string-resource escapes after decoding XML entities.
    // Quoting preserves whitespace and literal @/? prefixes in application names.
    std::string escapeAndroidStringResource(const std::string& value) {
        std::string out = "\"";
        for (char c : value) {
            switch (c) {
                case '\\': out += "\\\\"; break;
                case '"': out += "\\\""; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default: out += c; break;
            }
        }
        return escapeXmlAttribute(out + '"');
    }

    // Platform services a project's scripts use that need an Android permission
    // (System.vibrate: VIBRATE, System.getInternetReachability: ACCESS_NETWORK_STATE), found
    // by name in its Lua and C++ sources; a mention in a comment adds a harmless permission
    struct ScriptServiceUse {
        bool vibrate = false;
        bool reachability = false;
        fs::path vibrateFile;
        fs::path reachabilityFile;
    };

    ScriptServiceUse scanScriptServiceUse(const std::vector<fs::path>& roots) {
        ScriptServiceUse use;
        std::error_code ec;
        std::set<fs::path> seen;
        for (const fs::path& root : roots) {
            if (!fs::is_directory(root, ec)) continue;
            watch::LinkedDirectoryIterator it(root, fs::directory_options::skip_permission_denied, ec), end;
            for (; !ec && it != end; it.increment(ec)) {
                const fs::path& path = it->path();
                const std::string name = path.filename().string();
                std::error_code entryError;
                if (it->is_directory(entryError)) {
                    if (!name.empty() && name[0] == '.') it.disable_recursion_pending();
                    continue;
                }
                std::string extension = path.extension().string();
                for (char& c : extension) c = (char)std::tolower((unsigned char)c);
                const bool lua = extension == ".lua";
                const bool cpp = extension == ".cpp" || extension == ".cc" || extension == ".cxx" || extension == ".h" || extension == ".hpp";
                if (!lua && !cpp) continue;
                if (!seen.insert(fs::weakly_canonical(path, entryError)).second) continue;
                const uintmax_t size = it->file_size(entryError);
                if (entryError || size > 4 * 1024 * 1024) continue;
                std::ifstream file(path, std::ios::binary);
                const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
                auto mentions = [&](const char* luaName, const char* cppCall) {
                    return lua ? text.find(luaName) != std::string::npos : text.find(cppCall) != std::string::npos;
                };
                if (!use.vibrate && (mentions("System.vibrate", "vibrate(") || mentions("System:vibrate", ".vibrate("))) {
                    use.vibrate = true;
                    use.vibrateFile = path;
                }
                if (!use.reachability && mentions("getInternetReachability", "getInternetReachability(")) {
                    use.reachability = true;
                    use.reachabilityFile = path;
                }
                if (use.vibrate && use.reachability) return use;
            }
        }
        return use;
    }

    const char* androidPermissionManifestName(const std::string& key) {
        struct PermissionMap {
            const char* key;
            const char* manifestName;
        };
        static const PermissionMap permissions[] = {
            { "internet", "INTERNET" },
            { "access_network_state", "ACCESS_NETWORK_STATE" },
            { "access_wifi_state", "ACCESS_WIFI_STATE" },
            { "change_network_state", "CHANGE_NETWORK_STATE" },
            { "change_wifi_state", "CHANGE_WIFI_STATE" },
            { "vibrate", "VIBRATE" },
            { "wake_lock", "WAKE_LOCK" },
            { "post_notifications", "POST_NOTIFICATIONS" },
            { "camera", "CAMERA" },
            { "record_audio", "RECORD_AUDIO" },
            { "access_coarse_location", "ACCESS_COARSE_LOCATION" },
            { "access_fine_location", "ACCESS_FINE_LOCATION" },
            { "access_location_extra_commands", "ACCESS_LOCATION_EXTRA_COMMANDS" },
            { "access_media_location", "ACCESS_MEDIA_LOCATION" },
            { "read_external_storage", "READ_EXTERNAL_STORAGE" },
            { "write_external_storage", "WRITE_EXTERNAL_STORAGE" },
            { "manage_external_storage", "MANAGE_EXTERNAL_STORAGE" },
            { "read_media_audio", "READ_MEDIA_AUDIO" },
            { "read_media_images", "READ_MEDIA_IMAGES" },
            { "read_media_video", "READ_MEDIA_VIDEO" },
            { "read_media_visual_user_selected", "READ_MEDIA_VISUAL_USER_SELECTED" },
            { "bluetooth", "BLUETOOTH" },
            { "bluetooth_admin", "BLUETOOTH_ADMIN" },
            { "bluetooth_connect", "BLUETOOTH_CONNECT" },
            { "bluetooth_scan", "BLUETOOTH_SCAN" },
            { "nfc", "NFC" },
            { "transmit_ir", "TRANSMIT_IR" },
            { "use_biometric", "USE_BIOMETRIC" },
            { "use_fingerprint", "USE_FINGERPRINT" },
            { "read_contacts", "READ_CONTACTS" },
            { "write_contacts", "WRITE_CONTACTS" },
            { "get_accounts", "GET_ACCOUNTS" },
            { "read_calendar", "READ_CALENDAR" },
            { "write_calendar", "WRITE_CALENDAR" },
            { "read_call_log", "READ_CALL_LOG" },
            { "write_call_log", "WRITE_CALL_LOG" },
            { "read_phone_state", "READ_PHONE_STATE" },
            { "call_phone", "CALL_PHONE" },
            { "read_sms", "READ_SMS" },
            { "write_sms", "WRITE_SMS" },
            { "send_sms", "SEND_SMS" },
            { "receive_sms", "RECEIVE_SMS" },
            { "receive_mms", "RECEIVE_MMS" },
            { "receive_wap_push", "RECEIVE_WAP_PUSH" },
            { "receive_boot_completed", "RECEIVE_BOOT_COMPLETED" },
            { "kill_background_processes", "KILL_BACKGROUND_PROCESSES" },
            { "modify_audio_settings", "MODIFY_AUDIO_SETTINGS" },
            { "set_wallpaper", "SET_WALLPAPER" },
            { "set_wallpaper_hints", "SET_WALLPAPER_HINTS" },
            { "write_settings", "WRITE_SETTINGS" },
        };
        for (const PermissionMap& permission : permissions) {
            if (key == permission.key) return permission.manifestName;
        }
        return nullptr;
    }

    fs::path resolveProjectFile(editor::Project* project, const fs::path& path) {
        if (path.empty() || path.is_absolute()) return path;
        return project->getProjectPath() / path;
    }

    bool copyAndroidResourceFile(editor::Project* project, const fs::path& sourcePath, const fs::path& targetPath, std::string& error) {
        if (sourcePath.empty()) return true;

        std::error_code ec;
        fs::create_directories(targetPath.parent_path(), ec);
        if (ec) {
            error = "Failed to create Android resource directory: " + ec.message();
            return false;
        }

        fs::path source = resolveProjectFile(project, sourcePath);
        fs::copy_file(source, targetPath, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            error = "Failed to copy Android icon " + source.string() + ": " + ec.message();
            return false;
        }
        return true;
    }

    std::string stripDesktopEntryControlChars(const std::string& value) {
        std::string out;
        out.reserve(value.size());
        for (char c : value) {
            out += (c == '\n' || c == '\r') ? ' ' : c;
        }
        return out;
    }

    void replacePlistStringValue(std::string& plist, const std::string& key, const std::string& value) {
        const std::string keyTag = "\t<key>" + key + "</key>";
        size_t keyPos = plist.find(keyTag);
        if (keyPos == std::string::npos) {
            const size_t dictEnd = plist.rfind("</dict>");
            if (dictEnd == std::string::npos) return;
            plist.insert(dictEnd, "\t<key>" + key + "</key>\n\t<string>" + escapeXmlAttribute(value) + "</string>\n");
            return;
        }

        size_t stringStart = plist.find("<string>", keyPos);
        size_t stringEnd = plist.find("</string>", stringStart);
        if (stringStart == std::string::npos || stringEnd == std::string::npos) return;
        stringStart += std::string("<string>").size();
        plist.replace(stringStart, stringEnd - stringStart, escapeXmlAttribute(value));
    }

    // Sets a top-level key to an array of strings, replacing its value or adding the key
    void replacePlistStringArray(std::string& plist, const std::string& key, const std::vector<std::string>& values) {
        std::string array = "\t<array>\n";
        for (const std::string& value : values) array += "\t\t<string>" + escapeXmlAttribute(value) + "</string>\n";
        array += "\t</array>";

        const std::string keyTag = "\t<key>" + key + "</key>";
        const size_t keyPos = plist.find(keyTag);
        if (keyPos == std::string::npos) {
            const size_t dictEnd = plist.rfind("</dict>");
            if (dictEnd == std::string::npos) return;
            plist.insert(dictEnd, keyTag + "\n" + array + "\n");
            return;
        }
        const size_t valueStart = plist.find_first_not_of(" \t\r\n", keyPos + keyTag.size());
        if (valueStart == std::string::npos) return;
        size_t valueEnd = std::string::npos;
        if (plist.compare(valueStart, 7, "<array>") == 0) {
            valueEnd = plist.find("</array>", valueStart);
            if (valueEnd != std::string::npos) valueEnd += std::string("</array>").size();
        } else if (plist.compare(valueStart, 8, "<array/>") == 0) {
            valueEnd = valueStart + 8;
        }
        if (valueEnd == std::string::npos) return;
        // the value's own indentation is in `array`
        const size_t lineStart = plist.rfind('\n', valueStart);
        const size_t replaceStart = lineStart == std::string::npos ? valueStart : lineStart + 1;
        plist.replace(replaceStart, valueEnd - replaceStart, array);
    }

    void replacePlistBoolValue(std::string& plist, const std::string& key, bool value) {
        const std::string keyTag = "\t<key>" + key + "</key>";
        const std::string boolTag = value ? "\t<true/>\n" : "\t<false/>\n";
        size_t keyPos = plist.find(keyTag);
        if (keyPos == std::string::npos) {
            const size_t dictEnd = plist.rfind("</dict>");
            if (dictEnd == std::string::npos) return;
            plist.insert(dictEnd, keyTag + "\n" + boolTag);
            return;
        }

        size_t valueStart = plist.find_first_not_of(" \t\r\n", keyPos + keyTag.size());
        if (valueStart == std::string::npos) return;
        if (plist.compare(valueStart, 7, "<true/>") == 0) {
            plist.replace(valueStart, 7, value ? "<true/>" : "<false/>");
        } else if (plist.compare(valueStart, 8, "<false/>") == 0) {
            plist.replace(valueStart, 8, value ? "<true/>" : "<false/>");
        }
    }

    // Replaces (or adds) the top-level key holding a dictionary; `body` is the dictionary's
    // inner lines, indented with two tabs
    void replacePlistDictValue(std::string& plist, const std::string& key, const std::string& body) {
        const std::string keyTag = "\t<key>" + key + "</key>";
        const std::string value = "\t<dict>\n" + body + "\t</dict>\n";
        size_t keyPos = plist.find(keyTag);
        if (keyPos == std::string::npos) {
            const size_t dictEnd = plist.rfind("</dict>");
            if (dictEnd == std::string::npos) return;
            plist.insert(dictEnd, keyTag + "\n" + value);
            return;
        }
        const size_t valueStart = plist.find_first_not_of(" \t\r\n", keyPos + keyTag.size());
        if (valueStart == std::string::npos || plist.compare(valueStart, 6, "<dict>") != 0) return;
        // the matching </dict> (nested dictionaries counted)
        int depth = 0;
        size_t pos = valueStart;
        size_t valueEnd = std::string::npos;
        while (pos < plist.size()) {
            const size_t open = plist.find("<dict>", pos);
            const size_t close = plist.find("</dict>", pos);
            if (close == std::string::npos) return;
            if (open != std::string::npos && open < close) {
                depth++;
                pos = open + 6;
            } else {
                depth--;
                pos = close + 7;
                if (depth == 0) {
                    valueEnd = pos;
                    break;
                }
            }
        }
        if (valueEnd == std::string::npos) return;
        if (valueEnd < plist.size() && plist[valueEnd] == '\n') valueEnd++;
        const size_t lineStart = plist.rfind('\n', valueStart);
        const size_t eraseStart = lineStart == std::string::npos ? valueStart : lineStart + 1;
        plist.replace(eraseStart, valueEnd - eraseStart, value);
    }

    // The end of the plist value element starting at `start` (nested <array>/<dict> counted),
    // npos when it is not one
    size_t plistValueEnd(const std::string& plist, size_t start) {
        if (start >= plist.size() || plist[start] != '<') return std::string::npos;
        const size_t tagEnd = plist.find('>', start);
        if (tagEnd == std::string::npos) return std::string::npos;
        if (plist[tagEnd - 1] == '/') return tagEnd + 1;   // <true/>, <array/>
        std::string name = plist.substr(start + 1, tagEnd - start - 1);
        name = name.substr(0, name.find_first_of(" \t"));
        if (name != "array" && name != "dict") {
            const size_t close = plist.find("</" + name + ">", tagEnd);
            return close == std::string::npos ? std::string::npos : close + name.size() + 3;
        }
        int depth = 0;
        size_t pos = start;
        while (pos < plist.size()) {
            const size_t open = plist.find("<" + name, pos);
            const size_t close = plist.find("</" + name + ">", pos);
            if (close == std::string::npos) return std::string::npos;
            if (open != std::string::npos && open < close) {
                const size_t openEnd = plist.find('>', open);
                // an empty <array/> opens nothing
                if (openEnd != std::string::npos && plist[openEnd - 1] != '/') depth++;
                pos = open + name.size() + 1;
            } else {
                depth--;
                pos = close + name.size() + 3;
                if (depth == 0) return pos;
            }
        }
        return std::string::npos;
    }

    // Sets the top-level key to the plist value element `valueXml` (any type: the old value
    // goes, whatever it was)
    // Removes the TOP-LEVEL key (a line "\t<key>K</key>": nested dictionaries indent deeper) and
    // its value, whatever its type
    void removePlistKey(std::string& plist, const std::string& key) {
        const std::string keyLine = "\n\t<key>" + escapeXmlAttribute(key) + "</key>";
        const size_t at = plist.find(keyLine);
        if (at == std::string::npos) return;
        const size_t keyPos = at + 1;
        const size_t valueStart = plist.find_first_not_of(" \t\r\n", keyPos + keyLine.size() - 1);
        const size_t valueEnd = plistValueEnd(plist, valueStart);
        if (valueEnd == std::string::npos) return;
        size_t eraseEnd = valueEnd;
        if (eraseEnd < plist.size() && plist[eraseEnd] == '\n') eraseEnd++;
        plist.erase(keyPos, eraseEnd - keyPos);
    }

    // Sets the top-level key to the plist value element `valueXml` (any type: the old value
    // goes, whatever it was)
    void setPlistValue(std::string& plist, const std::string& key, const std::string& valueXml) {
        removePlistKey(plist, key);
        const size_t dictEnd = plist.rfind("</dict>");
        if (dictEnd == std::string::npos) return;
        plist.insert(dictEnd, "\t<key>" + escapeXmlAttribute(key) + "</key>\n\t" + valueXml + "\n");
    }

    // The top-level <key> names of a raw "<key>..</key><value>" fragment
    std::vector<std::string> plistFragmentKeys(const std::string& xml) {
        std::vector<std::string> keys;
        const std::string wrapped = "<dict>" + xml + "</dict>";
        tinyxml2::XMLDocument doc;
        if (doc.Parse(wrapped.c_str(), wrapped.size()) != tinyxml2::XML_SUCCESS) return keys;
        for (const tinyxml2::XMLElement* e = doc.FirstChildElement("dict")->FirstChildElement("key"); e; e = e->NextSiblingElement("key")) {
            if (e->GetText()) keys.push_back(e->GetText());
        }
        return keys;
    }

    std::string plistValueXml(const editor::NativePlistValue& value) {
        using Kind = editor::NativePlistValue::Kind;
        switch (value.kind) {
            case Kind::Bool: return value.flag ? "<true/>" : "<false/>";
            case Kind::Integer: return "<integer>" + std::to_string(value.integer) + "</integer>";
            case Kind::Real: {
                char text[64];
                snprintf(text, sizeof(text), "%.17g", value.real);
                return std::string("<real>") + text + "</real>";
            }
            case Kind::StringArray: {
                if (value.items.empty()) return "<array/>";
                std::string xml = "<array>\n";
                for (const std::string& item : value.items) xml += "\t\t<string>" + escapeXmlAttribute(item) + "</string>\n";
                return xml + "\t</array>";
            }
            case Kind::String: break;
        }
        return "<string>" + escapeXmlAttribute(value.text) + "</string>";
    }

    // Project Settings > Native Plugins: Info.plist entries (replacing keys), then raw XML pairs
    void writeNativePlistEntries(std::string& plist, const std::map<std::string, editor::NativePlistValue>& entries, const std::string& rawXml) {
        for (const auto& [key, value] : entries) setPlistValue(plist, key, plistValueXml(value));
        if (rawXml.find_first_not_of(" \t\r\n") == std::string::npos) return;
        // the fragment's keys replace the export's (one dictionary of each key)
        for (const std::string& key : plistFragmentKeys(rawXml)) removePlistKey(plist, key);
        const size_t dictEnd = plist.rfind("</dict>");
        if (dictEnd == std::string::npos) return;
        std::string text = rawXml;
        text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
        if (text.back() != '\n') text += '\n';
        plist.insert(dictEnd, text);
    }

    // App Transport Security from Project Settings > Player > Allow Insecure HTTP. Apple ignores
    // NSAllowsArbitraryLoads when NSAllowsLocalNetworking is there too, so only one is written:
    // arbitrary loads (any http://, localhost included) or local networking alone.
    void writeAppTransportSecurity(std::string& plist, bool allowInsecureHttp) {
        replacePlistDictValue(plist, "NSAppTransportSecurity", allowInsecureHttp
            ? "\t\t<key>NSAllowsArbitraryLoads</key>\n\t\t<true/>\n"
            : "\t\t<key>NSAllowsLocalNetworking</key>\n\t\t<true/>\n");
    }

    std::array<int, 4> parseWindowsVersion(const std::string& value) {
        std::array<int, 4> parts{1, 0, 0, 0};
        size_t start = 0;
        for (int i = 0; i < 4 && start <= value.size(); i++) {
            size_t dot = value.find('.', start);
            std::string token = value.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
            try {
                parts[i] = std::clamp(std::stoi(token), 0, 65535);
            } catch (...) {
                parts[i] = 0;
            }
            if (dot == std::string::npos) break;
            start = dot + 1;
        }
        return parts;
    }

    std::string escapeWindowsRcString(const std::string& value) {
        std::string out;
        out.reserve(value.size());
        for (char c : value) {
            if (c == '\\' || c == '"') out += '\\';
            if (c == '\n' || c == '\r') out += ' ';
            else out += c;
        }
        return out;
    }

    // UTF-8 with '/' separators on every platform
    std::string genericUtf8Path(const fs::path& path) {
        const auto utf8 = path.generic_u8string();
        return std::string(utf8.begin(), utf8.end());
    }

    // 24 hex digits like Xcode's ids. FNV-1a, since std::hash differs between standard libraries.
    std::string xcodeObjectId(const std::string& key) {
        uint64_t hash = 1469598103934665603ull;
        uint32_t extra = 2166136261u;
        for (unsigned char c : key) {
            hash ^= c;
            hash *= 1099511628211ull;
            extra ^= c;
            extra *= 16777619u;
        }
        char id[25];
        snprintf(id, sizeof(id), "%016llX%08X", static_cast<unsigned long long>(hash), static_cast<unsigned int>(extra));
        return id;
    }

    // Quoted unless Xcode would write it bare
    std::string pbxString(const std::string& value) {
        const bool bare = !value.empty() && std::all_of(value.begin(), value.end(), [](unsigned char c) {
            return c < 0x80 && (std::isalnum(c) || c == '_' || c == '$' || c == '.' || c == '/');
        });
        if (bare) return value;
        std::string out = "\"";
        for (char c : value) {
            if (c == '"' || c == '\\') out += '\\';
            out += c;
        }
        return out + "\"";
    }

    struct PbxRange {
        size_t start = std::string::npos;
        size_t end = std::string::npos;
        bool found() const { return start != std::string::npos; }
    };

    // An object starts on a line indented by two tabs and ends with "\t\t};"
    PbxRange pbxObjectAt(const std::string& pbx, size_t start) {
        const size_t end = pbx.find("\n\t\t};\n", start);
        return end == std::string::npos ? PbxRange{} : PbxRange{start, end + 6};
    }

    PbxRange findPbxObject(const std::string& pbx, const std::string& id) {
        if (id.empty()) return {};
        const size_t start = pbx.find("\n\t\t" + id + " ");
        return start == std::string::npos ? PbxRange{} : pbxObjectAt(pbx, start + 1);
    }

    PbxRange findPbxObjectContaining(const std::string& pbx, const std::string& section, const std::string& text) {
        const size_t sectionStart = pbx.find("/* Begin " + section + " section */");
        if (sectionStart == std::string::npos) return {};
        const size_t match = pbx.find(text, sectionStart);
        if (match >= pbx.find("/* End " + section + " section */", sectionStart)) return {};
        size_t start = pbx.rfind('\n', match);
        while (start != std::string::npos && start > 0 && pbx.compare(start + 1, 3, "\t\t\t") == 0) {
            start = pbx.rfind('\n', start - 1);
        }
        return start == std::string::npos ? PbxRange{} : pbxObjectAt(pbx, start + 1);
    }

    // The id in "key = <id> /* name */;" inside range
    std::string pbxValueId(const std::string& pbx, PbxRange range, const std::string& key) {
        if (!range.found()) return {};
        const size_t start = pbx.find("\t" + key + " = ", range.start);
        if (start >= range.end) return {};
        const size_t value = start + key.size() + 4;
        return pbx.substr(value, pbx.find_first_of(" ;", value) - value);
    }

    // The entry lines of the list "key = (" inside range; indent is the key's
    PbxRange findPbxList(const std::string& pbx, PbxRange range, const std::string& key, std::string& indent) {
        if (!range.found()) return {};
        const size_t keyStart = pbx.find("\t" + key + " = (\n", range.start);
        if (keyStart >= range.end) return {};
        const size_t lineStart = pbx.rfind('\n', keyStart) + 1;
        indent = pbx.substr(lineStart, keyStart + 1 - lineStart);
        const size_t start = pbx.find('\n', keyStart) + 1;
        const size_t end = pbx.find("\n" + indent + ");", start - 1);
        return end >= range.end ? PbxRange{} : PbxRange{start, end + 1};
    }

    std::vector<std::string> pbxListIds(const std::string& pbx, PbxRange range, const std::string& key) {
        std::string indent;
        const PbxRange list = findPbxList(pbx, range, key, indent);
        std::vector<std::string> ids;
        if (!list.found()) return ids;
        for (size_t line = list.start; line < list.end; line = pbx.find('\n', line) + 1) {
            const size_t id = line + indent.size() + 1;
            ids.push_back(pbx.substr(id, pbx.find_first_of(" ,", id) - id));
        }
        return ids;
    }

    // Adds the entries the list lacks; false when range has no list "key = ("
    bool addPbxListEntries(std::string& pbx, PbxRange range, const std::string& key, const std::vector<std::string>& entries) {
        std::string indent;
        const PbxRange list = findPbxList(pbx, range, key, indent);
        if (!list.found()) return false;
        std::set<std::string> lines;
        for (size_t line = list.start; line < list.end; line = pbx.find('\n', line) + 1) {
            lines.insert(pbx.substr(line, pbx.find('\n', line) + 1 - line));
        }
        std::string added;
        for (const std::string& entry : entries) {
            const std::string line = indent + "\t" + entry + ",\n";
            if (lines.insert(line).second) added += line;
        }
        pbx.insert(list.end, added);
        return true;
    }

    PbxRange findPbxBuildSettings(const std::string& pbx, const std::string& configurationId) {
        const PbxRange configuration = findPbxObject(pbx, configurationId);
        if (!configuration.found()) return {};
        const size_t start = pbx.find("\t\t\tbuildSettings = {\n", configuration.start);
        if (start >= configuration.end) return {};
        const size_t end = pbx.find("\n\t\t\t};\n", start);
        return end >= configuration.end ? PbxRange{} : PbxRange{start, end + 1};
    }

    // By name, as ids drift whenever the workspace is edited in Xcode
    bool findXcodeTarget(const std::string& pbx, const std::string& name, std::string& sourcesPhaseId,
                         std::vector<std::string>& configurationIds) {
        const PbxRange target = findPbxObjectContaining(pbx, "PBXNativeTarget", "\n\t\t\tname = " + pbxString(name) + ";\n");
        if (!target.found()) return false;
        for (const std::string& phaseId : pbxListIds(pbx, target, "buildPhases")) {
            const PbxRange phase = findPbxObject(pbx, phaseId);
            if (phase.found() && pbx.find("\t\t\tisa = PBXSourcesBuildPhase;\n", phase.start) < phase.end) sourcesPhaseId = phaseId;
        }
        const PbxRange configurationList = findPbxObject(pbx, pbxValueId(pbx, target, "buildConfigurationList"));
        configurationIds = pbxListIds(pbx, configurationList, "buildConfigurations");
        return !sourcesPhaseId.empty() && !configurationIds.empty();
    }

}

editor::Exporter::Exporter() {
}

void editor::Exporter::waitForExport() {
    // a process-channel job (generate_shaders) must not wait on its own channel: poll
    if (jobs::isInChannelJob()) {
        while (exportTask.isValid() && !exportTask.isReady())
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        return;
    }
    exportTask.wait();
}

editor::Exporter::~Exporter() {
    cancelRequested.store(true);
    commandRunner.cancel();
    waitForExport();
}

void editor::Exporter::setProgress(const std::string& step, float value) {
    std::lock_guard<std::mutex> lock(progressMutex);
    progress.currentStep = step;
    progress.overallProgress = value * progressScale;
    progress.detailLine.clear();
}

void editor::Exporter::setProgressRaw(const std::string& step, float value) {
    std::lock_guard<std::mutex> lock(progressMutex);
    progress.currentStep = step;
    progress.overallProgress = value;
    progress.detailLine.clear();
}

void editor::Exporter::setDetail(const std::string& line) {
    std::lock_guard<std::mutex> lock(progressMutex);
    progress.detailLine = line;
}

void editor::Exporter::setError(const std::string& message, bool reported) {
    std::lock_guard<std::mutex> lock(progressMutex);
    progress.failed = true;
    progress.errorMessage = message;
    // a failure the compiler's own error lines already reported is not one more error
    if (reported) Out::info("Build failed: %s", message.c_str());
    else Out::error("Build failed: %s", message.c_str());
}

bool editor::Exporter::copyOrFail(const fs::path& from, const fs::path& to) {
    std::string error;
    if (exportfiles::copyFile(from, to, error)) return true;
    setError(error);
    return false;
}

bool editor::Exporter::writeOrFail(const fs::path& path, const std::string& content) {
    std::string error;
    if (exportfiles::writeFile(path, content, error)) return true;
    setError(error);
    return false;
}

bool editor::Exporter::walkFiles(const fs::path& root, const exportfiles::DirFilter& filter, const exportfiles::Visitor& visit) {
    bool visitorFailed = false;
    const exportfiles::WalkResult walked = exportfiles::walk(root,
        [&](const fs::directory_entry& entry, const fs::path& rel) {
            if (isEarlierExport(entry)) return exportfiles::DirAction::Skip;
            return filter ? filter(entry, rel) : exportfiles::DirAction::Descend;
        },
        [&](const fs::directory_entry& entry, const fs::path& rel) {
            if (visit(entry, rel)) return true;
            visitorFailed = true;
            return false;
        });
    for (const fs::path& folder : walked.unreadable) {
        warn("W-UNREADABLE-FOLDER", FileUtils::pathToGenericUtf8(folder) + " could not be read and was left out of the build");
    }
    if (!walked.ok && !visitorFailed) setError(walked.error);
    return walked.ok;
}

bool editor::Exporter::isEarlierExport(const fs::directory_entry& entry) {
    std::error_code ec;
    const fs::path& dir = entry.path();
    std::string marker;
    for (const fs::path& own : {config.targetDir, config.destinationDir}) {
        if (!own.empty() && fs::equivalent(dir, own, ec)) {
            marker = "this build's output folder";
            break;
        }
    }
    if (marker.empty()) marker = exportfiles::exportFolderMarker(dir, getAppName());
    if (marker.empty()) return false;
    const fs::path where = project ? dir.lexically_proximate(project->getProjectPath()) : dir;
    if (warnedExportFolders.insert(dir.lexically_normal()).second) {
        warn("W-EXPORT-FOLDER-SKIPPED", FileUtils::pathToGenericUtf8(where) + " looks like an earlier export (" + marker +
             ") and was left out; move it out of the project");
    }
    return true;
}

bool editor::Exporter::writeWebFavicon(const fs::path& target) {
    const WebProjectSettings& web = project->getWebProjectSettings();
    if (!web.favicon.empty()) {
        const fs::path favicon = web.favicon.is_absolute() ? web.favicon : project->getProjectPath() / web.favicon;
        std::string error;
        if (exportfiles::copyFile(favicon, target, error)) return true;
        setError("Cannot copy the web favicon: " + error);
        return false;
    }
    std::error_code ec;
    fs::path icon = project->getWindowIcon();
    if (!icon.empty() && icon.is_relative()) icon = project->getProjectPath() / icon;
    std::string extension = FileUtils::pathToUtf8(icon.extension());
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (!icon.empty() && extension == ".png" && fs::is_regular_file(icon, ec)) return copyOrFail(icon, target);
    // a page without one makes the browser ask for /favicon.ico (a 404 in the console), as Unity's
    // template ships its own
    return writeOrFail(target, std::string(reinterpret_cast<const char*>(jasperengine_logo_png), jasperengine_logo_png_len));
}

bool editor::Exporter::removeStale(const fs::path& path) {
    std::error_code ec;
    fs::remove(path, ec);
    if (!ec) return true;
    setError("Failed to remove " + FileUtils::pathToGenericUtf8(path) + ": " + ec.message());
    return false;
}

void editor::Exporter::resetResult() {
    std::lock_guard<std::mutex> lock(progressMutex);
    result = ExportResult();
    result.outputDir = config.mode == ExportMode::SourceCode ? config.targetDir : config.destinationDir;
    warnedExportFolders.clear();
}

void editor::Exporter::failBuild(const std::string& what) {
    const std::vector<BuildDiagnostic>& errors = buildErrors.errors();
    if (!errors.empty()) {
        // at the end of the log, where a long build's output leaves the reader; not errors again
        // (the compiler's lines above count once each in the Console)
        Out::info("Build errors (%zu):", buildErrors.errorCount());
        for (const BuildDiagnostic& error : errors) Out::info("%s", error.text.c_str());
        if (buildErrors.errorCount() > errors.size()) Out::info("(and %zu more)", buildErrors.errorCount() - errors.size());
    }
    {
        std::lock_guard<std::mutex> lock(progressMutex);
        progress.errors = errors;
        progress.errorCount = buildErrors.errorCount();
    }
    setError(what + buildErrors.failureDetail(), !errors.empty());
}

std::string editor::Exporter::outputFolderRefusal(const Project* project, const fs::path& dir) {
    if (dir.empty()) return std::string();
    // CMake reads ';' as a list separator: the exported build would take the path apart
    if (FileUtils::pathToUtf8(dir).find(';') != std::string::npos) {
        return "The output folder " + FileUtils::pathToGenericUtf8(dir) + " has a ';' in its path, which CMake reads as a list "
               "separator: choose a folder whose path has none, or rename the folder.";
    }
    if (!project) return std::string();
    std::error_code ec;
    const fs::path where = fs::weakly_canonical(fs::absolute(dir, ec), ec).lexically_normal();
    const fs::path projectRoot = fs::weakly_canonical(project->getProjectPath(), ec).lexically_normal();
    auto inside = [&where](const fs::path& root) -> std::optional<fs::path> {
        if (root.empty()) return std::nullopt;
        const fs::path rel = where.lexically_relative(root);
        if (rel.empty() || *rel.begin() == "..") return std::nullopt;
        return rel;
    };
    // below a folder the project's scans leave out: a hidden one (.jasperengine/builds, MCP's
    // default) or the top-level build/
    auto skippedByScans = [](const fs::path& rel) {
        bool firstComponent = true;
        for (const fs::path& part : rel) {
            const std::string name = FileUtils::pathToUtf8(part);
            if (!name.empty() && name[0] == '.' && name != "." && name != "..") return true;
            if (firstComponent && name == "build") return true;
            firstComponent = false;
        }
        return false;
    };
    bool refused = false;
    if (std::optional<fs::path> rel = inside(projectRoot)) {
        refused = !skippedByScans(*rel);
    }
    for (const fs::path& other : {project->getAssetsPath(), project->getLuaPath()}) {
        if (refused || other.empty()) continue;
        const fs::path root = fs::weakly_canonical(other.is_absolute() ? other : project->getProjectPath() / other, ec).lexically_normal();
        if (inside(projectRoot) && inside(root)) continue;   // judged by the project rule above
        if (std::optional<fs::path> rel = inside(root)) refused = !skippedByScans(*rel);
    }
    if (!refused) return std::string();
    const std::string app = project->getName().empty() ? "jasperengine-project" : Factory::toIdentifier(project->getName());
    return "The output folder " + FileUtils::pathToGenericUtf8(dir) + " is inside the project: the editor would import the "
           "exported files as project assets. Choose a folder outside it, e.g. " +
           FileUtils::pathToGenericUtf8(project->getProjectPath().parent_path() / (app + "-build")) + ".";
}

editor::ExportProgress editor::Exporter::getProgress() const {
    std::lock_guard<std::mutex> lock(progressMutex);
    return progress;
}

bool editor::Exporter::isRunning() const {
    std::lock_guard<std::mutex> lock(progressMutex);
    return progress.started && !progress.finished && !progress.failed;
}

void editor::Exporter::cancelExport() {
    cancelRequested.store(true);
    commandRunner.cancel();
}

bool editor::Exporter::isCancelled() const {
    return cancelRequested.load();
}

void editor::Exporter::startExport(Project* proj, const ExportConfig& cfg) {
    waitForExport();

    this->project = proj;
    this->config = cfg;
    cancelRequested.store(false);
    this->fixedAppName.clear();
    this->fixedAppName = getAppName();
    {
        std::lock_guard<std::mutex> lock(progressMutex);
        this->progress = ExportProgress();
        this->progress.started = true;
        this->producedExecutable.clear();
    }
    resetResult();

    // the export reads the open scenes and prefabs while the frames go on: the holes of
    // destroyed entities are closed before it starts (ComponentArray; an index read on the
    // export's thread would close them under the main thread)
    if (proj) {
        proj->compactAllComponents();
    }

    // Run the export on the process channel so the UI does not block
    jobs::SubmitOptions options;
    options.name = "export";
    exportTask = jobs::system().processChannel().submit([this]() { runExport(); }, options);
}

bool editor::Exporter::exportProject(Project* proj, const ExportConfig& cfg) {
    waitForExport();

    this->project = proj;
    this->config = cfg;
    cancelRequested.store(false);
    this->fixedAppName.clear();
    this->fixedAppName = getAppName();
    {
        std::lock_guard<std::mutex> lock(progressMutex);
        this->progress = ExportProgress();
        this->progress.started = true;
        this->producedExecutable.clear();
    }
    resetResult();

    runExport();

    std::lock_guard<std::mutex> lock(progressMutex);
    return progress.finished && !progress.failed;
}

bool editor::Exporter::generateShaders(const ExportConfig& cfg) {
    waitForExport();

    this->project = nullptr;
    this->config = cfg;
    cancelRequested.store(false);
    {
        std::lock_guard<std::mutex> lock(progressMutex);
        this->progress = ExportProgress();
        this->progress.started = true;
    }
    resetResult();

    runExport();

    std::lock_guard<std::mutex> lock(progressMutex);
    return progress.finished && !progress.failed;
}

void editor::Exporter::runExport() {
    // The editor's regeneration of .jasperengine/generated (a change picked up meanwhile, a
    // setting) waits for the whole export, which clears, rewrites and copies that folder
    struct GeneratedExportGuard {
        Project* project;
        explicit GeneratedExportGuard(Project* p) : project(p) { if (project) project->beginGeneratedExport(); }
        ~GeneratedExportGuard() { if (project) project->endGeneratedExport(); }
    } generatedGuard(project);

    if (project && !checkRootsPinned()) return;

    // never into the project: the editor would import the export's files as project assets
    if (project) {
        const std::string refusal = outputFolderRefusal(project, config.mode == ExportMode::SourceCode ? config.targetDir : config.destinationDir);
        if (!refusal.empty()) {
            setError(refusal);
            return;
        }
    }

    // a player for a target: prebuilt, or this export's Desktop/Web build (S30 D1)
    if (project && config.mode == ExportMode::Player) {
        if (config.method == BuildMethod::Auto) {
            BuildMethod saved = BuildMethod::Auto;
            if (parseBuildMethod(project->getBuildMethod(), saved)) config.method = saved;
        }
        config.packNativeResources = config.packNativeResources || project->isPackContent();
        runPlayerBuild();
        return;
    }
    const bool shaderGenerationOnly = (project == nullptr);
    if (project && config.mode == ExportMode::Web)
        config.webMultithreaded = config.webMultithreaded || project->getThreadingSettings().webMultithreaded;
    // Build Settings > Development Build, whichever front end exports
    if (project)
        config.developmentBuild = config.developmentBuild || project->isDevelopmentBuild();
    const bool buildMode = !shaderGenerationOnly && config.mode != ExportMode::SourceCode;

    // The scene sources describe the FBX imports that ship, so none may change mid-export
    struct FbxImportPause {
        Project* project;
        explicit FbxImportPause(Project* project): project(project) {
            if (project) project->setFbxImportsPaused(true);
        }
        ~FbxImportPause() {
            if (project) project->setFbxImportsPaused(false);
        }
    } fbxImportPause(project);

    // Generation steps report progress in the [0,1] SourceCode range; for build
    // modes they occupy the first half, leaving the rest for configure/build/collect.
    progressScale = buildMode ? 0.5f : 1.0f;

    // the shared compile tree (prepareCacheTarget) is another build's once this one ends
    struct SharedTreeRelease {
        std::unique_ptr<io::ScopedFileLock>& lock;
        ~SharedTreeRelease() { lock.reset(); }
    } sharedTreeRelease{sharedBuildLock};

    if (buildMode && config.mode == ExportMode::Web) {
        // emcc.bat runs through cmd.exe on Windows: no '%' in the folders it is given
        std::string refusal = webBuildRefusal({project->getProjectPath(), getBuildCacheDir(), config.destinationDir});
        if (refusal.empty()) refusal = webTreeRefusal(webSourceRoots(project));   // the files it copies / compiles
        if (!refusal.empty()) {
            setError(refusal + ".");
            return;
        }
    }
    if (buildMode) {
        if (!prepareCacheTarget()) return;
    } else {
        if (!checkTargetDir()) return;
    }
    if (isCancelled()) { setError("Build cancelled"); return; }

    if (shaderGenerationOnly) {
        if (!buildAndSaveShaders()) return;
        if (isCancelled()) { setError("Build cancelled"); return; }

        setProgress("Shader generation complete", 1.0f);
        {
            std::lock_guard<std::mutex> lock(progressMutex);
            progress.finished = true;
        }
        Out::info("Shaders generated successfully at: %s", config.targetDir.string().c_str());
        return;
    }

    if (!clearGenerated()) return;
    holdForTest();
    if (isCancelled()) { setError("Build cancelled"); return; }
    if (!loadAndSaveAllScenes()) return;
    if (!checkMissingAssets()) return;
    resolveShaderKeys();
    if (isCancelled()) { setError("Build cancelled"); return; }
    if (!copyGenerated()) return;
    if (isCancelled()) { setError("Build cancelled"); return; }
    // Lua before assets, which skips the sources the lua tree took
    if (!copyLua()) return;
    if (isCancelled()) { setError("Build cancelled"); return; }
    if (!checkHtmlUIReferences()) return;
    checkSoundEventClips();
    if (isCancelled()) { setError("Build cancelled"); return; }
    if (!cookTextures()) return;
    if (isCancelled()) { setError("Build cancelled"); return; }
    if (!cookModels()) return;
    if (isCancelled()) { setError("Build cancelled"); return; }
    if (!copyAssets()) return;
    if (!writeAudioImports()) return;
    buildReport = BuildReport();
    measureBuildContent(getExportProjectRoot());
    if (!writeAssetMap()) return;
    if (isCancelled()) { setError("Build cancelled"); return; }
    if (!copyCustomShaderSources()) return;
    if (isCancelled()) { setError("Build cancelled"); return; }
    if (!copyCppScripts()) return;
    if (isCancelled()) { setError("Build cancelled"); return; }
    if (!copyNativePlugins()) return;
    // a macOS build without Xcode compiles no Swift: its Swift plugins answer "unavailable"
    if (config.mode == ExportMode::Desktop && hostBuildPlatform() == BuildPlatform::MacOS) {
        const NativePlugins::Scan scan = NativePlugins::scan(project->getProjectPath());
        std::vector<std::string> swift = scan.filesWith("apple", {".swift"});
        for (const std::string& file : scan.filesWith("macos", {".swift"})) swift.push_back(file);
        if (!swift.empty()) {
            warn("W-NATIVE-SWIFT-CMAKE", "Swift plugins (" + swift.front() + (swift.size() > 1 ? ", +" + std::to_string(swift.size() - 1) + " more" : "") +
                                         ") build only in the Xcode project (File > Build Settings > Export source, then Xcode): "
                                         "this build answers them with \"unavailable\"");
        }
    }
    if (isCancelled()) { setError("Build cancelled"); return; }
    if (!copyEngine()) return;
    if (isCancelled()) { setError("Build cancelled"); return; }
    if (!buildAndSaveShaders()) return;
    if (!writeEffectLicense()) return;
    if (config.mode == ExportMode::SourceCode) {
        if (isCancelled()) { setError("Build cancelled"); return; }
        if (!collectSourceResourcePack()) return;
    }

    if (buildMode) {
        if (isCancelled()) { setError("Build cancelled"); return; }
        if (!configureBuild()) return;
        if (isCancelled()) { setError("Build cancelled"); return; }
        if (!runBuild()) return;
        if (isCancelled()) { setError("Build cancelled"); return; }
        bool collected = (config.mode == ExportMode::Desktop)
            ? collectDesktopArtifacts()
            : collectWebArtifacts();
        if (!collected) return;
        if (isCancelled()) { setError("Build cancelled"); return; }

        if (config.mode == ExportMode::Web && config.webMultithreaded) {
            // the same project again, with pthreads, in a build tree of its own
            setProgressRaw("Building the multithreaded web player...", BUILD_PROGRESS_START);
            buildDirName = "build-threads";
            extraCMakeArgs = "-DEMSCRIPTEN_THREAD_SUPPORT=ON ";
            const bool threadsBuilt = configureBuild() && !isCancelled() && runBuild() && !isCancelled()
                && collectWebThreadsArtifacts();
            buildDirName = "build";
            extraCMakeArgs.clear();
            if (!threadsBuilt) {
                if (isCancelled()) setError("Build cancelled");
                return;
            }
        }

        measureBuildEngine(config.destinationDir);
        if (!writeBuildReport(config.destinationDir, "compile")) return;
        setProgressRaw("Build complete", 1.0f);
        {
            std::lock_guard<std::mutex> lock(progressMutex);
            if (!holdFinish) progress.finished = true;
        }
        Out::info("Build completed: %s", config.destinationDir.string().c_str());
        return;
    }

    if (isCancelled()) { setError("Build cancelled"); return; }
    if (!writeBuildReport(config.targetDir, "source")) return;

    setProgress("Build complete", 1.0f);
    {
        std::lock_guard<std::mutex> lock(progressMutex);
        if (!holdFinish) progress.finished = true;
    }
    Out::info("Build completed: %s", config.targetDir.string().c_str());
}

// Runs after loadAndSaveAllScenes(): saving a scene refreshes its cached keys, so
// scenes that were closed or outdated have contributed before the overrides apply.
void editor::Exporter::resolveShaderKeys() {
    if (!project) {
        return;
    }

    std::set<ShaderKey> keys;
    // forks whose files are gone: their saved, added or recorded variants can't be built
    std::set<std::string> deadForks;
    if (!config.shaderKeysOverride.empty()) {
        // an explicit --shader list is the user's word: a fork that can't build fails it
        for (ShaderKey key : config.shaderKeysOverride) {
            keys.insert(ShaderPool::normalizeKey(key));
        }
    } else {
        // the list every consumer shares (Project::collectBuildShaderKeys): each scene's saved
        // keys and, where it is loaded, what its objects were loaded with and what its content needs
        // now (a save writes no loaded state: Project::collectSavedShaderKeys)
        project->collectBuildShaderKeys(keys, &deadForks);
    }
    keys = project->finishBuildShaderKeys(keys, config.shaderAdditions, config.shaderExclusions, &deadForks);
    for (const std::string& fork : deadForks) {
        warn("W-SHADER-FORK-MISSING", "the variants of " + (fork.empty() ? std::string("an unknown shader fork") : fork) +
             " were left out: its files are gone");
    }

    config.selectedShaderKeys = std::move(keys);
}

// the project's warm-up list into ShaderWarmup (false: none, or unreadable, which is warned)
bool editor::Exporter::loadShaderWarmup() {
    ShaderWarmup::clear();
    const fs::path path = project->getShaderWarmupPath();
    if (!fs::exists(path)) {
        return false;
    }
    std::string error;
    if (!ShaderWarmup::load(path.string(), &error)) {
        Out::warning("Shader warm-up list not exported: %s", error.c_str());
        return false;
    }
    return true;
}

fs::path editor::Exporter::getExportProjectRoot() const {
    return config.targetDir / "project";
}

fs::path editor::Exporter::getAssetsSourceDir() const {
    fs::path assetsSrc = config.assetsDir;
    if (assetsSrc.empty()) {
        assetsSrc = project->getProjectPath();
    }
    if (assetsSrc.is_relative()) {
        assetsSrc = project->getProjectPath() / assetsSrc;
    }

    std::error_code ec;
    return fs::weakly_canonical(assetsSrc, ec);
}

std::string editor::Exporter::getAppName() const {
    if (!fixedAppName.empty()) return fixedAppName;   // fixed at the start of the export
    if (!project || project->getName().empty()) {
        return "jasperengine-project";
    }
    return Factory::toIdentifier(project->getName());
}

fs::path editor::Exporter::getProducedExecutable() const {
    std::lock_guard<std::mutex> lock(progressMutex);
    return producedExecutable;
}

std::string editor::Exporter::webPathRefusal(const std::vector<fs::path>& paths) {
    for (const fs::path& path : paths) {
        if (path.empty()) continue;
        const std::string text = path.string();
        if (text.find_first_of("%\"") != std::string::npos) {
            return "Web builds on Windows can't use a folder whose path contains % (or \"): rename the folder (" + text + ")";
        }
    }
    return std::string();
}

std::string editor::Exporter::webBuildRefusal(const std::vector<fs::path>& paths) {
#ifdef _WIN32
    return webPathRefusal(paths);
#else
    (void)paths;
    return std::string();
#endif
}

fs::path editor::Exporter::buildCacheDirFor(Project* project, ExportMode mode) {
    return project->getProjectInternalPath() / "export" / (mode == ExportMode::Desktop ? "desktop" : "web");
}

fs::path editor::Exporter::getBuildCacheDir() const {
    return project->getProjectInternalPath() / "export" / (config.mode == ExportMode::Desktop ? "desktop" : "web");
}

std::vector<std::string> editor::Exporter::compiledModules() const {
    // what Project Settings allow (the HTML UI, its video and the sound layer have no switch of their own)
    std::vector<std::string> modules = {"htmlui", "video", "soundlayer"};
    if (project->isRuntimeShadersEnabled()) modules.push_back("runtimeshaders");
    if (project->isPhysics3DEnabled()) modules.push_back("physics3d");
    if (project->isPhysics2DEnabled()) modules.push_back("physics2d");
    if (project->getNavigationSettings().includeInBuilds) modules.push_back("navigation");
    // of them, those the project reaches (a source export: all of them)
    if (moduleUsageKnown)
        modules.erase(std::remove_if(modules.begin(), modules.end(), [this](const std::string& m) { return !moduleUsage.needs(m); }), modules.end());
    std::vector<std::string> ordered;
    for (const std::string& module : player::allModules())
        if (std::find(modules.begin(), modules.end(), module) != modules.end()) ordered.push_back(module);
    return ordered;
}

fs::path editor::Exporter::getSharedBuildCacheDir() const {
    // What decides how the engine and the vendored libraries compile: the kit (configureBuild's,
    // without the tree's path) and the project settings that are definitions of the whole build
    // (the C++ standard, the compiled-in features, the crash handler and the watchdog).
    // Everything else of a project (its title, window, logging,
    // scenes, scripts, shaders) only recompiles its own units in a tree it shares. Another
    // editor's engine sources are synced into the tree like an update's (copyEngine), and Ninja
    // recompiles what changed.
    std::string key = std::string(config.mode == ExportMode::Desktop ? "desktop" : "web")
        + "\n" + getEffectiveGenerator() + "\n" + config.cmakeCCompiler + "\n" + config.cmakeCxxCompiler
        + "\n" + config.emsdkPath + "\n" + config.buildType + "\n" + config.graphicBackend
        + "\n" + (config.developmentBuild ? "development" : "release") + "\n" + extraCMakeArgs
        + "\ncxx" + std::to_string(project->getCxxStandard())
        + "\nphysics2d " + (project->isPhysics2DEnabled() ? "1" : "0")
        + "\nphysics3d " + (project->isPhysics3DEnabled() ? "1" : "0")
        + "\nnavigation " + (project->getNavigationSettings().includeInBuilds ? "1" : "0")
        + "\nruntime shaders " + (project->isRuntimeShadersEnabled() ? "1" : "0")
        + "\ncrash handler " + (project->getPlayerCrash().enabled ? "1" : "0")
        + "\nhang watchdog " + (project->getPlayerCrash().hangWatchdog ? "1" : "0");
    if (config.mode == ExportMode::Desktop) {
        const std::string platform = Generator::getGeneratorPlatform(config.cmakeGenerator);
        if (!platform.empty()) key += "\n" + platform;
    }
    return App::getUserCacheBaseDir() / "jasperengine" / "compile-builds" / sha256Hex(key).substr(0, 16);
}

std::string editor::Exporter::getEffectiveGenerator() const {
    if (!config.cmakeGenerator.empty()) {
        return config.cmakeGenerator;
    }
    const char* envGenerator = std::getenv("CMAKE_GENERATOR");
    return envGenerator ? envGenerator : "";
}

bool editor::Exporter::shouldSkipExportSupportFile(const fs::path& relativePath) {
    return relativePath == "CMakeLists.txt" || relativePath == "ProjectBuild.cmake"
        || relativePath.filename() == "AGENTS.md";
}

bool editor::Exporter::isCppHeaderFile(const fs::path& path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    static const std::set<std::string> headerExtensions = {
        ".h", ".hpp", ".hh", ".hxx", ".h++", ".inl", ".ipp", ".tpp"
    };
    return headerExtensions.count(ext) > 0;
}

bool editor::Exporter::isCppSourceFile(const fs::path& path) {
    // C++ scripts are exported to project/scripts by copyCppScripts(); they must
    // never be duplicated into the assets or lua trees. The exported build globs
    // every .cpp under the project root recursively, so a stray source file in
    // both assets and lua would compile twice and break linking with multiple
    // definition errors. Filter by extension so unregistered/orphan scripts are
    // skipped too (not only those attached to a scene).
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    static const std::set<std::string> sourceExtensions = {
        ".cpp", ".cc", ".cxx", ".c++", ".c"
    };
    return sourceExtensions.count(ext) > 0 || isCppHeaderFile(path);
}

bool editor::Exporter::isLuaExportFile(const fs::path& path) {
    // The lua directory carries the scripts and the data files they can read
    // at runtime; assets and C++ scripts ship through their own copy steps.
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    static const std::set<std::string> luaExtensions = {
        ".lua", ".luac", ".json", ".txt", ".csv", ".tsv", ".xml", ".ini", ".cfg", ".conf", ".toml", ".dat"
    };
    return luaExtensions.count(ext) > 0;
}

bool editor::Exporter::isLuaSourceFile(const fs::path& path) {
    // Only the lua tree needs these; every platform puts it on the "lua://" lookup path
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return ext == ".lua" || ext == ".luac";
}

bool editor::Exporter::checkTargetDir() {
    setProgress("Checking target directory...", 0.0f);

    if (config.targetDir.empty()) {
        setError("Target directory not specified");
        return false;
    }

    std::error_code ec;
    if (!fs::exists(config.targetDir, ec)) {
        fs::create_directories(config.targetDir, ec);
        if (ec) {
            setError("Failed to create target directory: " + ec.message());
            return false;
        }
    }

    if (project && !config.overwriteTarget && !fs::is_empty(config.targetDir, ec)) {
        setError("Target directory is not empty");
        return false;
    }

    return true;
}

bool editor::Exporter::prepareCacheTarget() {
    setProgress("Preparing build cache...", 0.0f);

    if (config.destinationDir.empty()) {
        setError("Destination directory not specified");
        return false;
    }

    // The kit's shared compile tree when no other build holds it (a C++-script game then compiles
    // its own code, the engine's objects are already there), else the project's own as before
    // (docs/specs/S33-build-speed.md R11). A project without a ProjectBuild.cmake only: that one
    // can change how anything compiles.
    std::error_code ec;
    sharedBuildLock.reset();
    config.targetDir = getBuildCacheDir();
    if (!fs::exists(project->getProjectPath() / "ProjectBuild.cmake", ec)) {
        const fs::path shared = getSharedBuildCacheDir();
        fs::create_directories(shared.parent_path(), ec);
        auto lock = std::make_unique<io::ScopedFileLock>(shared.parent_path() / (shared.filename().string() + ".lock"),
                                                     io::ScopedFileLock::Mode::Exclusive, false);
        if (lock->isLocked()) {
            sharedBuildLock = std::move(lock);
            config.targetDir = shared;
            Out::info("Building in the shared compile tree %s", shared.string().c_str());
        } else if (lock->getStatus() == io::ScopedFileLock::Status::Busy) {
            Out::info("Another build uses the shared compile tree; building in the project's own");
        }
    }
    // the compile tree is the project's own when the shared one is busy: CMake splits a path at ';'
    if (FileUtils::pathToUtf8(config.targetDir).find(';') != std::string::npos) {
        setError("The build folder " + FileUtils::pathToGenericUtf8(config.targetDir) + " has a ';' in its path, which CMake reads as "
                 "a list separator: move or rename the project folder so its path has none.");
        return false;
    }
    ec.clear();
    fs::create_directories(config.targetDir, ec);
    if (ec) {
        setError("Failed to create build cache directory: " + ec.message());
        return false;
    }

    // Stale generated sources from renamed/deleted scenes would be picked up by
    // the exported CMake's recursive glob and break the link, so the (small)
    // project tree is wiped and fully re-copied every export. The engine tree is
    // kept and synced with exportfiles::mirrorTree() so the build stays incremental.
    removeAllNoFollow(getExportProjectRoot(), ec);
    if (ec) {
        setError("Failed to clear cached project sources: " + ec.message());
        return false;
    }

    return true;
}

bool editor::Exporter::configureBuild() {
    setProgressRaw("Configuring build...", 0.5f);

    if (isCancelled()) { setError("Build cancelled"); return false; }

    const fs::path buildDir = config.targetDir / buildDirName;

    // Only explicit VS kits are pinned. Default export may freely select a
    // different toolchain because it rebuilds the engine from source.
    const std::string platform = config.mode == ExportMode::Desktop
        ? Generator::getGeneratorPlatform(config.cmakeGenerator) : "";
    EmsdkInfo emsdk;
    std::string kitId;
    if (config.mode == ExportMode::Web) {
        emsdk = detectEmsdk(config.emsdkPath);
        if (!emsdk.found) {
            setError("Emscripten SDK not found. Set it in Preferences > External Tools, set EMSDK, or add emcc to PATH.");
            return false;
        }
        Out::info("Using Emscripten %s", emsdk.description.c_str());
        kitId = "web\n" + Generator::cmakeProgram() + "\n" + emsdk.toolchainFile + "\n" + config.buildType + "\n" + config.graphicBackend +
                "\n" + extraCMakeArgs + (config.developmentBuild ? "\ndevelopment" : "");
    } else {
        std::string effectiveGenerator = getEffectiveGenerator();
        // The Xcode generator builds a macOS .app bundle (MACOSX_BUNDLE in the
        // engine CMake), whose layout artifact collection does not handle and
        // whose assets would need embedding into the bundle. Every other
        // generator on macOS uses the sokol backend with a plain executable.
        if (effectiveGenerator == "Xcode") {
            if (config.cmakeGenerator.empty()) {
                setError("Executable Player builds do not support the Xcode generator (it produces an app bundle). Unset the CMAKE_GENERATOR environment variable to use the default toolchain.");
            } else {
                setError("Executable Player builds do not support the Xcode generator (it produces an app bundle). Pick another compiler in Preferences > External Tools to use the default toolchain.");
            }
            return false;
        }
        kitId = "desktop\n" + effectiveGenerator + "\n" + config.cmakeCCompiler + "\n" + config.cmakeCxxCompiler + "\n" + config.buildType + "\n" + config.graphicBackend +
                (config.developmentBuild ? "\ndevelopment" : "");
        if (!platform.empty()) kitId += "\n" + platform;
    }

    // CMake cannot switch generators or toolchains in place and its cache pins
    // the export path: wipe the build tree whenever the kit differs.
    kitId += "\n" + config.targetDir.lexically_normal().generic_string();

    std::error_code ec;
    const fs::path kitMarker = buildDir / ".jasperengine_export_kit";
    if (fs::exists(buildDir, ec)) {
        std::string prevKit;
        {
            std::ifstream f(kitMarker);
            if (f.is_open()) {
                prevKit.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            }
        }
        if (prevKit != kitId) {
            Out::warning("Build settings or export location changed. Cleaning export build directory...");
            removeAllNoFollow(buildDir, ec);
            if (ec) {
                setError("Failed to clean export build directory: " + ec.message());
                return false;
            }
        }
    }

    // CMake re-parses stored paths treating backslashes as escapes; it accepts
    // forward slashes on every platform (see Generator::configureCMake).
    auto toCMakePath = [](const std::string& p) {
        std::string out = p;
        std::replace(out.begin(), out.end(), '\\', '/');
        return out;
    };

    if (!config.cmakeGenerator.empty() && !Generator::isKnownGenerator(config.cmakeGenerator)) {
        setError("Unknown CMake generator \"" + config.cmakeGenerator + "\"; set it in Preferences > External Tools.");
        return false;
    }
    // launched as an argv array, never a shell (CommandRunner::run): the target path and the build
    // settings are literal arguments, so metacharacters in them can't start a command
    std::vector<std::string> argv;
    if (config.mode == ExportMode::Web) {
        // CMake run directly with what emcmake.py adds (no emcmake wrapper: emcmake.bat would run
        // through cmd.exe, which parses its arguments), in the SDK's environment (below)
        argv = { Generator::cmakeProgram() };
        std::string emError;
        const CommandRunner::Environment sdkEnv = emscriptenEnvironment(emsdk);
        for (const std::string& arg : emscriptenCMakeArgs(emsdk, !config.cmakeGenerator.empty(), sdkEnv, emError)) argv.push_back(arg);
        if (!emError.empty()) {
            setError("Web build: " + emError + ".");
            return false;
        }
    } else {
        argv = { Generator::cmakeProgram() };
        if (!config.cmakeGenerator.empty()) { argv.push_back("-G"); argv.push_back(config.cmakeGenerator); }
        if (!platform.empty()) { argv.push_back("-A"); argv.push_back(platform); }
        if (!config.cmakeCCompiler.empty()) argv.push_back("-DCMAKE_C_COMPILER=" + toCMakePath(config.cmakeCCompiler));
        if (!config.cmakeCxxCompiler.empty()) argv.push_back("-DCMAKE_CXX_COMPILER=" + toCMakePath(config.cmakeCxxCompiler));
        // the same cap as the build step's --parallel: cl's /MP under Visual Studio generators
        // (jasperengine_msvc_parallel_compile)
        argv.push_back("-DJASPERENGINE_PARALLEL_BUILD_JOBS=" + std::to_string(effectiveBuildJobs()));
    }
    argv.push_back("-DCMAKE_BUILD_TYPE=" + config.buildType);
    if (config.developmentBuild) argv.push_back("-DJASPERENGINE_DEVELOPMENT_BUILD=ON");
    if (!config.graphicBackend.empty()) argv.push_back("-DGRAPHIC_BACKEND=" + config.graphicBackend);
    // the variant's own definitions (a threaded web build), one argument each
    {
        std::istringstream extra(extraCMakeArgs);
        for (std::string arg; extra >> arg;) argv.push_back(arg);
    }
    argv.push_back(toCMakePath(config.targetDir.string()));
    argv.push_back("-B");
    argv.push_back(toCMakePath(buildDir.string()));

    // Windows + Ninja: the MSVC environment (vcvars, loaded once and cached), cmake launched with it
    CommandRunner::Environment toolEnv;
    std::string msvcError;
    if (config.mode == ExportMode::Desktop && !CommandRunner::msvcEnvironment(getEffectiveGenerator(), toolEnv, msvcError)) {
        setError(msvcError);
        return false;
    }


    // Web: the SDK's environment (PATH, EMSDK, EM_CONFIG, EMSDK_NODE, EMSDK_PYTHON) for CMake and emcc
    if (config.mode == ExportMode::Web) toolEnv = emscriptenEnvironment(emsdk);

    Out::info("Configuring export build (%zu args)...", argv.size());
    bool incompatiblePython = false;
    buildErrors.clear();
    auto onConfigure = [this, &incompatiblePython](const std::string& line) {
        if (line.find("TypeError: 'type' object is not subscriptable") != std::string::npos) {
            incompatiblePython = true;
        }
        Out::build("%s", line.c_str());
        buildErrors.add(line);
        setDetail(line);
    };
    bool ok = commandRunner.run(argv, config.targetDir, onConfigure, toolEnv.empty() ? nullptr : &toolEnv);

    if (!ok) {
        if (isCancelled()) {
            setError("Build cancelled");
        } else if (config.mode == ExportMode::Web && incompatiblePython) {
            setError("Emscripten failed with an incompatible Python interpreter. Activate the SDK environment and restart the editor from that terminal. Check EMSDK_PYTHON and use a Python version supported by your installed SDK. See Output for the traceback.");
        } else {
            failBuild("CMake configuration failed");
        }
        return false;
    }

    fs::create_directories(buildDir, ec);
    if (!writeOrFail(kitMarker, kitId)) return false;

    return true;
}

unsigned int editor::Exporter::effectiveBuildJobs() const {
    const unsigned int jobs = config.buildJobs == 0 ? Generator::getAutomaticParallelBuildJobs() : config.buildJobs;
    return std::min(jobs, Generator::getMaxParallelBuildJobs());
}

bool editor::Exporter::runBuild() {
    setProgressRaw("Compiling project...", BUILD_PROGRESS_START);

    if (isCancelled()) { setError("Build cancelled"); return false; }

    const fs::path buildDir = config.targetDir / buildDirName;

    const unsigned int jobs = effectiveBuildJobs();

    // the CMake that configured the tree (Web too: no emcmake picking its own any more)
    const std::string cmakeCmd = Generator::cmakeProgram();

    std::vector<std::string> argv = { cmakeCmd, "--build", buildDir.string(), "--config", config.buildType,
                                      "--parallel", std::to_string(jobs) };
    // The build step invokes the compiler/linker, so it needs the same MSVC environment as configure.
    CommandRunner::Environment toolEnv;
    std::string msvcError;
    if (config.mode == ExportMode::Desktop && !CommandRunner::msvcEnvironment(getEffectiveGenerator(), toolEnv, msvcError)) {
        setError(msvcError);
        return false;
    }
    // Web: emcc runs in the SDK's environment
    if (config.mode == ExportMode::Web) {
        const EmsdkInfo emsdk = detectEmsdk(config.emsdkPath);
        if (emsdk.found) toolEnv = emscriptenEnvironment(emsdk);
    }

    // Initialization only predicts the work denominator. Do not advance the
    // bar until MSBuild confirms work by emitting a source filename.
    MsBuildProgress msBuildProgress(buildDir, config.buildType);

    Out::info("Building export with %u parallel jobs...", jobs);
    buildErrors.clear();
    auto onBuild = [this, &msBuildProgress](const std::string& line) {
        Out::build("%s", line.c_str());
        buildErrors.add(line);
        float fraction = 0.0f;
        bool hasProgress = parseBuildProgress(line, fraction);
        float buildEnd = BUILD_PROGRESS_END;
        if (!hasProgress && msBuildProgress.consumeLine(line, fraction)) {
            hasProgress = true;
            buildEnd = MSBUILD_COMPILE_END;
        }
        {
            std::lock_guard<std::mutex> lock(progressMutex);
            progress.detailLine = line;
            if (hasProgress) {
                progress.overallProgress = BUILD_PROGRESS_START
                    + (buildEnd - BUILD_PROGRESS_START) * fraction;
            }
        }
    };
    bool ok = commandRunner.run(argv, buildDir, onBuild, toolEnv.empty() ? nullptr : &toolEnv);

    if (!ok) {
        if (isCancelled()) {
            setError("Build cancelled");
        } else {
            failBuild("Compiling failed");
        }
        return false;
    }

    return true;
}

bool editor::Exporter::collectDesktopArtifacts() {
    setProgressRaw("Copying artifacts...", 0.95f);

    const fs::path buildDir = config.targetDir / "build";
    std::string exeName = getAppName();
#ifdef _WIN32
    exeName += ".exe";
#endif

    std::error_code ec;
    const fs::path projectRoot = getExportProjectRoot();
    fs::path exePath = buildDir / exeName;
    if (!fs::exists(exePath, ec)) {
        // Multi-config generators (Visual Studio) place binaries in a per-config subdir.
        fs::path configExePath = buildDir / config.buildType / exeName;
        if (fs::exists(configExePath, ec)) {
            exePath = configExePath;
        } else if (fs::exists(buildDir / (getAppName() + ".app"), ec)
                   || fs::exists(buildDir / config.buildType / (getAppName() + ".app"), ec)) {
            // A stale Xcode-configured cache can slip past the generator guard.
            setError("The build produced an app bundle, which Executable Player builds do not support. Delete the build folder " + config.targetDir.string() + " and build again with the default toolchain.");
            return false;
        } else {
            setError("Built executable not found at: " + exePath.string() + " or " + configExePath.string());
            return false;
        }
    }

    fs::create_directories(config.destinationDir, ec);
    if (ec) {
        setError("Failed to create destination directory: " + ec.message());
        return false;
    }

    // a symbolic link at the player's name is replaced, never written through
    const fs::path producedPath = config.destinationDir / exeName;
    if (fs::is_symlink(fs::symlink_status(producedPath, ec))) fs::remove(producedPath, ec);
    ec.clear();
    fs::copy_file(exePath, producedPath, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        setError("Failed to copy executable: " + ec.message());
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(progressMutex);
        producedExecutable = producedPath;
    }
    if (!copyLicenses()) {
        return false;
    }

    bool packCreated = false;
    if (config.packNativeResources) {
        if (!writeNativeResourcePack(config.destinationDir / RESOURCE_PACK_FILENAME, packCreated)) {
            return false;
        }
    } else {
        if (!removeResourcePack(config.destinationDir / RESOURCE_PACK_FILENAME)) {
            return false;
        }
    }

    if (isCancelled()) {
        // A committed pack stays: nothing else in the destination holds the resources
        setError("Build cancelled");
        return false;
    }

    if (packCreated) {
        for (const char* dir : {"assets", "lua"}) {
            ec.clear();
            removeAllNoFollow(config.destinationDir / dir, ec);
            if (ec) {
                setError(std::string("Failed to remove unpacked ") + dir + ": " + ec.message());
                return false;
            }
        }
    } else {
        // The desktop runtime resolves "assets" and "lua" relative to the working
        // directory, so ship them next to the executable.
        for (const char* dir : {"assets", "lua"}) {
            fs::path src = projectRoot / dir;
            if (!fs::exists(src, ec)) continue;
            const fs::path dst = config.destinationDir / dir;
            if (!walkFiles(src, nullptr, [&](const fs::directory_entry& entry, const fs::path& rel) {
                    return copyOrFail(entry.path(), dst / rel);
                })) {
                return false;
            }
        }
    }

#if defined(__linux__)
    const std::vector<std::string> settingsArgv = { Generator::cmakeProgram(),
        "-DAPP_FILE=" + (fs::absolute(config.destinationDir) / exeName).string(), "-DWEB=OFF",
        "-P", (projectRoot / "export-settings" / "apply.cmake").string() };
    if (!commandRunner.run(settingsArgv, config.targetDir, [](const std::string& line) { Out::build("%s", line.c_str()); })) {
        setError("Failed to generate Linux launcher");
        return false;
    }
#endif

    return true;
}

// Clears a pack and the temporary an interrupted write may have left beside it.
bool editor::Exporter::removeResourcePack(const fs::path& packPath) {
    std::error_code ec;
    for (const fs::path& path : {packPath, packPath.parent_path() / RESOURCE_PACK_TEMP_FILENAME}) {
        fs::remove(path, ec);
        if (ec) {
            setError("Failed to remove " + path.string() + ": " + ec.message());
            return false;
        }
    }

    return true;
}

bool editor::Exporter::writeNativeResourcePack(const fs::path& outputPath, bool& created) {
    setProgressRaw("Packing resources...", 0.95f);

    created = false;
    std::vector<ResourcePackBuildEntry> entries;
    const fs::path projectRoot = getExportProjectRoot();

    // Written aside and renamed, so a half-written pack is never left for the cleanup
    const fs::path tempPath = outputPath.parent_path() / RESOURCE_PACK_TEMP_FILENAME;

    // Sizing pass: paths and sizes settle the header, so no bytes are read yet
    std::error_code ec;
    for (const char* rootName : {"assets", "lua"}) {
        const fs::path root = projectRoot / rootName;

        fs::recursive_directory_iterator files(root, fs::directory_options::skip_permission_denied, ec);
        if (ec == std::errc::no_such_file_or_directory) continue;
        if (ec) {
            setError(std::string("Failed to read exported ") + rootName + " directory: " + ec.message());
            return false;
        }

        for (fs::recursive_directory_iterator end; files != end; files.increment(ec)) {
            if (isCancelled()) {
                setError("Build cancelled");
                return false;
            }

            const fs::path filePath = files->path();
            const bool regular = files->is_regular_file(ec);
            if (ec) {
                setError("Failed to read resource for pack: " + filePath.string() + ": " + ec.message());
                return false;
            }
            if (!regular) continue;
            // A pack left by an earlier export into the same directory is replaced, not packed
            if (filePath == outputPath || filePath == tempPath) continue;

            fs::path relPath = fs::relative(filePath, projectRoot, ec);
            if (ec || relPath.empty()) {
                setError("Failed to resolve resource path for pack: " + filePath.string());
                return false;
            }

            ResourcePackBuildEntry entry;
            entry.path = relPath.generic_string();
            if (entry.path.size() > UINT16_MAX) {
                setError("Resource path is too long for pack: " + entry.path);
                return false;
            }

            const uintmax_t fileSize = fs::file_size(filePath, ec);
            if (ec) {
                setError("Failed to read resource size for pack: " + filePath.string() + ": " + ec.message());
                return false;
            }
            // Data::open() reports lengths as unsigned int
            if (fileSize > UINT32_MAX) {
                setError("Resource is too large to pack: " + filePath.string());
                return false;
            }

            entry.source = filePath;
            entry.size = static_cast<uint64_t>(fileSize);
            deriveObfuscation(entry);

            entries.push_back(std::move(entry));
        }
        // increment() lands on end when it fails, so the loop condition exits first
        if (ec) {
            setError(std::string("Failed to walk exported ") + rootName + " directory: " + ec.message());
            return false;
        }
    }

    if (entries.empty()) {
        return removeResourcePack(outputPath);
    }

    std::sort(entries.begin(), entries.end(), [](const ResourcePackBuildEntry& a, const ResourcePackBuildEntry& b) {
        return a.path < b.path;
    });

    // magic + count, then per entry: u16 path length, path, u64 offset, u64 size, u8 key, u32 shift
    uint64_t offset = sizeof(RESOURCE_PACK_MAGIC) + 4;
    for (const auto& entry : entries) {
        offset += 2 + entry.path.size() + 8 + 8 + 1 + 4;
    }
    for (auto& entry : entries) {
        entry.offset = offset;
        offset += entry.size;
    }

    if (offset > MAX_RESOURCE_PACK_SIZE) {
        setError("Packed resources exceed the 2 GiB limit. Disable Native Resource Pack in Project Settings > Player > Publishing Settings.");
        return false;
    }

    fs::create_directories(outputPath.parent_path(), ec);
    if (ec) {
        setError("Failed to create resource pack directory: " + ec.message());
        return false;
    }

    std::ofstream out(tempPath, std::ios::binary);
    if (!out) {
        setError("Failed to create resource pack: " + tempPath.string());
        return false;
    }

    out.write(RESOURCE_PACK_MAGIC, sizeof(RESOURCE_PACK_MAGIC));
    writeU32(out, static_cast<uint32_t>(entries.size()));

    for (const auto& entry : entries) {
        writeU16(out, static_cast<uint16_t>(entry.path.size()));
        out.write(entry.path.data(), static_cast<std::streamsize>(entry.path.size()));
        writeU64(out, entry.offset);
        writeU64(out, entry.size);
        writeU8(out, entry.key);
        writeU32(out, entry.shift);
    }

    // Write pass: one resource in memory at a time, so packing does not scale with the project
    std::vector<unsigned char> data;
    bool bodiesWritten = true;
    for (const auto& entry : entries) {
        if (isCancelled()) {
            setError("Build cancelled");
            bodiesWritten = false;
            break;
        }
        if (!readWholeFile(entry.source, data)) {
            setError("Failed to read resource for pack: " + entry.source.string());
            bodiesWritten = false;
            break;
        }
        if (data.size() != entry.size) {
            setError("Resource changed while packing: " + entry.source.string());
            bodiesWritten = false;
            break;
        }

        obfuscate(data, entry.key, entry.shift);
        if (!data.empty()) {
            out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        }
    }

    // A flush failure only surfaces on close, and Windows will not remove an open file
    out.close();
    if (bodiesWritten && !out) {
        setError("Failed to write resource pack: " + tempPath.string());
        bodiesWritten = false;
    }
    if (!bodiesWritten) {
        fs::remove(tempPath, ec);
        return false;
    }

    if (isCancelled()) {
        fs::remove(tempPath, ec);
        setError("Build cancelled");
        return false;
    }

    // published like an atomic write: retried while another program holds the old pack
    io::AtomicWriteOptions options;
    options.durability = io::Durability::Cache;
    const io::WriteResult published = io::publishFile(tempPath, outputPath, options);
    if (!published.ok()) {
        setError("Failed to replace resource pack: " + io::describeWriteError(published));
        return false;
    }

    created = true;
    return true;
}

bool editor::Exporter::collectSourceResourcePack() {
    const fs::path projectRoot = getExportProjectRoot();
    const fs::path assetsDir = projectRoot / "assets";
    const fs::path luaDir = projectRoot / "lua";
    const fs::path packPath = assetsDir / RESOURCE_PACK_FILENAME;

    // Overwriting an earlier export keeps its files, and a pack shadows the loose trees
    if (!config.packNativeResources) {
        return removeResourcePack(packPath);
    }

    bool packCreated = false;
    if (!writeNativeResourcePack(packPath, packCreated)) {
        return false;
    }
    if (!packCreated) {
        return true;
    }

    if (isCancelled()) {
        removeResourcePack(packPath);
        setError("Build cancelled");
        return false;
    }

    // Only the pack ships, but the lua directory itself stays: the exported Xcode
    // workspace lists it as a resource folder and fails the build when it is missing.
    std::error_code ec;
    std::vector<fs::path> removable;
    if (!collectPackedTreeEntries(assetsDir, removable, packPath)) return false;
    if (!collectPackedTreeEntries(luaDir, removable)) return false;

    for (const fs::path& path : removable) {
        removeAllNoFollow(path, ec);
        if (ec) {
            setError("Failed to remove unpacked resource after packing: " + path.string() + ": " + ec.message());
            return false;
        }
    }

    return true;
}

// Gathers a packed tree's contents before anything is removed, so the walk never
// mutates the directory it is reading. The directory itself is left in place.
bool editor::Exporter::collectPackedTreeEntries(const fs::path& dir, std::vector<fs::path>& out, const fs::path& keep) {
    std::error_code ec;
    fs::directory_iterator entries(dir, ec);
    if (ec == std::errc::no_such_file_or_directory) return true;

    for (fs::directory_iterator end; !ec && entries != end; entries.increment(ec)) {
        if (!keep.empty() && entries->path() == keep) continue;

        out.push_back(entries->path());
    }
    if (ec) {
        setError("Failed to read exported directory " + dir.string() + ": " + ec.message());
        return false;
    }

    return true;
}

bool editor::Exporter::collectWebArtifacts() {
    setProgressRaw("Copying artifacts...", 0.95f);

    const fs::path buildDir = config.targetDir / "build";
    const std::string appName = getAppName();

    std::error_code ec;
    fs::create_directories(config.destinationDir, ec);
    if (ec) {
        setError("Failed to create destination directory: " + ec.message());
        return false;
    }

    // the page is index.html, as web hosts (itch.io) and the prebuilt web player have it
    for (const char* ext : {".html", ".js", ".wasm"}) {
        const bool page = std::string(ext) == ".html";
        const fs::path src = buildDir / (appName + (page ? ".export.html" : ext));
        if (!fs::exists(src, ec)) {
            setError("Built web output not found: " + src.string());
            return false;
        }
        if (!copyOrFail(src, config.destinationDir / (page ? std::string("index.html") : appName + ext))) return false;
    }
    // an earlier export's page (before index.html), which would load this build's player as if current
    std::string lowerName = appName;
    std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lowerName != "index" && !removeStale(config.destinationDir / (appName + ".html"))) return false;

    if (!copyLicenses()) {
        return false;
    }

    // export-settings/apply.cmake always links one (writeWebFavicon)
    const fs::path favicon = buildDir / "favicon.png";
    if (fs::exists(favicon, ec)) {
        if (!copyOrFail(favicon, config.destinationDir / "favicon.png")) return false;
    } else if (!removeStale(config.destinationDir / "favicon.png")) {
        return false;
    }

    // The .data preload bundle exists only when the project has assets or lua; an earlier
    // build's would be loaded by this one's page
    const fs::path dataSrc = buildDir / (appName + ".data");
    if (fs::exists(dataSrc, ec)) {
        if (!copyOrFail(dataSrc, config.destinationDir / (appName + ".data"))) return false;
    } else if (!removeStale(config.destinationDir / (appName + ".data"))) {
        return false;
    }

    return true;
}

namespace {

    // S06's cross-origin isolation service worker and the loader that picks the single-thread or
    // the pthreads player: engine/platform/emscripten/web/, shared with the prebuilt web player
    bool readWebTemplate(const fs::path& targetDir, const char* name, std::string& out) {
        std::error_code ec;
        fs::path path = targetDir / "platform" / "emscripten" / "web" / name;
        if (!fs::is_regular_file(path, ec)) path = editor::FileUtils::getEngineDir() / "platform" / "emscripten" / "web" / name;
        std::ifstream in(path, std::ios::binary);
        if (!in) return false;
        out.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        return true;
    }

}

bool editor::Exporter::collectWebThreadsArtifacts() {
    setProgressRaw("Copying multithreaded artifacts...", 0.97f);

    const fs::path buildDir = config.targetDir / buildDirName;
    const std::string appName = getAppName();
    const fs::path threadsDir = config.destinationDir / "threads";

    std::error_code ec;
    fs::create_directories(threadsDir, ec);
    if (ec) {
        setError("Failed to create " + threadsDir.string() + ": " + ec.message());
        return false;
    }
    // the player's code and data; the page is the single-thread build's
    for (const char* ext : {".js", ".wasm", ".data", ".worker.js"}) {
        const fs::path src = buildDir / (appName + ext);
        const bool required = std::string(ext) == ".js" || std::string(ext) == ".wasm";
        if (!fs::exists(src, ec)) {
            if (required) {
                setError("Built multithreaded web output not found: " + src.string());
                return false;
            }
            fs::remove(threadsDir / (appName + ext), ec);
            continue;
        }
        fs::copy_file(src, threadsDir / (appName + ext), fs::copy_options::overwrite_existing, ec);
        if (ec) {
            setError("Failed to copy " + src.filename().string() + ": " + ec.message());
            return false;
        }
    }

    for (const char* name : {"coi-serviceworker.js", "player-loader.js"}) {
        std::string text;
        if (!readWebTemplate(config.targetDir, name, text)) {
            setError(std::string("The engine has no platform/emscripten/web/") + name);
            return false;
        }
        if (!FileUtils::writeIfChanged(config.destinationDir / name, text)) {
            setError(std::string("Failed to write ") + name);
            return false;
        }
    }
    // what hosts that read a _headers file (and the dev server) send for isolation
    if (!writeOrFail(config.destinationDir / "_headers",
        "/*\n  Cross-Origin-Opener-Policy: same-origin\n  Cross-Origin-Embedder-Policy: require-corp\n")) return false;

    // the page loads the player through the loader instead of its own <script src>
    const fs::path page = config.destinationDir / "index.html";
    std::string html;
    {
        std::ifstream in(page, std::ios::binary);
        html.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    const std::string marker = "src=\"" + appName + ".js\"";
    const size_t at = html.find(marker);
    const size_t open = at == std::string::npos ? std::string::npos : html.rfind("<script", at);
    const size_t close = at == std::string::npos ? std::string::npos : html.find("</script>", at);
    if (open == std::string::npos || close == std::string::npos) {
        setError("The web page does not load " + appName + ".js with a <script src>; the multithreaded build cannot be wired in (custom HTML shell?)");
        return false;
    }
    html.replace(open, close + 9 - open, "<script src=\"player-loader.js\" data-st=\"" + appName + ".js\" data-mt=\"threads/" + appName +
                                             ".js\" data-worker=\"coi-serviceworker.js\"></script>");
    if (!FileUtils::writeIfChanged(page, html)) {
        setError("Failed to write " + page.string());
        return false;
    }
    Out::info("Multithreaded web player in threads/: the page uses it when cross-origin isolated (COOP/COEP headers or coi-serviceworker.js)");
    return true;
}

bool editor::Exporter::clearGenerated() {
    setProgress("Clearing generated directory...", 0.05f);

    fs::path generatedDir = project->getProjectInternalPath() / "generated";

    std::error_code ec;
    if (fs::exists(generatedDir, ec)) {
        removeAllNoFollow(generatedDir, ec);
        if (ec) {
            setError("Failed to clear generated directory: " + ec.message());
            return false;
        }
    }
    fs::create_directories(generatedDir, ec);
    if (ec) {
        setError("Failed to recreate generated directory: " + ec.message());
        return false;
    }

    return true;
}

// The harness holds an export at its most exposed point, the generated folder just cleared: while
// <path> exists, after creating <path>.held to say the export got here
void editor::Exporter::holdForTest() {
    const char* hold = std::getenv("JASPER_TEST_EXPORT_HOLD");
    if (!hold || !*hold) return;
    const fs::path path(hold);
    std::error_code ec;
    if (!fs::exists(path, ec)) return;
    std::ofstream(fs::path(path.string() + ".held")) << "held\n";
    while (fs::exists(path, ec) && !isCancelled()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

bool editor::Exporter::loadAndSaveAllScenes() {
    setProgress("Saving scene sources...", 0.1f);

    std::promise<bool> savePromise;
    auto saveFuture = savePromise.get_future();
    std::string importError;

    editor::getEditorHost().enqueueMainThreadTask([this, &savePromise, &importError]() {
        try {
            std::vector<uint32_t> temporarilyLoaded;
            auto& scenes = project->getScenes();

            // Imports done before this export reload into the open scenes first
            bool imported = project->flushFbxReloads(importError);

            // Prefab Mode's unsaved changes (Auto Save off) are the assets the build uses
            for (size_t i = 0; i < project->getPrefabStageCount(); i++) {
                PrefabStage* stage = project->getPrefabStageAt(i);
                if (stage && project->hasUnsavedPrefabStage(stage->sceneProject.id)) {
                    project->savePrefabStage(stage->sceneProject.id);
                }
            }

            // Load all unloaded scenes (opened=false to avoid UI side-effects)
            for (size_t i = 0; imported && i < scenes.size(); i++) {
                auto& sceneProject = scenes[i];
                if (sceneProject.filepath.empty() || sceneProject.scene) {
                    continue;
                }
                project->loadScene(sceneProject.filepath, false, false, true);

                temporarilyLoaded.push_back(sceneProject.id);
            }

            // A scene that did not load (or that the project only knows by name) is not in this
            // build as it is now: what it needs comes from its last known list
            for (const auto& sceneProject : scenes) {
                if (!sceneProject.filepath.empty() && !sceneProject.scene) {
                    Out::warning("Scene '%s' (%s) cannot be read: this build uses what it last knew of it, and its shaders may be incomplete.",
                                 sceneProject.name.c_str(), sceneProject.filepath.generic_string().c_str());
                }
            }
            for (const auto& [position, path] : project->getUnresolvedScenes()) {
                Out::warning("Scene %s is in the project and cannot be read: it is not in this build.", path.generic_string().c_str());
            }

            // Every FBX model needs its current import before the sources describe it
            imported = imported && project->prepareFbxModelsForExport(importError);

            // Save all scenes to regenerate their .cpp sources; one that did not load completely
            // stops the export (its save would drop data). One not saved has its old source (the
            // save writes it after the file): the export would build what the editor no longer
            // shows, so it stops too (a save refused for a GameObject it would lose)
            std::string degraded;
            std::string notSaved;
            for (size_t i = 0; imported && i < scenes.size(); i++) {
                auto& sceneProject = scenes[i];
                if (sceneProject.filepath.empty() || !sceneProject.scene) {
                    continue;
                }
                if (project->isSceneSaveGated(&sceneProject)) {
                    degraded += (degraded.empty() ? "" : ", ") + sceneProject.filepath.generic_string();
                    continue;
                }
                project->warnPreservedNotInBuild(sceneProject);
                if (!project->saveSceneToPath(sceneProject.id, sceneProject.filepath)) {
                    notSaved += (notSaved.empty() ? "'" : ", '") + sceneProject.name + "'";
                }
            }

            // never-saved scenes: their sources from what the editor holds (the cooked path below
            // ships them alike), and every scene main.cpp names has its source
            std::vector<std::string> unsavedWritten, unsavedNotOpen, withoutSource;
            if (imported && notSaved.empty() && degraded.empty()) {
                project->writeUnsavedSceneSources(unsavedWritten, unsavedNotOpen);
                // (a prebuilt player loads cooked scenes, not generated sources)
                if (unsavedNotOpen.empty() && config.mode != ExportMode::Player) withoutSource = project->scenesWithoutSource();
            }
            for (const std::string& name : unsavedWritten) {
                warn("W-SCENE-NOT-SAVED", "Scene '" + name + "' has never been saved; it ships as it is in the editor. Save it to keep it.");
            }

            // S30: a prebuilt player's cooked scenes and prefabs, from the scenes as they are
            // loaded now (the same GameObjects the generated sources describe)
            cookedFiles.clear();
            cookedPrefabs.clear();
            if (imported && config.mode == ExportMode::Player) {
                std::string cookError;
                if (!cook::SceneCooker::cookProject(project, cookedFiles, cookedPrefabs, cookError)) {
                    importError = "Cooking the scenes: " + cookError;
                    imported = false;
                }
            }

            // The UI Documents the build ships, checked by checkHtmlUIReferences()
            htmlUIDocuments.clear();
            for (const auto& sceneProject : scenes) {
                if (sceneProject.filepath.empty() || !sceneProject.scene) {
                    continue;
                }
                auto documents = sceneProject.scene->getComponentArray<UIDocumentComponent>();
                for (size_t d = 0; d < documents->size(); d++) {
                    const UIDocumentComponent& document = documents->getComponentFromIndex(d);
                    if (!document.filename.empty()) {
                        htmlUIDocuments.push_back({sceneProject.name, sceneProject.scene->getEntityName(documents->getEntity(d)), document.filename});
                    }
                }
            }

            // A disabled backend silently drops its components from the generated sources
            auto warnDroppedPhysics = [&](const char* backend, auto usesBackend) {
                std::string names;
                for (const auto& sceneProject : scenes) {
                    if (sceneProject.scene && usesBackend(sceneProject.scene)) {
                        names += (names.empty() ? "" : ", ") + sceneProject.name;
                    }
                }
                if (!names.empty()) {
                    Out::warning("%s Physics is off, so its Rigidbodies and joints are left out of the build. Scenes using them: %s", backend, names.c_str());
                }
            };
            if (!project->isPhysics2DEnabled()) {
                warnDroppedPhysics("2D", [](Scene* scene) {
                    return scene->getComponentArray<Rigidbody2DComponent>()->size() > 0 || scene->getComponentArray<Joint2DComponent>()->size() > 0;
                });
            }
            // S19 R42, S75 R36: the build's navigation warnings and errors (never blocking)
            logNavigationPreflight(project);
            if (!project->isPhysics3DEnabled()) {
                warnDroppedPhysics("3D", [](Scene* scene) {
                    return scene->getComponentArray<RigidbodyComponent>()->size() > 0 || scene->getComponentArray<Joint3DComponent>()->size() > 0;
                });
            }

            // S04: the references of the build (its scenes, and the prefabs and materials they
            // use) that resolve to no asset, checked by checkMissingAssets()
            missingBuildReferences.clear();
            project->getAssetIndex().waitUntilReady();
            {
                std::set<Guid> owners;
                std::vector<Guid> queue;
                for (const auto& sceneProject : scenes) {
                    if (sceneProject.filepath.empty()) continue;
                    const Guid guid = project->getAssetDatabase().getAssetGuid(sceneProject.filepath);
                    if (!guid.isNull() && owners.insert(guid).second) queue.push_back(guid);
                }
                for (size_t i = 0; i < queue.size(); i++) {
                    for (const DepEdge& edge : project->getAssetIndex().dependenciesOf(queue[i], 1)) {
                        if (edge.target.isNull() || (edge.expected != AssetType::Prefab && edge.expected != AssetType::Material)) continue;
                        const AssetDatabase::Resolution found = project->getAssetDatabase().resolve(edge.target, edge.targetPath, edge.expected, false);
                        if (found.found() && owners.insert(found.guid).second) queue.push_back(found.guid);
                    }
                }
                MissingAssetTracker& tracker = project->getMissingAssets();
                tracker.update(project->getAssetDatabase(), project->getAssetIndex(), true);
                for (const MissingAssetTracker::Group& group : tracker.getGroups()) {
                    for (const MissingAssetTracker::Referrer& referrer : group.referrers) {
                        if (!owners.count(referrer.owner)) continue;
                        std::string line = referrer.ownerPath;
                        if (!referrer.entityPath.empty()) line += " > " + referrer.entityPath;
                        if (!referrer.component.empty()) line += " > " + referrer.component;
                        line += "." + referrer.field + ": missing " + std::string(assetTypeName(group.expected)) + " " +
                                group.lastPath + (group.guid.isNull() ? std::string() : " (GUID " + group.guid.str() + ")");
                        missingBuildReferences.push_back(line);
                    }
                }
            }

            // S34: the engine modules the build needs, from what it ships while every scene is
            // loaded (a source export keeps every module its settings allow: its user's code may
            // use any)
            moduleUsageKnown = false;
            if (imported && config.mode != ExportMode::SourceCode) {
                moduleUsage = ModuleUsageDetector::analyze(ModuleUsageDetector::collect(project));
                moduleUsageKnown = true;
            }

            // Unload all scenes that were not loaded before export
            for (uint32_t sceneId : temporarilyLoaded) {
                SceneProject* sp = project->getScene(sceneId);
                if (sp) {
                    project->deleteSceneProject(sp);
                }
            }

            if (!notSaved.empty()) {
                throw std::runtime_error("scene " + notSaved + " could not be saved (see the console)");
            }
            if (!unsavedNotOpen.empty()) {
                throw std::runtime_error("scene '" + unsavedNotOpen.front() + "' has never been saved and is not open: save it or remove it from the project");
            }
            if (!withoutSource.empty()) {
                throw std::runtime_error("scene '" + withoutSource.front() + "' has no generated source, and the game's main.cpp loads it");
            }
            if (!degraded.empty()) {
                throw std::runtime_error("these scenes did not load completely and exporting saves them: " + degraded +
                                         ". Save them first (Save Anyway drops what this editor could not read, Save As keeps the original).");
            }
            savePromise.set_value(imported);
        } catch (...) {
            savePromise.set_exception(std::current_exception());
        }
    });

    try {
        if (!saveFuture.get()) {
            setError(importError);
            return false;
        }
    } catch (const std::exception& e) {
        setError(std::string("Scene save failed: ") + e.what());
        return false;
    }

    return true;
}

bool editor::Exporter::checkHtmlUIReferences(const fs::path& projectPath, const fs::path& assetsPath,
                                             const std::vector<std::pair<std::string, std::string>>& documents,
                                             std::vector<std::string>& errors, std::vector<std::string>& warnings) {
    std::error_code ec;
    const fs::path assetsRoot = fs::weakly_canonical(assetsPath, ec);

    // reported as project-relative paths (assets/ui/menu.html), like the MCP tools
    std::string prefix = fs::path(assetsPath).lexically_relative(projectPath).generic_string();
    if (prefix.empty() || prefix == ".") {
        prefix.clear();
    } else if (prefix.back() != '/') {
        prefix += '/';
    }
    auto shown = [&prefix](const std::string& assetPath) {
        return prefix + assetPath;
    };

    const HtmlUIFileReader reader = [&assetsRoot](const std::string& path, std::string* content) -> bool {
        const fs::path file = fs::path(path).is_absolute() ? fs::path(path) : assetsRoot / path;
        std::error_code error;
        if (!fs::is_regular_file(file, error)) {
            return false;
        }
        if (content) {
            std::ifstream stream(file, std::ios::binary);
            if (!stream) {
                return false;
            }
            content->assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
        }
        return true;
    };

    std::set<std::string> checked;
    for (const auto& [where, filename] : documents) {
        const std::string document = normalizeHtmlUIPath(filename);
        if (!checked.insert(document).second) {
            continue;
        }
        if (!reader(document, nullptr)) {
            errors.push_back(shown(document) + ": the document is missing (UI Document of " + where + ")");
            continue;
        }

        // .html/.htm: the compat layer's own check (phase-2 hook H14): what the document loads in
        // the game, so a remote URL the editor imported (import manifest) is fine; its errors
        // (missing files, remote URLs without an import, ...) fail the export
        if (htmlui::compat::isHtmlDocumentPath(document)) {
            htmlui::compat::CheckOptions options;
            options.verifyWithRmlUi = false;
            options.files.read = [&reader](const std::string& path, std::string& content) { return reader(path, &content); };
            options.files.exists = [&reader](const std::string& path) { return reader(path, nullptr); };
            for (htmlui::compat::Diagnostic diagnostic : htmlui::compat::checkDocument(document, options).diagnostics) {
                if (diagnostic.severity != htmlui::compat::Severity::Error) {
                    continue;
                }
                if (!diagnostic.file.empty()) {
                    diagnostic.file = shown(diagnostic.file);
                }
                errors.push_back(htmlui::compat::toText(diagnostic));
            }
            continue;
        }

        for (const HtmlUIReference& reference : collectHtmlUIReferences(document, reader)) {
            const std::string location = shown(reference.fromFile) + ":" + std::to_string(reference.line) + ":" + std::to_string(reference.column);
            if (reference.remote) {
                errors.push_back(location + ": remote " + getHtmlUIReferenceKindName(reference.kind) + " '" + reference.raw +
                                 "': games load no URLs; download it into the project's assets directory (in an .html document the URL can stay: the editor imports it)");
            } else if (reference.outsideAssets) {
                errors.push_back(location + ": '" + reference.raw + "' resolves outside the assets directory, which is all a build ships");
            } else if (reference.absolute) {
                warnings.push_back(location + ": '" + reference.raw + "' is an absolute path: it only exists on this machine; use a path relative to the document");
                if (reference.missing) {
                    errors.push_back(location + ": '" + reference.raw + "' does not exist");
                }
            } else if (reference.missing) {
                errors.push_back(location + ": " + getHtmlUIReferenceKindName(reference.kind) + " '" + reference.raw + "' not found (" +
                                 shown(reference.path) + ")");
            }
        }
    }

    return errors.empty();
}

void editor::Exporter::checkSoundEventClips() {
    // a missing clip is a warning: the game plays the event's other clips
    for (const fs::path& file : ProjectChecks::findProjectFiles(project->getProjectPath().lexically_normal(), {".soundevent"})) {
        SoundEventDef def;
        std::string error;
        const std::string name = project->normalizeToAssetsRelative(file).generic_string();
        if (!SoundEventDocument::read(file, def, &error)) {
            Out::warning("Sound Event %s does not read, so it will not play: %s", name.c_str(), error.c_str());
            continue;
        }
        for (const std::string& problem : SoundEventDocument::problems(project, def)) {
            if (problem.find("missing clip") != std::string::npos) Out::warning("Sound Event %s: %s", name.c_str(), problem.c_str());
        }
    }
}

bool editor::Exporter::checkHtmlUIReferences() {
    if (htmlUIDocuments.empty()) {
        return true;
    }
    setProgress("Checking HTML UI references...", 0.25f);

    std::vector<std::pair<std::string, std::string>> documents;
    for (const HtmlUIDocumentUse& use : htmlUIDocuments) {
        documents.push_back({"'" + use.entity + "' in scene '" + use.scene + "'", use.filename});
    }

    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    const bool ok = checkHtmlUIReferences(project->getProjectPath(), project->getAssetsPath(), documents, errors, warnings);

    for (const std::string& warning : warnings) {
        Out::warning("[HTML UI] %s", warning.c_str());
    }
    if (ok) {
        return true;
    }

    std::string message = "HTML UI documents refer to files the build cannot ship:";
    for (const std::string& error : errors) {
        Out::error("[HTML UI] %s", error.c_str());
        message += "\n" + error;
    }
    setError(message);
    return false;
}

bool editor::Exporter::copyGenerated() {
    setProgress("Copying generated files...", 0.2f);

    fs::path generatedSrc = project->getProjectInternalPath() / "generated";
    fs::path generatedDst = getExportProjectRoot();
    cppExportedPaths.clear();

    std::error_code ec;
    fs::create_directories(generatedDst, ec);
    if (ec) {
        setError("Failed to create " + FileUtils::pathToGenericUtf8(generatedDst) + ": " + ec.message());
        return false;
    }

    // main.cpp is handled separately below. PlatformEditor.* is the GLFW host
    // the editor used to emit into every project; projects created before it
    // was dropped still carry the files, and the exported build globs every
    // *.cpp under the project root, so a stale copy would be compiled.
    static const std::set<std::string> excludedFiles = {
        "main.cpp", "PlatformEditor.h", "PlatformEditor.cpp"
    };

    if (fs::exists(generatedSrc, ec)) {
        const bool copied = walkFiles(generatedSrc, nullptr, [&](const fs::directory_entry& entry, const fs::path& relativePath) {
            if (excludedFiles.count(FileUtils::pathToUtf8(relativePath.filename()))) return true;
            if (!copyOrFail(entry.path(), generatedDst / relativePath)) return false;
            cppExportedPaths.insert(relativePath);
            return true;
        });
        if (!copied) return false;
    }

    // Process main.cpp: copy from Generator output but strip editor-specific parts
    fs::path mainSrc = generatedSrc / "main.cpp";
    if (fs::exists(mainSrc, ec)) {
        std::ifstream ifs(mainSrc, std::ios::in | std::ios::binary);
        if (!ifs) {
            setError("Failed to read generated main.cpp");
            return false;
        }
        std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        ifs.close();

        // Remove the entry point (the exported build compiles the engine's own
        // platform main.cpp, which already defines main()). Generator brackets
        // it with markers because the block is preprocessor-conditional.
        const std::string entryBegin = "// JASPERENGINE_ENTRY_POINT_BEGIN\n";
        const std::string entryEnd = "// JASPERENGINE_ENTRY_POINT_END\n";
        size_t pos = content.find(entryBegin);
        if (pos != std::string::npos) {
            size_t endPos = content.find(entryEnd, pos);
            if (endPos != std::string::npos) {
                endPos += entryEnd.size();
                while (endPos < content.size() && content[endPos] == '\n') endPos++;
                content.erase(pos, endPos - pos);
            }
        }

        // Update start scene if user selected one
        if (config.startSceneId != 0) {
            std::string startSceneName;
            for (const auto& sceneProject : project->getScenes()) {
                if (sceneProject.id == config.startSceneId) {
                    startSceneName = sceneProject.name;
                    break;
                }
            }
            if (!startSceneName.empty()) {
                std::string loadPrefix = "SceneManager::loadScene(\"";
                pos = content.find(loadPrefix);
                const size_t nameStart = pos == std::string::npos ? pos : pos + loadPrefix.size();
                const size_t nameEnd = pos == std::string::npos ? pos : content.find("\")", nameStart);
                if (nameEnd == std::string::npos) {
                    // the default start would ship instead of the chosen one
                    setError("The generated main.cpp has no start scene to replace with '" + startSceneName + "'");
                    return false;
                }
                content.replace(nameStart, nameEnd - nameStart, startSceneName);
            }
        }

        if (!writeOrFail(generatedDst / "main.cpp", content)) return false;
    }

    // Also copy scene_scripts.cpp
    fs::path sceneScriptsSrc = project->getProjectInternalPath() / "scene_scripts.cpp";
    if (fs::exists(sceneScriptsSrc, ec)) {
        std::ifstream ifs(sceneScriptsSrc, std::ios::in | std::ios::binary);
        if (!ifs) {
            setError("Failed to read generated scene_scripts.cpp");
            return false;
        }

        std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        const std::string editorOnlyComment = "// JasperEngine API headers for this project are provided by .jasperengine/engine-api; see generated CMakeLists.txt for local and upstream source references.\n";
        const std::string exportComment = "// This file binds scene script metadata to compiled C++ and Lua scripts for the current build configuration.\n";
        size_t commentPos = content.find(editorOnlyComment);
        if (commentPos != std::string::npos) {
            content.replace(commentPos, editorOnlyComment.size(), exportComment);
        }

        if (!writeOrFail(generatedDst / "scene_scripts.cpp", content)) return false;
        cppExportedPaths.insert("scene_scripts.cpp");
    }

    return true;
}

bool editor::Exporter::checkMissingAssets() {
    if (missingBuildReferences.empty()) {
        return true;
    }
    // errors that stop the build, warnings when it goes on anyway
    for (const std::string& line : missingBuildReferences) {
        if (config.allowMissingAssets) Out::warning("%s", line.c_str());
        else Out::error("%s", line.c_str());
    }
    if (config.allowMissingAssets) {
        Out::warning("Exporting with %zu missing asset reference(s) (Export Anyway)", missingBuildReferences.size());
        return true;
    }
    // the first ones in the error itself: what an agent or a CI log reads of a failed export
    constexpr size_t shownReferences = 5;
    std::string listed;
    for (size_t i = 0; i < missingBuildReferences.size() && i < shownReferences; i++) {
        listed += "\n  " + missingBuildReferences[i];
    }
    if (missingBuildReferences.size() > shownReferences) {
        listed += "\n  (" + std::to_string(missingBuildReferences.size() - shownReferences) + " more)";
    }
    setError(std::to_string(missingBuildReferences.size()) + " reference(s) of the build point at missing assets "
             "(listed in the Console, and in Window > Assets > Missing Assets). Fix them, or export anyway." + listed);
    return false;
}

bool editor::Exporter::writeAssetMap() {
    // every asset the game can name by GUID: its current path in the form references use
    // (relative to the assets directory for asset types, to the project otherwise), its
    // former GUIDs and its sub-assets ("<model>#<key>")
    const AssetIndex::Roots roots = project->getAssetRoots();
    std::vector<AssetMap::Entry> entries;
    project->getAssetDatabase().forEachAsset([&](const AssetDatabase::AssetInfo& info) {
        const AssetTypeInfo& type = AssetTypes::info(info.type);
        std::string path = info.path;
        if (type.base == AssetPathBase::Assets) {
            if (!roots.assetsDir.empty()) {
                if (path.compare(0, roots.assetsDir.size() + 1, roots.assetsDir + "/") != 0) return; // not shipped
                path = path.substr(roots.assetsDir.size() + 1);
            }
        }
        entries.push_back({info.guid, info.type, false, path});
        for (const Guid& former : info.formerGuids) entries.push_back({former, info.type, true, path});
        for (const AssetDatabase::SubAsset& sub : info.subAssets) entries.push_back({sub.guid, AssetType::SubAsset, false, path + "#" + sub.key});
    });
    const std::vector<uint8_t> bytes = AssetMap::encode(std::move(entries));
    const fs::path target = getExportProjectRoot() / "assets" / AssetMap::fileName;
    std::string error;
    if (!assetio::writeFile(target, std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), assetio::Durability::Cache, &error)) {
        setError("Failed to write " + target.string() + ": " + error);
        return false;
    }
    return true;
}

bool editor::Exporter::gatherImportedAssets() {
    importedRecords.clear();
    using pixelart::aseprite::AsepriteImporter;
    using pixelart::aseprite::ImportRecord;
    // the importer belongs to the main thread: asked there until every file's import is in
    std::vector<std::string> paths;
    bool first = true;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(10);
    for (;;) {
        std::promise<size_t> asked;
        auto future = asked.get_future();
        editor::getEditorHost().enqueueMainThreadTask([this, &paths, &asked, first]() {
            AsepriteImporter& importer = AsepriteImporter::get();
            if (importer.getProject() != project) importer.attach(project);
            if (first) {
                project->getAssetDatabase().forEachAsset([&](const AssetDatabase::AssetInfo& info) {
                    if (info.type == AssetType::AsepriteSprite) paths.push_back((project->getProjectPath() / info.path).string());
                });
                for (const std::string& path : paths) {
                    if (!importer.record(path) && !importer.isPending(path)) importer.reimport(path);
                }
            }
            importer.update();
            size_t pending = 0;
            for (const std::string& path : paths) pending += importer.isPending(path) ? 1 : 0;
            if (pending == 0) {
                for (const std::string& path : paths) importedRecords.push_back(importer.record(path));
            }
            asked.set_value(pending);
        });
        const size_t pending = future.get();
        first = false;
        if (pending == 0) break;
        if (isCancelled()) {
            setError("Build cancelled");
            return false;
        }
        if (std::chrono::steady_clock::now() > deadline) {
            setError("Cannot export: " + std::to_string(pending) + " Aseprite file(s) are still importing");
            return false;
        }
        setDetail("Importing " + std::to_string(pending) + " Aseprite file(s)");
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    for (size_t i = 0; i < importedRecords.size(); i++) {
        const std::shared_ptr<const ImportRecord>& record = importedRecords[i];
        if (!record || !record->ok) {
            const std::string rel = editor::FileUtils::pathToUtf8(fs::path(paths[i]).lexically_relative(project->getProjectPath()));
            setError("Cannot export: " + rel + " could not be imported" +
                     (record ? ": " + record->error.kind + ": " + record->error.message : std::string()));
            return false;
        }
    }
    return true;
}

namespace {
    // "je-imported/<k[0:2]>/<k>/": where an import's files go in an export (the sheet records name
    // their pages so)
    std::string exportedImportDir(const jasperengine::editor::pixelart::aseprite::ImportRecord& record) {
        return std::string(jasperengine::ImportedAssets::kImportedDir) + "/" + record.key.substr(0, 2) + "/" + record.key + "/";
    }
}

bool editor::Exporter::copyImportedAssets(const fs::path& assetsDst) {
    std::error_code ec;
    fs::remove_all(assetsDst / ImportedAssets::kImportedDir, ec);
    if (importedRecords.empty() && atlasBuilds.empty() && textureSheetFiles.empty()) return true;
    nlohmann::json files = nlohmann::json::object();
    auto ship = [&](const fs::path& from, const std::string& to) { return copyOrFail(from, assetsDst / editor::FileUtils::pathFromUtf8(to)); };
    // S45: texture sheets, then the atlases (a packed member's remap replaces its own sheet)
    for (const auto& [texture, file] : textureSheetFiles) {
        const std::string to = std::string(ImportedAssets::kImportedDir) + "/sheets/" + fs::path(file).filename().string();
        if (!ship(file, to)) return false;
        files[ImportedAssets::reference(texture, "sheet")] = to;
    }
    std::set<std::string> shippedAtlasFiles;
    std::map<std::string, std::string> claimed;
    for (const auto& [atlas, remaps] : atlasBuilds) {
        for (const sprite::AtlasBuildResult::ExportedFile& f : atlas->exportedFiles(remaps)) {
            const bool cookedPage = f.key.rfind("image#", 0) == 0 && cookedTextureSources.count(f.exportPath);
            if (!cookedPage && !shippedAtlasFiles.count(f.exportPath)) {
                if (!ship(f.cacheFile, f.exportPath)) return false;
                shippedAtlasFiles.insert(f.exportPath);
            }
            // two atlases packing one source: the one whose path sorts first draws it (R18; the
            // builds come in path order)
            if (f.source != atlas->atlasPath && claimed.count(f.source) && claimed[f.source] != atlas->atlasPath) continue;
            if (f.source != atlas->atlasPath) claimed[f.source] = atlas->atlasPath;
            files[ImportedAssets::reference(f.source, f.key)] = f.exportPath;
        }
    }
    for (const auto& record : importedRecords) {
        // the record's own table: the editor's provider answers the same keys from it
        for (const pixelart::aseprite::ImportRecord::ExportedFile& f : record->exportedFiles()) {
            // a page cooked with the project's sprite rules ships as its je-cooked/ file only
            const bool cookedPage = f.key.rfind("image#", 0) == 0 && cookedTextureSources.count(f.exportPath);
            if (!cookedPage && !ship(f.cacheFile, f.exportPath)) return false;
            files[ImportedAssets::reference(record->assetPath, f.key)] = f.exportPath;
        }
    }
    const nlohmann::json index = {{"format", ImportedAssets::kIndexFormat}, {"version", ImportedAssets::kIndexVersion}, {"files", files}};
    const fs::path target = assetsDst / ImportedAssets::kImportedDir / "index.json";
    io::AtomicWriteOptions options;
    options.skipIfUnchanged = true;     // a re-export keeps the build cache's file time
    const io::WriteResult written = io::writeFileAtomic(target, index.dump(1) + "\n", options);
    if (!written.ok()) {
        setError("Failed to write " + target.string() + ": " + written.message);
        return false;
    }
    return true;
}

bool editor::Exporter::gatherSpriteAtlases(const std::vector<std::string>& platforms) {
    atlasBuilds.clear();
    textureSheetFiles.clear();
    atlasOnlySources.clear();
    // one family's layout: a source export (every platform) takes the desktop one
    std::string family = "desktop";
    if (platforms.size() == 1) {
        const std::string& p = platforms[0];
        family = p == "web" ? "web" : (p == "android" || p == "ios") ? "mobile" : "desktop";
    } else if (config.mode == ExportMode::SourceCode && config.selectedBackends.size() == 1 &&
               *config.selectedBackends.begin() == ShaderBackend::GLES3) {
        // a source export for GLES3 / WebGL2 only: the web family's layout
        family = "web";
    }
    struct Gathered {
        std::vector<sprite::AtlasBuildInput> inputs;
        std::map<std::string, std::string> sheets;
        std::map<std::string, std::vector<DepEdge>> referrers;
        std::string error;
        bool disabled = false;
    };
    Gathered gathered;
    std::promise<void> asked;
    auto future = asked.get_future();
    editor::getEditorHost().enqueueMainThreadTask([this, &gathered, &asked, family]() {
        sprite::SpriteAtlasBuilder& builder = sprite::SpriteAtlasBuilder::get();
        if (builder.getProject() != project) builder.attach(project);
        gathered.disabled = project->getSpriteAtlasMode() == "disabled";
        // textures with a Sprite Mode (or a name@WxH grid): their records ship
        project->getAssetDatabase().forEachAsset([&](const AssetDatabase::AssetInfo& info) {
            if (info.type != AssetType::Texture) return;
            const fs::path absolute = project->getProjectPath() / info.path;
            const std::string assetPath = absolute.lexically_normal().lexically_relative(project->getAssetsPath().lexically_normal()).generic_string();
            if (assetPath.rfind("..", 0) == 0 || !SpriteSheetSources::isTexturePath(assetPath)) return;
            std::string file;
            if (sprite::SpriteSheetBuilder::get().resolve(assetPath, file)) gathered.sheets[assetPath] = file;
        });
        if (!gathered.disabled) {
            project->getAssetIndex().waitUntilReady();
            for (const std::string& atlasPath : builder.atlasPaths()) {
                pixelart::PixelProblem problem;
                std::optional<sprite::SpriteAtlasDocument> doc = sprite::SpriteAtlasBuilder::readDocument(project->getAssetsPath() / atlasPath, &problem);
                if (!doc) {
                    gathered.error = atlasPath + ": " + problem.message;
                    return asked.set_value();
                }
                sprite::AtlasBuildInput input;
                sprite::SpriteAtlasBuilder::gather(*project, atlasPath, *doc, family, input);
                for (const sprite::AtlasMemberInput& m : input.members) {
                    if (m.kind != "texture" || gathered.referrers.count(m.path)) continue;
                    const fs::path absolute = project->getAssetsPath() / m.path;
                    const std::string rel = project->normalizeToProjectRelative(absolute).generic_string();
                    const AssetDatabase::Resolution res = project->getAssetDatabase().lookupPath(rel);
                    gathered.referrers[m.path] = project->getAssetIndex().referencesTo(res.found() ? res.guid : Guid(), rel, false, 4096);
                }
                gathered.inputs.push_back(std::move(input));
            }
        }
        asked.set_value();
    });
    future.get();
    if (!gathered.error.empty()) {
        setError("Cannot export: the Sprite Atlas " + gathered.error);
        return false;
    }
    textureSheetFiles = gathered.sheets;
    for (const sprite::AtlasBuildInput& input : gathered.inputs) {
        for (const pixelart::PixelProblem& p : input.problems) {
            if (p.kind == "importing" || p.kind == "missing_member") warn("W-SPRITE-ATLAS", input.atlasPath + ": " + p.message);
        }
        setDetail("Building sprite atlas " + fs::path(input.atlasPath).stem().string());
        std::shared_ptr<sprite::AtlasBuildResult> built = sprite::SpriteAtlasBuilder::get().build(input);
        if (!built->ok) {
            setError("Cannot export: the Sprite Atlas " + input.atlasPath + " does not build: " + built->error.message +
                     (built->error.fix.empty() ? std::string() : " (" + built->error.fix + ")"));
            return false;
        }
        atlasBuilds.emplace_back(built, input.doc.includeInBuild);
        if (!input.doc.includeInBuild) continue;
        // R19: a packed texture only sprites read (their sheet) ships as the atlas's pages only
        for (const sprite::AtlasMemberInput& m : input.members) {
            if (m.kind != "texture") continue;
            const std::vector<DepEdge>& edges = gathered.referrers[m.path];
            bool onlySprites = !edges.empty();
            for (const DepEdge& edge : edges) {
                const FieldLocatorParts parts = parseFieldLocator(edge.field);
                onlySprites = onlySprites && parts.hasEntity && parts.component == "SpriteComponent";
            }
            if (onlySprites) atlasOnlySources.insert(m.path);
        }
    }
    return true;
}

bool editor::Exporter::cookTextures() {
    if (!gatherImportedAssets()) return false;
    setProgress("Cooking textures...", 0.33f);
    cookedTextureSources.clear();

    // Windows/Linux/macOS -> desktop, iOS and Android -> mobile (+ android_etc2), Web -> web;
    // a source export builds for every platform, so it carries every family (R9)
    std::vector<std::string> platforms;
    if (config.mode == ExportMode::Desktop) platforms = {"linux"};
    else if (config.mode == ExportMode::Web) platforms = {"web"};
    else if (config.mode == ExportMode::Player) platforms = {buildPlatformName(config.platform)};
    else platforms = {"windows", "linux", "macos", "ios", "android", "web"};
    const std::vector<TextureFamily> families =
        TextureCookQueue::familiesForExport(platforms, project->getTextureSettings().androidEtc2Fallback);

    // the settings and references of every texture source come from the main thread
    std::map<std::string, TextureCookInputs> inputs;
    std::string gatherError;
    std::promise<bool> gathered;
    auto gatheredFuture = gathered.get_future();
    editor::getEditorHost().enqueueMainThreadTask([this, &inputs, &gatherError, &gathered]() {
        project->getAssetIndex().waitUntilReady();
        gathered.set_value(TextureExport::gather(*project, inputs, gatherError));
    });
    if (!gatheredFuture.get()) {
        setError("Texture import settings: " + gatherError);
        return false;
    }
    // S45: the export family's atlases; platform-format pages cook as sprites under their
    // je-imported/ path, lossless ones ship as PNG; a packed texture only sprites read is not cooked
    if (!gatherSpriteAtlases(platforms)) return false;
    for (const std::string& source : atlasOnlySources) inputs.erase(source);
    for (const auto& [atlas, remaps] : atlasBuilds) {
        if (atlas->compression != "platform_default") continue;
        for (const sprite::AtlasBuildResult::ExportedFile& f : atlas->exportedFiles(false)) {
            if (f.key.rfind("image#", 0) != 0) continue;
            TextureCookInputs in;
            in.assetPath = f.exportPath;
            in.projectRelative = atlas->atlasPath + " (" + f.key + ")";
            in.absolute = f.cacheFile;
            in.settings.type = TextureImportType::Sprite;
            in.settings.inferredFrom = atlas->atlasPath;
            in.hasSettings = true;
            in.inference.type = TextureImportType::Sprite;
            in.inference.reason = atlas->atlasPath;
            inputs[in.assetPath] = std::move(in);
        }
    }
    // S44: the pages of imports set to compression "project" cook as the project's sprites do,
    // under their je-imported/ path (which the sheet records name)
    for (const auto& record : importedRecords) {
        const nlohmann::json texture = record->settings.value("texture", nlohmann::json::object());
        if (!texture.is_object() || texture.value("compression", std::string("none")) != "project") continue;
        for (size_t p = 0; p < record->pages.size(); p++) {
            TextureCookInputs in;
            in.assetPath = exportedImportDir(*record) + "page" + std::to_string(p) + ".png";
            in.projectRelative = record->assetPath + " (page " + std::to_string(p) + ")";
            in.absolute = record->pageFile(p);
            in.settings.type = TextureImportType::Sprite;
            in.settings.inferredFrom = record->assetPath;
            in.hasSettings = true;
            in.inference.type = TextureImportType::Sprite;
            in.inference.reason = record->assetPath;
            inputs[in.assetPath] = std::move(in);
        }
    }
    if (inputs.empty()) {
        std::error_code ec;
        fs::remove_all(getExportProjectRoot() / "assets" / CookedTextures::kCookedDir, ec);
        return true;
    }

    const TextureExportResult result = TextureExport::cookForExport(*project, inputs, families, getExportProjectRoot() / "assets",
        (int)config.buildJobs, [this](int done, int total, const std::string& path) {
            setDetail("Cooking textures " + std::to_string(done) + "/" + std::to_string(total) + ": " + path);
        }, [this]() { return isCancelled(); });
    if (!result.ok) {
        if (result.error != "cancelled") setError("Texture cook failed: " + result.error);
        else setError("Build cancelled");
        return false;
    }
    // the export log keeps the report
    for (const std::string& line : StringUtils::split(TextureCookReport::toText(result.rows), '\n')) {
        if (!line.empty()) Out::info("%s", line.c_str());
    }
    Out::info("Textures: %d cooked, %d from the cache", result.cooked, result.cacheHits);
    cookedTextureSources = result.cookedSources;
    return true;
}

bool editor::Exporter::writeAudioImports() {
    std::vector<std::string> problems;
    const std::string text = AudioImportRegistry::exportText(AudioImportRegistry::collect(*project, problems));
    for (const std::string& problem : problems) warn("W-AUDIO-IMPORT", "import settings not exported: " + problem);
    // a project without settings writes no file: its build is byte-identical to one without this
    if (text.empty()) return true;
    const fs::path target = getExportProjectRoot() / "assets" / AudioImportTable::FILE_NAME;
    const io::WriteResult written = io::writeFileAtomic(target, text);
    if (!written.ok()) {
        setError("Can't write " + target.string() + ": " + written.message);
        return false;
    }
    return true;
}

bool editor::Exporter::writeAudioCredits() {
    const fs::path file = config.targetDir / "licenses" / AudioCredits::FILE_NAME;
    std::error_code removed;
    fs::remove(file, removed);
    std::vector<fs::path> shipped;
    const fs::path assets = project->getAssetsPath();
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(getExportProjectRoot() / "assets", fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (it->is_regular_file(ec)) shipped.push_back(fs::relative(it->path(), getExportProjectRoot() / "assets", ec));
    }
    const AudioCredits::Report report = AudioCredits::collect(assets, shipped);
    const std::string licence = AudioCredits::licenceWarning(report);
    if (!licence.empty()) warn("W-AUDIO-LICENCE", licence);
    const std::string placeholder = AudioCredits::placeholderWarning(report);
    if (!placeholder.empty()) warn("W-AUDIO-PLACEHOLDER", placeholder);
    if (report.empty() || !project->getAudioSettings().creditsFile) return true;
    const io::WriteResult written = io::writeFileAtomic(file, AudioCredits::render(report, project->getName()));
    if (!written.ok()) {
        setError("The audio credits: " + written.message);
        return false;
    }
    return true;
}

bool editor::Exporter::checkFbxImportPathFree(const fs::path& relPath) {
    fs::path importRelPath = relPath;
    importRelPath += ".glb";
    std::error_code ec;
    if (!fs::exists(getAssetsSourceDir() / importRelPath, ec)) return true;
    setError("Cannot export: " + importRelPath.generic_string() + " would replace the import of " +
        relPath.generic_string() + ". Rename it.");
    return false;
}

bool editor::Exporter::cookModels() {
    setProgress("Cooking models...", 0.34f);
    cookedModelSources.clear();
    cookedModelSizes.clear();
    std::vector<std::string> platforms;
    if (config.mode == ExportMode::Desktop) platforms = {"linux"};
    else if (config.mode == ExportMode::Web) platforms = {"web"};
    else if (config.mode == ExportMode::Player) platforms = {buildPlatformName(config.platform)};
    else platforms = {"windows", "linux", "macos", "ios", "android", "web"};
    const std::vector<ModelFamily> families = ModelCookQueue::familiesForExport(platforms);

    // the settings, the scenes' blend overrides and Read/Write's readers come from the main thread
    ModelExportInputs inputs;
    std::string gatherError;
    std::promise<bool> gathered;
    auto gatheredFuture = gathered.get_future();
    editor::getEditorHost().enqueueMainThreadTask([this, &inputs, &families, &gatherError, &gathered]() {
        project->getAssetIndex().waitUntilReady();
        gathered.set_value(ModelExport::gather(*project, families, inputs, gatherError));
    });
    if (!gatheredFuture.get()) {
        setError("Model cook failed: " + gatherError);
        return false;
    }
    // before the cook, which would read such an asset as a model and fail on it instead
    for (const ModelBatchItem& item : inputs.items) {
        const fs::path relPath = FileUtils::pathFromUtf8(item.assetPath);
        if (FbxImporter::isFbxFile(relPath) && !checkFbxImportPathFree(relPath)) return false;
    }
    const fs::path assetsOut = getExportProjectRoot() / "assets";
    if (inputs.items.empty()) {
        std::error_code ec;
        fs::remove_all(assetsOut / CookedModels::kCookedDir, ec);
        return true;
    }
    // a prebuilt player must decode what the cook writes (R30)
    if (player && std::find(player->modelCookFormats.begin(), player->modelCookFormats.end(), CookedModels::kFormatVersion) == player->modelCookFormats.end()) {
        setError("The player runtime " + player->id + " cannot load cooked models (format " + std::to_string(CookedModels::kFormatVersion) +
                 "): it predates them. Install the runtime of this editor's version (Players window, or build it with tools/players), "
                 "or export with the Compile method");
        return false;
    }
    const ModelExportResult result = ModelExport::cookForExport(*project, inputs, assetsOut, (int)config.buildJobs,
        [this](int done, int total, const std::string& path) {
            setDetail("Cooking models " + std::to_string(done) + "/" + std::to_string(total) + ": " + path);
        }, [this]() { return isCancelled(); });
    if (!result.ok) {
        if (result.error != "cancelled") setError("Model cook failed: " + result.error);
        else setError("Build cancelled");
        return false;
    }
    for (const std::string& line : StringUtils::split(ModelExport::toText(result), '\n')) {
        if (!line.empty()) Out::info("%s", line.c_str());
    }
    Out::info("Models: %d cooked, %d from the cache", result.cooked, result.cacheHits);
    cookedModelSources = result.cookedSources;
    for (const ModelExportRow& row : result.rows)
        for (const auto& [family, cooked] : row.families)
            cookedModelSizes[std::string("assets/") + CookedModels::kCookedDir + "/" + row.files.at(family)] = {row.assetPath, cooked.record.sourceBytes};
    return true;
}

void editor::Exporter::measureBuildContent(const fs::path& contentRoot) {
    std::map<std::string, std::pair<std::string, uint64_t>> cooked = cookedModelSizes;
    for (const auto& [file, source] : BuildReport::cookedTextures(contentRoot / "assets", project->getAssetsPath())) cooked[file] = source;
    for (const char* tree : {"assets", "lua", "scenes", "prefabs"}) {
        std::error_code ec;
        if (fs::is_directory(contentRoot / tree, ec)) buildReport.addTree(contentRoot / tree, cooked, {}, std::string(tree) + "/");
    }
}

void editor::Exporter::measureBuildEngine(const fs::path& root) {
    buildReport.addTree(root, {}, {"engine"});
}

bool editor::Exporter::writeBuildReport(const fs::path& outputDir, const std::string& method) {
    buildReport.platform = config.mode == ExportMode::Player ? buildPlatformName(config.platform)
                         : config.mode == ExportMode::Web ? "web" : config.mode == ExportMode::Desktop ? "desktop" : "source";
    buildReport.method = method;
    const fs::path file = outputDir / BuildReport::kFileName;
    std::string error;
    if (!buildReport.write(file, error)) {
        setError("Build Report: " + error);
        return false;
    }
    std::lock_guard<std::mutex> lock(progressMutex);
    result.buildReport = file;
    result.buildBytes = buildReport.totalBytes;
    result.buildCategories = buildReport.categories;
    BuildReport::setLast(file);
    Out::info("%s", buildReport.summary().c_str());
    return true;
}

void editor::Exporter::walkTree(const fs::path& root, const TreeVisitor& visit, std::error_code& ec) {
    ec.clear();
    // Nested links can multiply (a folder of ten links, each to a folder of ten links...), with no
    // cycle to end them: the export stops following at this many
    constexpr size_t MAX_LINKS_ENTERED = 4096;
    // where each folder of the branch being walked is on disk, root first
    std::vector<fs::path> branch = {safewrite::resolvedPath(root)};
    if (branch.back().empty()) {
        ec = std::make_error_code(std::errc::too_many_symbolic_link_levels);
        return;
    }
    // a link can lead to the export's own folder, or to one that holds it: what is written there
    // while it is walked would be shipped into itself without end
    const fs::path realOutput = config.targetDir.empty() ? fs::path() : safewrite::resolvedPath(config.targetDir);
    bool stopped = false;
    treeListFailure.clear();
    std::function<void(const fs::path&, const fs::path&)> walk = [&](const fs::path& dir, const fs::path& relDir) {
        std::error_code listError;
        // listed in the long form (past MAX_PATH on Windows); `dir` stays the plain path
        fs::directory_iterator it(exportfiles::longPath(dir), listError);
        if (listError && exportfiles::isSkippableOpenError(listError) && !relDir.empty()) {
            // a folder this user may not read (export-pipeline Deviation 9): left out, said so
            warn("W-UNREADABLE-FOLDER", FileUtils::pathToGenericUtf8(relDir) + " could not be read and was left out of the build");
            return;
        }
        for (fs::directory_iterator end; !listError && it != end && !stopped; it.increment(listError)) {
            const fs::directory_entry& entry = *it;
            const fs::path name = entry.path().filename();
            const fs::path relPath = relDir / name;
            const bool link = watch::isLink(entry);
            const fs::path real = link ? safewrite::resolvedPath(dir / name) : branch.back() / name;
            std::error_code typeError;
            const bool isDirectory = entry.is_directory(typeError);
            // an earlier export left in the project ships nothing (W-EXPORT-FOLDER-SKIPPED)
            if (isDirectory && isEarlierExport(entry)) continue;
            const TreeStep step = visit(entry, relPath, isDirectory, real);
            if (step == TreeStep::Stop) {
                stopped = true;
                return;
            }
            if (step != TreeStep::Enter || !isDirectory || real.empty()) continue;
            if (!realOutput.empty() && safewrite::isAtOrBelow(real, realOutput)) {
                if (leftOutLinks.insert(branch.back() / name).second) {
                    Out::warning("The folder \"%s\" is left out of the export: it is the export's own folder",
                                 FileUtils::pathToGenericUtf8(relPath).c_str());
                }
                continue;
            }
            if (link) {
                const bool holdsItself = std::any_of(branch.begin(), branch.end(), [&](const fs::path& folder) { return safewrite::isAtOrBelow(folder, real); });
                if (holdsItself) {
                    if (leftOutLinks.insert(branch.back() / name).second) {
                        Out::warning("The linked folder \"%s\" is left out of the export: it leads to %s, which holds it, so it would hold itself",
                                     FileUtils::pathToGenericUtf8(relPath).c_str(), FileUtils::pathToGenericUtf8(real).c_str());
                    }
                    continue;
                }
                if (++treeLinksEntered > MAX_LINKS_ENTERED) {
                    if (leftOutLinks.insert(branch.back() / name).second) {
                        Out::warning("The linked folder \"%s\" is left out of the export: the project's links lead to more than %zu folders",
                                     FileUtils::pathToGenericUtf8(relPath).c_str(), MAX_LINKS_ENTERED);
                    }
                    continue;
                }
            }
            branch.push_back(real);
            walk(dir / name, relPath);
            branch.pop_back();
        }
        // a folder listed only in part would ship without what was left: the export fails
        if (listError && !stopped) {
            ec = listError;
            treeListFailure = dir;
            stopped = true;
        }
    };
    walk(root, fs::path());
}

bool editor::Exporter::checkRootsPinned() {
    const std::vector<fs::path> moved = project->getAssetDatabase().getMovedRoots();
    if (moved.empty()) return true;
    setError("Cannot export: \"" + FileUtils::pathToGenericUtf8(moved.front().lexically_proximate(project->getProjectPath())) +
             "\" no longer leads where it did when the project loaded, so the game would ship whatever is there now. "
             "Put it back, or open the project again if the change is meant.");
    return false;
}

bool editor::Exporter::copyAssets() {
    setProgress("Copying assets...", 0.35f);
    // (again here: an export takes long enough for a root to be swapped during it)
    if (!checkRootsPinned()) return false;

    const fs::path assetsSrc = getAssetsSourceDir();

    std::error_code ec;
    if (!fs::exists(assetsSrc, ec)) {
        setError("Assets directory does not exist: " + assetsSrc.string());
        return false;
    }

    fs::path assetsDst = getExportProjectRoot() / "assets";
    fs::create_directories(assetsDst, ec);
    if (ec) {
        setError("Failed to create " + FileUtils::pathToGenericUtf8(assetsDst) + ": " + ec.message());
        return false;
    }
    const fs::path nativePluginsRoot = NativePlugins::root(project->getProjectPath()).lexically_normal();

    // References are relative to this directory, so its contents become the export root, linked
    // folders included, at the paths the editor reaches them by
    bool failed = false;
    auto stop = [&failed]() {
        failed = true;
        return TreeStep::Stop;
    };
    walkTree(assetsSrc, [&](const fs::directory_entry& entry, const fs::path& relPath, bool isDirectory, const fs::path& real) {
        std::error_code ec;

        // Skip hidden directories (starting with '.') and build directories
        std::string firstComponent = relPath.begin()->string();
        if (!firstComponent.empty() && Project::isNonAssetTopLevelDir(firstComponent)) {
            return TreeStep::Skip;
        }

        // The engine loads a "resources.pak" at the asset root, so the name is reserved.
        // Matched without case and before the regular-file skip: a directory aliases it too.
        if (isReservedPackName(firstComponent)) {
            setError("\"" + firstComponent + "\" is reserved for the resource pack. "
                "Rename " + entry.path().string());
            return stop();
        }

        // The cooked textures' directory (S14)
        if (firstComponent == CookedTextures::kCookedDir) {
            setError("\"" + firstComponent + "\" is reserved for the cooked textures. Rename " + entry.path().string());
            return stop();
        }

        // The imported assets' directory (S44)
        if (firstComponent == ImportedAssets::kImportedDir) {
            setError("\"" + firstComponent + "\" is reserved for the imported assets. Rename " + entry.path().string());
            return stop();
        }

        // An .aseprite file ships as its import (copyImportedAssets), never itself
        if (SpriteSheetPool::isAsepritePath(relPath.generic_string())) return TreeStep::Skip;

        // The exporter writes the clips' import settings there (S36)
        if (relPath == AudioImportTable::FILE_NAME) {
            setError("\"" + std::string(AudioImportTable::FILE_NAME) + "\" is reserved for the audio import settings. Rename " + entry.path().string());
            return stop();
        }

        // The exporter writes the asset map there (S04)
        if (relPath == AssetMap::fileName) {
            setError("\"" + std::string(AssetMap::fileName) + "\" is reserved for the asset map. Rename " + entry.path().string());
            return stop();
        }

        // Skip project support files that should not ship as assets, and the assets' metas
        if (shouldSkipExportSupportFile(relPath) || AssetTypes::isMetaFile(relPath)) return TreeStep::Enter;

        // A texture only sprites read, packed in an atlas, ships as the atlas's pages only (S45 R19)
        if (atlasOnlySources.count(relPath.generic_string())) {
            return removeStale(assetsDst / relPath) ? TreeStep::Enter : stop();
        }
        // A texture source the cooks replace ships as assets/je-cooked/<family>/ only
        if (cookedTextureSources.count(relPath.generic_string())) {
            return removeStale(assetsDst / relPath) ? TreeStep::Enter : stop();
        }
        // and a model's as assets/je-cooked/models/ (an FBX's import too, S53)
        if (cookedModelSources.count(relPath.generic_string())) {
            if (!removeStale(assetsDst / relPath)) return stop();
            if (FbxImporter::isFbxFile(entry.path())) {
                fs::path importRelPath = relPath;
                importRelPath += ".glb";
                if (!removeStale(assetsDst / importRelPath)) return stop();
            }
            return TreeStep::Enter;
        }

        // The native plugins' sources and SDKs go into the platform projects (copyNativePlugins)
        if (!NativePlugins::platformOf(project->getProjectPath(), entry.path()).empty() ||
                entry.path().lexically_normal() == nativePluginsRoot) {
            return TreeStep::Skip;
        }

        // Skip C++ source/header files; registered scripts ship via copyCppScripts
        if (isDirectory) return TreeStep::Enter;
        std::error_code typeError;
        if (!entry.is_regular_file(typeError) || isCppSourceFile(entry.path())) return TreeStep::Skip;

        // An FBX ships as its import, which the engine loads in its place. Those the scenes
        // use were imported before the scenes were saved, others are imported here.
        if (FbxImporter::isFbxFile(entry.path())) {
            fs::path importRelPath = relPath;
            importRelPath += ".glb";
            // The engine loads the import from that path, so no asset can have it
            if (!checkFbxImportPathFree(relPath)) return stop();
            const fs::path fbxPath = project->resolveAssetPath(relPath);
            const fs::path glbPath = project->getImportedModelPath(fbxPath);
            // Scripts may load any of them, so none can be left out or outdated
            if (!project->importFbxModel(fbxPath) ||
                    FbxImporter::getState(fbxPath, glbPath, project->getAssetsPath()) != FbxImporter::State::UpToDate) {
                setError("Cannot export: " + relPath.generic_string() + " could not be imported");
                return stop();
            }
            if (!copyOrFail(glbPath, assetsDst / importRelPath)) return stop();
            return TreeStep::Skip;
        }

        // Skip Lua sources already shipped in the lua tree, and editor scripts
        if (isLuaSourceFile(entry.path()) && luaCopiedSources.count(real)) return TreeStep::Skip;
        if (isLuaSourceFile(entry.path()) && EditorScripts::isEditorOnlyScript(entry.path(), project->getProjectPath())) return TreeStep::Skip;

        // A different file at a path the lua tree already has: Android merges both trees
        // into one assets root and fails on the duplicate, so only one of them can ship.
        // Scripts stay in the lua tree; anything else stays here.
        if (luaExportedPaths.count(relPath.lexically_normal())) {
            if (isLuaSourceFile(relPath)) {
                Out::warning("Asset \"%s\" is left out of the export: the Lua script at the same path takes it",
                             relPath.generic_string().c_str());
                return TreeStep::Skip;
            }
            fs::remove(getExportProjectRoot() / "lua" / relPath, ec);
            if (ec) {
                setError("Failed to remove " + relPath.generic_string() + " from the exported lua directory: " + ec.message());
                return stop();
            }
            Out::warning("\"%s\" exists in both the Lua and assets directories; only the asset is exported",
                         relPath.generic_string().c_str());
        }

        fs::path destPath = assetsDst / relPath;
        fs::create_directories(destPath.parent_path(), ec);

        // A .cube colour LUT ships cooked (S13 R25): <name>.jlut, float16 with the 3D table at most
        // 33^3, which the game loads in place of the .cube. One that does not parse ships as it
        // is, so the game logs the same error the editor does
        if (Util::isColorLutFile(entry.path().string()) && cookColorLut(entry.path(), destPath)) return TreeStep::Skip;

        // a PNG ships without the editor's own chunks (the sprite normals recipe and paint layer,
        // the texture synthesis recipe: jasper.* text chunks a game never reads); losslessly, the
        // file read once
        if (copyPngWithoutEditorChunks(entry.path(), destPath)) return TreeStep::Skip;
        return copyOrFail(entry.path(), destPath) ? TreeStep::Skip : stop();
    }, ec);
    if (failed) return false;
    if (ec) {
        setError("Failed to list the assets: cannot read the folder " + FileUtils::pathToGenericUtf8(treeListFailure) + ": " + ec.message());
        return false;
    }
    return copyImportedAssets(assetsDst);
}

bool editor::Exporter::cookColorLut(const fs::path& source, const fs::path& destination) {
    std::ifstream in(source, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    CubeLut lut;
    std::string error;
    std::vector<uint8_t> bytes;
    if (!in || !parseCubeLut(text.str(), lut, error) || !writeJlut(lut, bytes)) {
        Out::warning("%s ships uncooked: %s", source.filename().string().c_str(), error.empty() ? "it could not be read" : error.c_str());
        return false;
    }
    fs::path cooked = destination;
    cooked.replace_extension(".jlut");
    io::AtomicWriteOptions options;
    options.skipIfUnchanged = true;     // a re-export keeps the build cache's file time
    const io::WriteResult written = io::writeFileAtomic(cooked, std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), options);
    if (!written.ok()) {
        Out::warning("%s ships uncooked: %s", source.filename().string().c_str(), written.message.c_str());
        return false;
    }
    // a .cube left from an earlier export would shadow nothing (the game prefers the .jlut), but
    // it is not part of this one
    std::error_code ec;
    fs::remove(destination, ec);
    return true;
}

bool editor::Exporter::copyPngWithoutEditorChunks(const fs::path& source, const fs::path& destination) {
    std::string ext = jasperengine::editor::FileUtils::pathToUtf8(source.extension());
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ext != ".png") return false;
    // an explicit sized read: a short or failed read (an I/O error mid-file) falls back to the
    // caller's copy_file instead of shipping truncated bytes
    std::error_code ec;
    const uintmax_t size = fs::file_size(source, ec);
    if (ec || size > static_cast<uintmax_t>(std::numeric_limits<std::streamsize>::max())) return false;
    std::ifstream in(source, std::ios::binary);
    if (!in) return false;
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    if (size > 0 && (!in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size)) ||
                     in.gcount() != static_cast<std::streamsize>(size))) {
        return false;
    }
    // read once: the stripped bytes, or (nothing to strip, not a well-formed PNG) the bytes as read
    std::vector<uint8_t> stripped;
    const std::vector<uint8_t>& shipped = pngchunks::stripEditorChunks(bytes, stripped) ? stripped : bytes;
    // written the atomic way (S03): a failed write leaves no part of a file to ship
    io::AtomicWriteOptions options;
    options.skipIfUnchanged = true;     // a re-export keeps the build cache's file time
    if (!io::writeFileAtomic(destination, std::string_view(reinterpret_cast<const char*>(shipped.data()), shipped.size()), options).ok()) {
        return false;   // copied as it is instead
    }
    return true;
}

bool editor::Exporter::copyCustomShaderSources() {
    // Without runtime compilation a game still reads the manifests of its forks (parameter
    // defaults, textures, render state) from their entry points: those ship, includes do not
    const bool runtimeShaders = project->isRuntimeShadersEnabled();

    setProgress("Copying shader sources...", 0.38f);

    // A fork's customShader value, and every path its includes resolve against, is
    // project-relative; the runtime reads them from the asset root. With the assets
    // directory at the project root they are already there.
    const fs::path projectRoot = project->getProjectPath();
    const fs::path assetsDst = getExportProjectRoot() / "assets";
    std::error_code ec;
    bool failed = false;
    walkTree(projectRoot, [&](const fs::directory_entry& entry, const fs::path& relPath, bool isDirectory, const fs::path&) {
        // Same skips as copyAssets: hidden directories (the project's own data) and builds
        const std::string firstComponent = relPath.begin()->string();
        if (!firstComponent.empty() && Project::isNonAssetTopLevelDir(firstComponent)) return TreeStep::Skip;
        if (isDirectory) return TreeStep::Enter;
        std::error_code fileError;
        if (!entry.is_regular_file(fileError)) return TreeStep::Skip;

        std::string ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
        if (ext != ".vert" && ext != ".frag" && ext != ".glsl") return TreeStep::Skip;
        if (!runtimeShaders && ext == ".glsl") return TreeStep::Skip;

        if (!copyOrFail(entry.path(), assetsDst / relPath)) {
            failed = true;
            return TreeStep::Stop;
        }
        return TreeStep::Skip;
    }, ec);
    if (failed) return false;
    if (ec) {
        setError("Failed to list the project's shader sources: cannot read the folder " + FileUtils::pathToGenericUtf8(treeListFailure) + ": " + ec.message());
        return false;
    }

    return true;
}

bool editor::Exporter::copyLua() {
    setProgress("Copying Lua scripts...", 0.3f);
    if (!checkRootsPinned()) return false;

    // Cleared before the early returns: with no lua tree, assets keeps the Lua files
    luaCopiedSources.clear();
    luaExportedPaths.clear();
    // the first copy of an export
    leftOutLinks.clear();
    treeLinksEntered = 0;

    fs::path luaSrc = config.luaDir;
    if (luaSrc.empty()) {
        return true; // No Lua directory configured, skip
    }
    if (luaSrc.is_relative()) {
        luaSrc = project->getProjectPath() / luaSrc;
    }

    std::error_code ec;
    luaSrc = fs::weakly_canonical(luaSrc, ec);

    if (!fs::exists(luaSrc, ec)) {
        return true; // Lua directory doesn't exist, not an error
    }

    // Created by the copy below, so a project without Lua scripts has no lua directory
    fs::path luaDst = getExportProjectRoot() / "lua";

    // A directory of its own holds only what the scripts need, so all of it ships. With
    // the default "." the Lua root is the project root, where the allowlist filters.
    const bool luaRootIsProjectRoot = (luaSrc == fs::weakly_canonical(project->getProjectPath(), ec));
    // Sharing the assets root, copyAssets() ships everything but the Lua sources under the
    // same path; Android merges both trees into one and fails the build on duplicates.
    const bool luaRootIsAssetsRoot = (luaSrc == getAssetsSourceDir());

    bool failed = false;
    walkTree(luaSrc, [&](const fs::directory_entry& entry, const fs::path& relPath, bool isDirectory, const fs::path& real) {
        // Skip hidden directories (starting with '.') and build directories
        std::string firstComponent = relPath.begin()->string();
        if (!firstComponent.empty() && Project::isNonAssetTopLevelDir(firstComponent)) return TreeStep::Skip;
        if (isDirectory) return TreeStep::Enter;

        // Skip project support files that should not ship in the Lua directory, and metas (S04)
        if (shouldSkipExportSupportFile(relPath) || AssetTypes::isMetaFile(relPath)) return TreeStep::Skip;

        // Skip C++ source/header files; registered scripts ship via copyCppScripts
        std::error_code ec;
        if (!entry.is_regular_file(ec) || isCppSourceFile(entry.path())) return TreeStep::Skip;

        if (luaRootIsAssetsRoot && !isLuaSourceFile(entry.path())) return TreeStep::Skip;
        if (luaRootIsProjectRoot && !isLuaExportFile(entry.path())) return TreeStep::Skip;
        // editor scripts (Editor/ with ---@menu annotations) never ship
        if (EditorScripts::isEditorOnlyScript(entry.path(), project->getProjectPath())) return TreeStep::Skip;

        if (!copyOrFail(entry.path(), luaDst / relPath)) {
            failed = true;
            return TreeStep::Stop;
        }
        luaCopiedSources.insert(real);
        luaExportedPaths.insert(relPath.lexically_normal());
        return TreeStep::Skip;
    }, ec);
    if (failed) return false;
    if (ec) {
        setError("Failed to list the Lua scripts: cannot read the folder " + FileUtils::pathToGenericUtf8(treeListFailure) + ": " + ec.message());
        return false;
    }
    return true;
}

bool editor::Exporter::copyCppScripts() {
    setProgress("Copying C++ scripts...", 0.4f);

    fs::path scriptsDst = getExportProjectRoot() / "scripts";
    const fs::path projectRoot = project->getProjectPath();

    std::error_code ec;
    std::set<std::string> copiedPaths;
    const fs::path normalizedProjectRoot = projectRoot.lexically_normal();
    const auto projectRelativePath = [&](const fs::path& path) {
        const fs::path relPath = path.lexically_normal().lexically_relative(normalizedProjectRoot);
        if (relPath.empty() || relPath.is_absolute()
            || (relPath.begin() != relPath.end() && *relPath.begin() == "..")) {
            return fs::path();
        }
        return relPath;
    };

    // the scenes' scripts and those of the prefabs scripts instantiate
    const std::vector<SceneScriptSource> buildScripts = project->getBuildCppScripts();
    // a registered script's file, at its project-relative place under scripts/
    auto copyScriptFile = [&](const fs::path& file, bool source) {
        fs::path path = file;
        if (path.is_relative()) path = project->getProjectPath() / path;
        if (!copiedPaths.insert(path.string()).second) return true;
        if (!fs::exists(path, ec)) return true;
        const fs::path relPath = projectRelativePath(path);
        if (relPath.empty()) return true;
        if (!copyOrFail(path, scriptsDst / relPath)) return false;
        if (source) cppExportedPaths.insert(fs::path("scripts") / relPath);
        return true;
    };
    for (const auto& script : buildScripts) {
        if (!script.path.empty() && !copyScriptFile(script.path, true)) return false;
        if (!script.headerPath.empty() && !copyScriptFile(script.headerPath, false)) return false;
    }

    // Preserve unregistered support headers for project-root-relative includes.
    const bool headersCopied = walkFiles(projectRoot,
        [](const fs::directory_entry&, const fs::path& relPath) {
            return Project::isNonAssetTopLevelDir(FileUtils::pathToUtf8(*relPath.begin())) ? exportfiles::DirAction::Skip
                                                                                            : exportfiles::DirAction::Descend;
        },
        [&](const fs::directory_entry& entry, const fs::path& relPath) {
            // (entry's path is the walk's long form on Windows: names are compared by the plain one)
            const fs::path path = normalizedProjectRoot / relPath;
            if (!isCppHeaderFile(path)) return true;
            if (!copiedPaths.insert(path.string()).second) return true;
            return copyOrFail(entry.path(), scriptsDst / relPath);
        });
    if (!headersCopied) return false;

    // Sources under a script root are compiled without a script component
    // referencing them, so they ship even when nothing registered pulls them in.
    for (const fs::path& scriptDir : project->getBuildScriptDirs()) {
        const fs::path relDir = scriptDirRelativePath(scriptDir);
        if (relDir.empty()) {
            Out::warning("Script directory outside the project is not exported: %s", scriptDir.generic_string().c_str());
            continue;
        }

        const fs::path rootPath = normalizedProjectRoot / relDir;
        if (!fs::is_directory(rootPath, ec)) continue;
        // a script root that is the project's: its top-level non-asset folders are skipped (S27 R31)
        std::error_code sameEc;
        const bool atProjectRoot = fs::equivalent(rootPath, normalizedProjectRoot, sameEc);

        const bool sourcesCopied = walkFiles(rootPath,
            [atProjectRoot](const fs::directory_entry& entry, const fs::path& rel) {
                const std::string name = FileUtils::pathToUtf8(entry.path().filename());
                const bool topLevel = std::distance(rel.begin(), rel.end()) == 1;
                if (name.empty() || name[0] == '.' || name == "build" ||
                        (atProjectRoot && topLevel && Project::isNonAssetTopLevelDir(name))) {
                    return exportfiles::DirAction::Skip;
                }
                return exportfiles::DirAction::Descend;
            },
            [&](const fs::directory_entry& entry, const fs::path& rel) {
                const fs::path path = rootPath / rel;
                if (!isCppSourceFile(path)) return true;
                // the C++ editor scripts are the editor's, never a game's
                if (EditorScripts::isEditorOnlyScript(path, projectRoot)) return true;
                const fs::path relPath = (relDir / rel).lexically_normal();
                if (!copiedPaths.insert(path.string()).second) return true;
                if (!copyOrFail(entry.path(), scriptsDst / relPath)) return false;
                cppExportedPaths.insert(fs::path("scripts") / relPath);
                return true;
            });
        if (!sourcesCopied) return false;
    }

    // Goes to the project root, not scripts/, because that is where the exported
    // CMakeLists includes it from.
    const fs::path userBuildFile = projectRoot / "ProjectBuild.cmake";
    if (fs::is_regular_file(userBuildFile, ec) && !copyOrFail(userBuildFile, getExportProjectRoot() / "ProjectBuild.cmake")) {
        return false;
    }

    return true;
}

namespace {

    bool sameFileContents(const fs::path& a, const fs::path& b) {
        std::error_code ec;
        if (fs::file_size(a, ec) != fs::file_size(b, ec) || ec) return false;
        std::ifstream fa(a, std::ios::binary), fb(b, std::ios::binary);
        if (!fa || !fb) return false;
        std::vector<char> bufA(1 << 16), bufB(1 << 16);
        while (true) {
            fa.read(bufA.data(), (std::streamsize)bufA.size());
            fb.read(bufB.data(), (std::streamsize)bufB.size());
            if (fa.gcount() != fb.gcount()) return false;
            if (fa.gcount() == 0) return true;
            if (std::memcmp(bufA.data(), bufB.data(), (size_t)fa.gcount()) != 0) return false;
        }
    }

    // Mirrors the folder `src` (below `root`) into `dst` without following links: a link that
    // stays inside `root` (a framework's Versions/Current) is copied as that link (its target
    // when links cannot be made), any other link is left out with a warning; unchanged files keep
    // their time; what `src` no longer has goes
    bool mirrorNoFollow(const fs::path& root, const fs::path& src, const fs::path& dst, std::string& error) {
        std::error_code ec;
        const fs::file_status dstStatus = fs::symlink_status(dst, ec);
        if (fs::exists(dstStatus) && !fs::is_directory(dstStatus)) removeAllNoFollow(dst, ec);
        fs::create_directories(dst, ec);
        if (ec) {
            error = "cannot create " + genericUtf8Path(dst) + ": " + ec.message();
            return false;
        }
        std::set<fs::path> names;
        for (fs::directory_iterator it(src, ec), end; !ec && it != end; it.increment(ec)) {
            const fs::path from = it->path();
            const fs::path to = dst / from.filename();
            names.insert(from.filename());
            const fs::file_status status = it->symlink_status(ec);
            if (fs::is_symlink(status)) {
                const fs::path target = fs::read_symlink(from, ec);
                const fs::path resolved = (target.is_absolute() ? target : from.parent_path() / target).lexically_normal();
                const fs::path inside = resolved.lexically_relative(root.lexically_normal());
                if (ec || inside.empty() || *inside.begin() == ".." || !fs::exists(resolved, ec)) {
                    editor::Out::warning("Native plugins: %s is a link out of plugins/native (or to nothing); it is not exported",
                                         genericUtf8Path(from).c_str());
                    names.erase(from.filename());
                    continue;
                }
                std::error_code linkError;
                const fs::file_status old = fs::symlink_status(to, linkError);
                if (fs::is_symlink(old) && fs::read_symlink(to, linkError) == target) continue;
                if (fs::exists(old)) removeAllNoFollow(to, linkError);
                linkError.clear();
                fs::create_symlink(target, to, linkError);
                if (!linkError) continue;
                // no links here (Windows without the privilege): the target's content
                if (fs::is_directory(resolved, ec)) {
                    if (!mirrorNoFollow(root, resolved, to, error)) return false;
                } else {
                    std::error_code copyError;
                    fs::copy_file(resolved, to, fs::copy_options::overwrite_existing, copyError);
                    if (copyError) {
                        error = "cannot copy " + genericUtf8Path(resolved) + ": " + copyError.message();
                        return false;
                    }
                }
                continue;
            }
            if (fs::is_directory(status)) {
                if (!mirrorNoFollow(root, from, to, error)) return false;
            } else if (fs::is_regular_file(status)) {
                std::error_code copyError;
                const fs::file_status old = fs::symlink_status(to, copyError);
                if (fs::is_regular_file(old) && sameFileContents(from, to)) continue;
                if (fs::exists(old)) removeAllNoFollow(to, copyError);
                fs::copy_file(from, to, fs::copy_options::overwrite_existing, copyError);
                if (copyError) {
                    error = "cannot copy " + genericUtf8Path(from) + ": " + copyError.message();
                    return false;
                }
            }
        }
        std::vector<fs::path> stale;
        for (fs::directory_iterator it(dst, ec), end; !ec && it != end; it.increment(ec)) {
            if (!names.count(it->path().filename())) stale.push_back(it->path());
        }
        for (const fs::path& path : stale) removeAllNoFollow(path, ec);
        return true;
    }

}

bool editor::Exporter::copyNativePlugins() {
    std::vector<std::string> platforms;
    // C++ plugins are a script root: copyCppScripts ships them under scripts/
    for (const std::string& platform : NativePlugins::platformNames()) {
        if (platform != "cpp") platforms.push_back(platform);
    }
    return copyNativePluginFolders(getExportProjectRoot() / NativePlugins::relativeRoot(), platforms);
}

bool editor::Exporter::copyNativePluginFolders(const fs::path& destination, const std::vector<std::string>& platforms) {
    const fs::path source = NativePlugins::root(project->getProjectPath());
    std::error_code ec;
    for (const std::string& platform : platforms) {
        const fs::path from = source / platform;
        const fs::path to = destination / platform;
        // a real folder only (a link to one is not followed)
        if (fs::is_directory(fs::symlink_status(from, ec))) {
            std::string error;
            if (!mirrorNoFollow(source, from, to, error)) {
                setError("Failed to copy the native plugins of " + platform + ": " + error);
                return false;
            }
        } else if (fs::exists(fs::symlink_status(to, ec))) {
            // an earlier export's: the project no longer has them
            removeAllNoFollow(to, ec);
        }
    }
    return true;
}

bool editor::Exporter::ensureExecutable(const std::vector<fs::path>& files, std::string& error) {
    error.clear();
#ifdef _WIN32
    (void)files;
#else
    for (const fs::path& file : files) {
        std::error_code ec;
        fs::permissions(file, fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec, fs::perm_options::add, ec);
        if (ec) {
            error = "Could not make " + FileUtils::pathToGenericUtf8(file) + " executable: " + ec.message();
            return false;
        }
    }
#endif
    return true;
}

bool editor::Exporter::copyEngine() {
    setProgress("Copying engine...", 0.5f);

    fs::path exeDir = FileUtils::getExecutableDir();

    fs::path sdkRoot;
    const std::vector<fs::path> sdkCandidates = {
        FileUtils::getEngineDir(),
        exeDir / "engine",
        exeDir.parent_path() / "engine",
        exeDir
    };

    std::error_code ec;
    for (const auto& candidate : sdkCandidates) {
        if (fs::exists(candidate / "CMakeLists.txt", ec)) {
            sdkRoot = candidate;
            break;
        }
    }

    if (sdkRoot.empty()) {
        setError("JasperEngine SDK root not found near executable");
        return false;
    }

    auto copyDir = [&](const std::string& name) -> bool {
        fs::path src = sdkRoot / name;
        fs::path dst = config.targetDir / name;

        if (!fs::exists(src, ec)) {
            setError(name + " directory not found at: " + src.string());
            return false;
        }
        // Content-compare copy: unchanged files keep their mtime so the
        // Desktop/Web build cache stays incremental across exports. The workspaces are opened in
        // IDEs: their output is never copied from the SDK, nor removed from an exported workspace
        std::string error;
        if (!exportfiles::mirrorTree(src, dst, name == "workspaces", error)) {
            setError("Failed to copy " + name + " directory: " + error);
            return false;
        }
        return true;
    };

    if (!copyDir("core")) return false;
    if (!copyDir("libs")) return false;
    if (!copyDir("platform")) return false;
    if (!copyDir("renders")) return false;
    // the GLSL the build embeds for its runtime shader compiler
    if (!copyDir("shaderlib")) return false;
    // JasperEngineToolchain.cmake: the C++ standard and the compiler floors
    if (!copyDir("cmake")) return false;
    if (!copyDir("workspaces")) return false;
    if (config.mode == ExportMode::SourceCode) {
        fs::copy_file(sdkRoot / "jasperengine.py", config.targetDir / "jasperengine.py", fs::copy_options::overwrite_existing, ec);
        if (ec) {
            setError("Failed to copy jasperengine.py: " + ec.message());
            return false;
        }
        // run from a terminal as ./jasperengine.py and ./gradlew (docs/shipping.md)
        std::string executableError;
        if (!ensureExecutable({config.targetDir / "jasperengine.py", config.targetDir / "workspaces" / "androidstudio" / "gradlew"},
                              executableError)) {
            setError(executableError);
            return false;
        }
        if (!writeAndroidProjectSettings()) return false;
        if (!writeAppleProjectSettings()) return false;
    }

    // The SDK "shaders" dir holds only stub headers (getBase64Shader returning
    // "") under the exact names buildAndSaveShaders generates into. Copy them
    // only when missing: syncing would clobber the previous export's generated
    // headers with stubs just to regenerate them, bumping their mtime and
    // recompiling every TU that includes them on each incremental export.
    {
        fs::path src = sdkRoot / "shaders";
        fs::path dst = config.targetDir / "shaders";
        if (!fs::exists(src, ec)) {
            setError("shaders directory not found at: " + src.string());
            return false;
        }
        fs::create_directories(dst, ec);
        if (ec) {
            setError("Failed to create shaders directory: " + ec.message());
            return false;
        }
        for (const auto& entry : fs::directory_iterator(src)) {
            if (!entry.is_regular_file()) continue;
            fs::path target = dst / entry.path().filename();
            if (fs::exists(target, ec)) continue;
            fs::copy_file(entry.path(), target, ec);
            if (ec) {
                setError("Failed to copy shaders directory: " + ec.message());
                return false;
            }
        }

        // The Xcode workspace builds the engine from its own file list, with no step to
        // embed the shader library, so it reads the one written here. CMake builds embed
        // shaderlib/ themselves and prefer theirs.
        // the library shader manifests, which every build embeds
        {
            const auto& manifests = ShaderManifests::getLibraryManifestSources();
            const std::map<std::string, std::string> sorted(manifests.begin(), manifests.end());
            if (!writeOrFail(dst / "shaderlib_manifests.h", ShaderHeaderBuilder::buildShaderLibHeader(sorted, "shaderlib_manifests"))) return false;
        }
        if (project->isRuntimeShadersEnabled()) {
            const auto& library = ShaderVariantCompiler::getLibrarySources();
            const std::map<std::string, std::string> sorted(library.begin(), library.end());
            if (!writeOrFail(dst / "shaderlib.h", ShaderHeaderBuilder::buildShaderLibHeader(sorted))) return false;
        } else {
            fs::copy_file(src / "shaderlib.h", dst / "shaderlib.h", fs::copy_options::overwrite_existing, ec);
            if (ec) {
                setError("Failed to copy shaders/shaderlib.h: " + ec.message());
                return false;
            }
        }

        // Same for the HTML UI's user-agent stylesheet and its SVGs (engine/core/htmlui/compat/ua)
        if (!writeOrFail(dst / "htmlui_ua.h", htmlui::compat::UserAgentStyle::buildEmbeddedHeader())) return false;
        // and the built-in Lucide icon pack (engine/libs/lucide)
        if (!writeOrFail(dst / "htmlui_lucide.h", htmlui::compat::LucideIcons::buildEmbeddedHeader())) return false;
    }

    fs::path cmakeSrc = sdkRoot / "CMakeLists.txt";
    fs::path cmakeDst = config.targetDir / "CMakeLists.txt";
    if (!fs::exists(cmakeSrc, ec)) {
        setError("CMakeLists.txt not found at: " + cmakeSrc.string());
        return false;
    }

    std::string appName = getAppName();

    // Patch from the SDK source and write with writeIfChanged so an unchanged
    // result keeps its mtime (no spurious reconfigure of the build cache).
    std::ifstream cmakeIfs(cmakeSrc, std::ios::in | std::ios::binary);
    if (!cmakeIfs) {
        setError("Failed to read SDK CMakeLists.txt");
        return false;
    }
    std::string cmakeContent((std::istreambuf_iterator<char>(cmakeIfs)), std::istreambuf_iterator<char>());
    cmakeIfs.close();
    normalizeNewlines(cmakeContent);

    const std::string defaultAppName = "set(APP_NAME jasperengine-project)";
    const std::string patchedAppName = "set(APP_NAME " + appName + ")";
    const size_t appNamePos = cmakeContent.find(defaultAppName);
    if (appNamePos == std::string::npos) {
        setError("Exported CMakeLists.txt is missing default APP_NAME");
        return false;
    }
    cmakeContent.replace(appNamePos, defaultAppName.size(), patchedAppName);

    // Generate the embedded window/executable icon before the settings marker
    // so JASPERENGINE_WINDOW_ICON is only switched on when the files exist. A bad
    // icon degrades to an iconless export (warned), never a failed one.
    bool iconGenerated = false;
    if (!project->getWindowIcon().empty()) {
        iconGenerated = writeAppIcon();
    }
    if (!iconGenerated) {
        for (const char* name : {"app_icon.h", "app_icon.ico", "app_icon.png"}) {
            fs::remove(getExportProjectRoot() / name, ec);
            if (ec) { setError("Failed to remove previous export icon: " + ec.message()); return false; }
        }
    }
    if (!writeWindowsResourceFile(iconGenerated)) {
        return false;
    }

    const std::string projectSettingsMarker = "# @JASPERENGINE_PROJECT_SETTINGS@";
    const size_t projectSettingsPos = cmakeContent.find(projectSettingsMarker);
    if (projectSettingsPos != std::string::npos) {
        const WindowSettings window = project->getWindowSettings();

        // The title becomes a C string literal through the engine's compile definitions: carried
        // as escaped bytes (util/CMakeText.h), so any character in any language builds.
        // Control characters are sanitized upstream in Project::getWindowSettings().
        const std::string cmakeTitle = cmaketext::cStringDefinitionValue(window.title);

        // First line reuses the marker's existing indentation; subsequent lines
        // match the surrounding standalone setup block (4 spaces).
        const std::string indent = "\n    ";
        std::string projectSettings = std::string("set(JASPERENGINE_VSYNC_ENABLED ")
            + (project->isVSyncEnabled() ? "ON" : "OFF") + ")";
        projectSettings += indent + "set(JASPERENGINE_WINDOW_WIDTH " + std::to_string(window.width) + ")";
        projectSettings += indent + "set(JASPERENGINE_WINDOW_HEIGHT " + std::to_string(window.height) + ")";
        projectSettings += indent + "set(JASPERENGINE_WINDOW_MODE " + std::to_string(static_cast<int>(window.mode)) + ")";
        projectSettings += indent + std::string("set(JASPERENGINE_WINDOW_RESIZABLE ") + (window.resizable ? "ON" : "OFF") + ")";
        projectSettings += indent + "set(JASPERENGINE_WINDOW_TITLE \"" + cmakeTitle + "\")";
        // Web saves use the identifier, unless it is the default every project shares
        const std::string identifier = project->getApplicationIdentifier();
        const std::string storageId = (identifier != ApplicationSettings{}.identifier) ? identifier : project->getName();
        projectSettings += indent + "set(JASPERENGINE_STORAGE_ID \"" + toStorageId(storageId) + "\")";
        // Desktop saves (data://) go to a per-user folder named after it (System::getUserDataPath)
        projectSettings += indent + "set(JASPERENGINE_APP_IDENTIFIER \"" + toStorageId(storageId) + "\")";
        if (iconGenerated) {
            projectSettings += indent + "set(JASPERENGINE_WINDOW_ICON ON)";
        }
        projectSettings += indent + "set(JASPERENGINE_CXX_STANDARD " + std::to_string(project->getCxxStandard()) + ")";
        // the optional modules (S34 R6): those Project Settings allow, of the ones the project needs
        const std::vector<std::string> modules = compiledModules();
        auto moduleSwitch = [&](const char* option, const char* module) {
            const bool on = std::find(modules.begin(), modules.end(), module) != modules.end();
            projectSettings += indent + "set(" + option + (on ? " ON)" : " OFF)");
        };
        moduleSwitch("JASPERENGINE_PHYSICS_2D", "physics2d");
        moduleSwitch("JASPERENGINE_PHYSICS_3D", "physics3d");
        moduleSwitch("JASPERENGINE_NAVIGATION", "navigation");
        moduleSwitch("JASPERENGINE_RUNTIME_SHADERS", "runtimeshaders");
        moduleSwitch("JASPERENGINE_HTMLUI", "htmlui");
        moduleSwitch("JASPERENGINE_VIDEO", "video");
        moduleSwitch("JASPERENGINE_SOUND_LAYER", "soundlayer");
        // Development Build: the profiler in a Release game (a source export builds it as one)
        if (config.developmentBuild) {
            projectSettings += indent + "set(JASPERENGINE_DEVELOPMENT_BUILD ON)";
        }
        // Player > Logging
        const PlayerLoggingSettings& logging = project->getPlayerLogging();
        projectSettings += indent + "set(JASPERENGINE_LOG_LEVEL \"" + logging.level + "\")";
        projectSettings += indent + std::string("set(JASPERENGINE_LOG_FILE ") + (logging.fileLog ? "ON" : "OFF") + ")";
        projectSettings += indent + std::string("set(JASPERENGINE_LOG_BACKTRACE ") + (logging.backtrace ? "ON" : "OFF") + ")";
        // Player > Crash Handling
        const PlayerCrashSettings& crash = project->getPlayerCrash();
        projectSettings += indent + std::string("set(JASPERENGINE_CRASH_HANDLER ") + (crash.enabled ? "ON" : "OFF") + ")";
        projectSettings += indent + std::string("set(JASPERENGINE_CRASH_LOG_TAIL ") + (crash.includeLogTail ? "ON" : "OFF") + ")";
        projectSettings += indent + std::string("set(JASPERENGINE_CRASH_DUMP_WITH_HEAP ") + (crash.dumpWithHeap ? "ON" : "OFF") + ")";
        projectSettings += indent + std::string("set(JASPERENGINE_CRASH_HANG_WATCHDOG ") + (crash.hangWatchdog ? "ON" : "OFF") + ")";
        projectSettings += indent + "set(JASPERENGINE_CRASH_HANG_MS " + std::to_string(crash.hangSeconds * 1000) + ")";
        projectSettings += indent + std::string("set(JASPERENGINE_CRASH_WEB_OVERLAY ") + (crash.webCrashOverlay ? "ON" : "OFF") + ")";
        // App Transport Security of a bare macOS executable (Ninja/Makefile builds have no bundle)
        projectSettings += indent + std::string("set(JASPERENGINE_ALLOW_INSECURE_HTTP ") +
                           (project->getApplicationSettings().allowInsecureHttp ? "ON" : "OFF") + ")";
        cmakeContent.replace(projectSettingsPos, projectSettingsMarker.size(), projectSettings);
    } else {
        Out::warning("Exported CMakeLists.txt is missing the project settings marker; using platform defaults");
    }

    // Script roots reach the export as include directories; their sources come in
    // through the recursive glob once copyCppScripts() has copied them.
    const std::string scriptDirsMarker = "# @JASPERENGINE_SCRIPT_DIRS@";
    const size_t scriptDirsPos = cmakeContent.find(scriptDirsMarker);
    if (scriptDirsPos != std::string::npos) {
        cmakeContent.replace(scriptDirsPos, scriptDirsMarker.size(), buildScriptDirIncludes());
    }

    if (!writeExportSettingsScript(cmakeContent)) return false;
    if (!writeOrFail(cmakeDst, cmakeContent)) return false;

    return true;
}

const char* editor::Exporter::webNativeStub() {
    return "<script>window.JasperNative = window.JasperNative || {_pending: [], _pendingEvents: [], "
           "register: function(n, o) { this._pending.push([n, o]); }, "
           "emit: function(p, e, d) { this._pendingEvents.push([p, e, d]); }};</script>";
}

std::vector<std::string> editor::Exporter::webNativePluginFiles(const fs::path& webPluginsDir) {
    std::vector<std::string> files;
    std::error_code ec;
    if (!fs::is_directory(webPluginsDir, ec)) return files;
    for (const auto& entry : fs::directory_iterator(webPluginsDir, ec)) {
        std::string ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const std::string name = entry.path().filename().string();
        if (entry.is_regular_file(ec) && ext == ".js" && !name.empty() && name[0] != '.') files.push_back(genericUtf8Path(entry.path().filename()));
    }
    std::sort(files.begin(), files.end());
    return files;
}

std::string editor::Exporter::webNativeScripts(const std::vector<std::string>& scriptUrls, const std::vector<std::string>& files) {
    std::string scripts;
    for (const std::string& url : scriptUrls) {
        if (url.find_first_not_of(" \t") == std::string::npos) continue;
        scripts += "<script src=\"" + escapeXmlAttribute(url) + "\"></script>\n";
    }
    for (const std::string& file : files) scripts += "<script src=\"plugins/" + escapeXmlAttribute(file) + "\"></script>\n";
    return scripts;
}

bool editor::Exporter::writeExportSettingsScript(std::string& cmakeContent) {
    // Bracket arguments preserve user text without CMake variable expansion.
    auto literal = [](const std::string& value) {
        std::string equals = "=";
        while (value.find("]" + equals + "]") != std::string::npos) equals += "=";
        return "[" + equals + "[" + value + "]" + equals + "]";
    };
    const WebProjectSettings& web = project->getWebProjectSettings();
    const LinuxProjectSettings& linuxSettings = project->getLinuxProjectSettings();
    const fs::path settingsDir = getExportProjectRoot() / "export-settings";
    std::error_code ec;
    fs::create_directories(settingsDir, ec);
    if (ec) { setError("Cannot create export settings directory: " + ec.message()); return false; }
    auto copySettingFile = [&](const fs::path& source, const char* name) {
        const fs::path target = settingsDir / name;
        if (source.empty()) fs::remove(target, ec);
        else fs::copy_file(resolveProjectFile(project, source), target, fs::copy_options::overwrite_existing, ec);
        if (ec) { setError("Cannot prepare export setting file: " + ec.message()); return false; }
        return true;
    };
    if (config.mode != ExportMode::Desktop
            && (!writeWebFavicon(settingsDir / "favicon.png") || !copySettingFile(web.customHtmlShell, "shell.html"))) return false;
    std::string script;
    auto value = [&](const char* name, const std::string& text) {
        script += "set(" + std::string(name) + " " + literal(text) + ")\n";
    };
    value("title", escapeXmlAttribute(project->getApplicationName(web.applicationName)));
    value("head", web.headInclude);
    value("resize", web.resizeCanvasToWindow ? "ON" : "OFF");
    value("hide_ui", web.hideEmscriptenUI ? "ON" : "OFF");
    value("viewport_cover", web.viewportFitCover ? "ON" : "OFF");
    value("name", stripDesktopEntryControlChars(project->getApplicationName(linuxSettings.applicationName)));
    value("comment", stripDesktopEntryControlChars(linuxSettings.comment));
    value("categories", stripDesktopEntryControlChars(linuxSettings.categories));
    // native plugins (web): the JasperNative stub first (plugin scripts register before the
    // engine starts), the SDK URLs of web.script_urls, then plugins/native/web/*.js in name order
    // (a CMake list holds the file names: none with ';')
    std::vector<std::string> nativeFiles = webNativePluginFiles(getExportProjectRoot() / NativePlugins::relativeRoot() / "web");
    std::erase_if(nativeFiles, [](const std::string& file) { return file.find(';') != std::string::npos; });
    value("native_scripts", webNativeScripts(web.scriptUrls, nativeFiles));
    // first in <head>, always: web.head_include snippets and plugin scripts may register before
    // the engine starts (it adopts what they queued)
    value("native_stub", webNativeStub());
    {
        std::string files;
        for (const std::string& file : nativeFiles) files += (files.empty() ? "" : ";") + file;
        value("native_files", files);
    }
    script += R"cmake(
get_filename_component(output "${APP_FILE}" DIRECTORY)
get_filename_component(app "${APP_FILE}" NAME_WE)
if(WEB)
    # Keep the linker output untouched; repeated builds cannot duplicate inserts.
    file(READ "${APP_FILE}" html)
    if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/shell.html")
        file(READ "${CMAKE_CURRENT_LIST_DIR}/shell.html" shell)
        string(FIND "${shell}" "{{JASPERENGINE_DEFAULT_HTML}}" marker)
        if(marker EQUAL -1)
            message(FATAL_ERROR "Custom web HTML shell must contain {{JASPERENGINE_DEFAULT_HTML}}")
        endif()
        string(REPLACE "{{JASPERENGINE_DEFAULT_HTML}}" "${html}" html "${shell}")
    endif()
    string(FIND "${html}" "<title>" first)
    string(FIND "${html}" "</title>" last)
    if(first GREATER_EQUAL 0 AND last GREATER first)
        math(EXPR first "${first} + 7")
        string(SUBSTRING "${html}" 0 ${first} prefix)
        string(SUBSTRING "${html}" ${last} -1 suffix)
        set(html "${prefix}${title}${suffix}")
    endif()
    set(extra "${native_stub}\n${head}\n")
    if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/favicon.png")
        file(COPY "${CMAKE_CURRENT_LIST_DIR}/favicon.png" DESTINATION "${output}")
        string(APPEND extra "<link rel=\"icon\" href=\"favicon.png\">\n")
    else()
        file(REMOVE "${output}/favicon.png")
    endif()
    if(resize)
        string(APPEND extra "<style>html, body { margin: 0; width: 100%; min-height: 100%; } #canvas { width: 100% !important; height: 100vh !important; height: 100dvh !important; display: block; border: 0; padding: 0; }</style>\n")
    endif()
    if(hide_ui)
        # Keep nodes alive: the SDK's status/print callbacks reference them.
        string(APPEND extra "<style>#emscripten_logo, .emscripten_logo, #status, #progress, #spinner, .spinner, #controls, #output, body > a[href='http://emscripten.org'], body > a[href='https://emscripten.org'] { display: none !important; } .emscripten_border { border: 0 !important; } body > hr { display: none; }</style>\n")
    endif()
    if(NOT native_scripts STREQUAL "")
        string(APPEND extra "${native_scripts}")
    endif()
    # plugins/native/web, next to the page (an earlier build's copies are replaced)
    if(NOT native_files STREQUAL "")
        file(REMOVE_RECURSE "${output}/plugins")
        foreach(native_file IN LISTS native_files)
            file(COPY "${CMAKE_CURRENT_LIST_DIR}/../plugins/native/web/${native_file}" DESTINATION "${output}/plugins")
        endforeach()
    endif()
    if(viewport_cover)
        # the page covers the notch: env(safe-area-inset-*) and so Screen.safeArea report it
        string(REGEX MATCH "<meta[^>]*name=[\"']viewport[\"'][^>]*>" viewport "${html}")
        if(viewport)
            if(NOT viewport MATCHES "viewport-fit")
                string(REGEX REPLACE "content=([\"'])" "content=\\1viewport-fit=cover, " covered "${viewport}")
                string(REPLACE "${viewport}" "${covered}" html "${html}")
            endif()
        else()
            string(APPEND extra "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1, viewport-fit=cover\">\n")
        endif()
    endif()
    string(REPLACE "{{JASPERENGINE_TITLE}}" "${title}" html "${html}")
    string(FIND "${html}" "{{JASPERENGINE_HEAD_INCLUDE}}" marker)
    if(marker EQUAL -1)
        string(REPLACE "</head>" "${extra}</head>" html "${html}")
    else()
        string(REPLACE "{{JASPERENGINE_HEAD_INCLUDE}}" "${extra}" html "${html}")
    endif()
    file(WRITE "${output}/${app}.export.html" "${html}")
    # the page web hosts open; an app named index has its linker output there
    string(TOLOWER "${app}" app_lower)
    if(NOT app_lower STREQUAL "index")
        file(WRITE "${output}/index.html" "${html}")
    endif()
else()
    set(executable "${APP_FILE}")
    foreach(character IN ITEMS "\\" "\"" "`" "$")
        string(REPLACE "${character}" "\\${character}" executable "${executable}")
    endforeach()
    set(entry "[Desktop Entry]\nType=Application\nName=${name}\nComment=${comment}\nCategories=${categories}\nExec=\"${executable}\"\nPath=${output}\nTerminal=false\nStartupWMClass=${app}\n")
    if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../app_icon.png")
        file(COPY "${CMAKE_CURRENT_LIST_DIR}/../app_icon.png" DESTINATION "${output}")
        string(APPEND entry "Icon=${output}/app_icon.png\n")
    else()
        file(REMOVE "${output}/app_icon.png")
    endif()
    file(WRITE "${output}/${app}.desktop" "${entry}")
endif()
)cmake";
    if (!writeOrFail(settingsDir / "apply.cmake", script)) return false;
    cmakeContent += R"cmake(
if(TARGET ${APP_NAME} AND (EMSCRIPTEN OR CMAKE_SYSTEM_NAME STREQUAL "Linux"))
    add_custom_target(jasperengine-export-settings ALL
        COMMAND "${CMAKE_COMMAND}" "-DAPP_FILE=$<TARGET_FILE:${APP_NAME}>" "-DWEB=${EMSCRIPTEN}"
            -P "${PROJECT_ROOT}/export-settings/apply.cmake"
        DEPENDS ${APP_NAME}
        VERBATIM)
endif()
)cmake";
    // CMake and the hand-maintained Xcode project must use the same IDs.
    replaceAll(cmakeContent, "set(APP_BUNDLE_IDENTIFIER \"com.jaspergameengine.jasperengine\")",
        "if(CMAKE_SYSTEM_NAME STREQUAL \"iOS\")\nset(APP_BUNDLE_IDENTIFIER "
        + literal(project->getApplicationIdentifier(project->getIOSProjectSettings().bundleIdentifier))
        + ")\nelse()\nset(APP_BUNDLE_IDENTIFIER "
        + literal(project->getApplicationIdentifier(project->getMacOSProjectSettings().bundleIdentifier)) + ")\nendif()");
    replaceAll(cmakeContent, "MACOSX_BUNDLE_INFO_PLIST \"${JASPERENGINE_ROOT}/workspaces/xcode/macos/Info.plist\"",
        "MACOSX_BUNDLE_INFO_PLIST \"${JASPERENGINE_ROOT}/workspaces/xcode/macos/Info.plist\"\n"
        "                XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${APP_BUNDLE_IDENTIFIER}");
    return true;
}

fs::path editor::Exporter::scriptDirRelativePath(const fs::path& scriptDir) const {
    const fs::path projectRoot = project->getProjectPath().lexically_normal();
    const fs::path rootPath = (scriptDir.is_absolute() ? scriptDir : projectRoot / scriptDir).lexically_normal();
    const fs::path relPath = rootPath.lexically_relative(projectRoot);

    if (relPath.empty() || relPath.is_absolute() || *relPath.begin() == "..") {
        return {};
    }
    return relPath;
}

std::vector<std::string> editor::Exporter::getExportedScriptRoots() const {
    // copyCppScripts() mirrors the project-relative layout under scripts/, so a
    // root keeps its path there.
    std::vector<std::string> roots;
    for (const fs::path& scriptDir : project->getBuildScriptDirs()) {
        const fs::path relDir = scriptDirRelativePath(scriptDir);
        if (relDir.empty()) {
            continue;
        }
        roots.push_back(relDir == "." ? "scripts" : "scripts/" + genericUtf8Path(relDir));
    }
    return roots;
}

std::string editor::Exporter::buildScriptDirIncludes() const {
    std::string dirs;
    for (const std::string& root : getExportedScriptRoots()) {
        dirs += "\n        ${PROJECT_ROOT}/" + root;
    }

    // macOS CMake builds: the Objective-C native plugins
    std::string nativePlugins = NativePlugins::macosCMake(NativePlugins::scan(getExportProjectRoot()), "${APP_NAME}", "${PROJECT_ROOT}/plugins/native",
                                                          project->getMacOSProjectSettings().frameworks, "${JASPERENGINE_ROOT}/platform/apple");
    if (dirs.empty()) {
        return nativePlugins;
    }
    return "target_include_directories(${APP_NAME} PRIVATE" + dirs + "\n    )" + nativePlugins;
}

namespace {

    bool isHttpUrl(const std::string& value){
        std::string head = value.substr(0, 8);
        std::transform(head.begin(), head.end(), head.begin(), [](unsigned char c){ return (char)std::tolower(c); });
        return head.rfind("http://", 0) == 0 || head.rfind("https://", 0) == 0;
    }

    // a Video Player component node (scene key videoplayer) with an http(s) url, anywhere under `node`
    bool hasUrlVideoPlayerNode(const YAML::Node& node, int depth){
        if (depth > 64 || !node)
            return false;
        if (node.IsMap()){
            for (auto it = node.begin(); it != node.end(); ++it){
                const bool player = it->first.IsScalar() && (it->first.Scalar() == "videoplayer" || it->first.Scalar() == "VideoPlayerComponent");
                if (player && it->second.IsMap()){
                    const YAML::Node url = it->second["url"];
                    if (url && url.IsScalar() && isHttpUrl(url.Scalar()))
                        return true;
                }else if (hasUrlVideoPlayerNode(it->second, depth + 1)){
                    return true;
                }
            }
        }else if (node.IsSequence()){
            for (const YAML::Node& child : node){
                if (hasUrlVideoPlayerNode(child, depth + 1))
                    return true;
            }
        }
        return false;
    }

}

// A scene or prefab file's text with a Video Player (key videoplayer) whose url is http(s): the
// YAML parsed (no regex over the text: scene files have very long lines)
bool editor::Exporter::hasUrlVideoPlayer(const std::string& text) {
    if (text.find("ideoplayer") == std::string::npos && text.find("ideoPlayer") == std::string::npos)
        return false;
    try {
        return hasUrlVideoPlayerNode(YAML::Load(text), 0);
    } catch (const std::exception&) {
        return false;
    }
}

// Lua, C++ scripts and HTML UI documents that may play a URL video: a Video Player whose url (or
// source) the code sets, or an HTML <video> with an http(s) src. Conservative: the url may be
// any string the script builds, so setting one at all asks for INTERNET
std::string editor::Exporter::scriptVideoUrlFile(const std::filesystem::path& root, const std::vector<std::filesystem::path>& files) {
    for (const fs::path& relative : files) {
        std::string extension = relative.extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c){ return (char)std::tolower(c); });
        const bool lua = extension == ".lua";
        const bool cpp = extension == ".cpp" || extension == ".h" || extension == ".hpp" || extension == ".cc";
        const bool html = extension == ".html" || extension == ".htm" || extension == ".rml";
        if (!lua && !cpp && !html)
            continue;
        std::ifstream in(root / relative, std::ios::binary);
        if (!in)
            continue;
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (html){
            std::string lower = text;
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c){ return (char)std::tolower(c); });
            for (size_t at = lower.find("<video"); at != std::string::npos; at = lower.find("<video", at + 6)){
                const size_t close = lower.find('>', at);
                const std::string tag = lower.substr(at, close == std::string::npos ? std::string::npos : close - at);
                if (tag.find("src=\"http") != std::string::npos || tag.find("src='http") != std::string::npos)
                    return relative.generic_string();
            }
            // <source src="https://..."> inside a <video>
            if (lower.find("<video") != std::string::npos &&
                (lower.find("<source src=\"http") != std::string::npos || lower.find("<source src='http") != std::string::npos))
                return relative.generic_string();
            text = ExportFeatureScan::luaOfHtml(text);
        }
        if (text.find("VideoPlayer") == std::string::npos)
            continue;
        if (text.find(".url") != std::string::npos || text.find("setUrl") != std::string::npos ||
            text.find("VideoPlayerSource.URL") != std::string::npos || text.find("VideoPlayerSource::URL") != std::string::npos)
            return relative.generic_string();
    }
    return "";
}

// The first scene or prefab file of the project with a Video Player whose url is an http(s) URL
// (the scene files' text: unloaded scenes count too); "" when there is none
std::string editor::Exporter::videoUrlScene() const {
    std::vector<fs::path> files;
    for (const SceneProject& sceneProject : project->getScenes())
        files.push_back(sceneProject.filepath);
    std::error_code ec;
    for (watch::LinkedDirectoryIterator it(project->getAssetsPath(), fs::directory_options::none, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file(ec) && Util::isPrefabFile(it->path().string()))
            files.push_back(it->path());
    }
    for (const fs::path& file : files) {
        std::ifstream in(file, std::ios::binary);
        if (file.empty() || !in)
            continue;
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (hasUrlVideoPlayer(text))
            return FileUtils::pathToUtf8(file.filename());
    }
    return "";
}

bool editor::Exporter::writeAndroidProjectSettings() {
    const AndroidProjectSettings& android = project->getAndroidProjectSettings();
    if (!android.abiArmeabiV7a && !android.abiArm64V8a && !android.abiX86 && !android.abiX86_64) {
        setError("Android export needs at least one selected architecture");
        return false;
    }

    auto readText = [&](const fs::path& path, std::string& out) -> bool {
        std::ifstream ifs(path, std::ios::in | std::ios::binary);
        if (!ifs) {
            setError("Failed to read Android export file: " + path.string());
            return false;
        }
        out.assign(std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>());
        normalizeNewlines(out);
        return true;
    };

    const fs::path androidAppDir = config.targetDir / "workspaces" / "androidstudio" / "app";
    const fs::path buildGradlePath = androidAppDir / "build.gradle";
    const fs::path manifestPath = androidAppDir / "src" / "main" / "AndroidManifest.xml";
    const fs::path stringsPath = androidAppDir / "src" / "main" / "res" / "values" / "strings.xml";
    const fs::path stylesPath = androidAppDir / "src" / "main" / "res" / "values" / "styles.xml";
    const fs::path drawableDir = androidAppDir / "src" / "main" / "res" / "drawable";
    const fs::path adaptiveIconDir = androidAppDir / "src" / "main" / "res" / "mipmap-anydpi-v26";
    const fs::path mainActivityPath = config.targetDir / "platform" / "android" / "java" / "com" / "jaspergameengine" / "jasperengine" / "MainActivity.java";

    const std::string appName = project->getApplicationName(android.applicationName);

    std::string gradle;
    if (!readText(buildGradlePath, gradle)) return false;
    for (const char* marker : {"compileSdk 36", "applicationId \"com.yourcompany.project\"",
            "minSdkVersion 21", "targetSdkVersion 36", "versionCode 1", "versionName \"1.0\"",
            "                abiFilters \"arm64-v8a\"\n"
            "                abiFilters \"x86\"\n"
            "                abiFilters \"armeabi-v7a\"\n"
            "                abiFilters \"x86_64\""}) {
        if (gradle.find(marker) == std::string::npos) {
            setError("Incompatible Android Gradle export template: missing " + std::string(marker));
            return false;
        }
    }

    replaceAll(gradle, "compileSdk 36", "compileSdk " + std::to_string(android.targetSdk));
    replaceAll(gradle, "applicationId \"com.yourcompany.project\"",
        "applicationId \"" + escapeGradleString(project->getApplicationIdentifier(android.packageName)) + "\"");
    replaceAll(gradle, "minSdkVersion 21", "minSdkVersion " + std::to_string(android.minSdk));
    replaceAll(gradle, "targetSdkVersion 36", "targetSdkVersion " + std::to_string(android.targetSdk));
    replaceAll(gradle, "versionCode 1", "versionCode " + std::to_string(project->getApplicationVersionCode(android.versionCode)));
    replaceAll(gradle, "versionName \"1.0\"",
        "versionName \"" + escapeGradleString(project->getApplicationVersion(android.versionName)) + "\"");

    std::vector<std::string> abis;
    if (android.abiArm64V8a) abis.push_back("\"arm64-v8a\"");
    if (android.abiX86) abis.push_back("\"x86\"");
    if (android.abiArmeabiV7a) abis.push_back("\"armeabi-v7a\"");
    if (android.abiX86_64) abis.push_back("\"x86_64\"");
    std::string abiLine = "                abiFilters ";
    for (size_t i = 0; i < abis.size(); i++) {
        if (i > 0) abiLine += ", ";
        abiLine += abis[i];
    }
    replaceAll(gradle,
        "                abiFilters \"arm64-v8a\"\n"
        "                abiFilters \"x86\"\n"
        "                abiFilters \"armeabi-v7a\"\n"
        "                abiFilters \"x86_64\"",
        abiLine);

    // native plugins: the bridge and nativeplugins modules, repositories, gradle.properties,
    // and the manifest additions below
    NativeExportAndroid::Result nativeExport;
    {
        std::string nativeError;
        if (!NativeExportAndroid::write(config.targetDir / "workspaces" / "androidstudio", getExportProjectRoot(), android, gradle,
                                        nativeExport, nativeError)) {
            setError(nativeError);
            return false;
        }
    }
    if (!writeOrFail(buildGradlePath, gradle)) return false;

    const bool hasLauncherIcon = !android.launcherIcon.empty();
    const bool hasAdaptiveIcon = !android.adaptiveIconForeground.empty() && !android.adaptiveIconBackground.empty();
    std::string iconReference = "@mipmap/ic_launcher";

    if (hasLauncherIcon) {
        std::string copyError;
        if (!copyAndroidResourceFile(project, android.launcherIcon, drawableDir / "ic_launcher.png", copyError)) {
            setError(copyError);
            return false;
        }
        iconReference = "@drawable/ic_launcher";
    }

    if (hasAdaptiveIcon) {
        std::string copyError;
        if (!copyAndroidResourceFile(project, android.adaptiveIconForeground, drawableDir / "ic_launcher_foreground.png", copyError)) {
            setError(copyError);
            return false;
        }
        if (!copyAndroidResourceFile(project, android.adaptiveIconBackground, drawableDir / "ic_launcher_background.png", copyError)) {
            setError(copyError);
            return false;
        }

        std::string adaptiveIcon;
        adaptiveIcon += "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n";
        adaptiveIcon += "<adaptive-icon xmlns:android=\"http://schemas.android.com/apk/res/android\">\n";
        adaptiveIcon += "    <background android:drawable=\"@drawable/ic_launcher_background\" />\n";
        adaptiveIcon += "    <foreground android:drawable=\"@drawable/ic_launcher_foreground\" />\n";
        adaptiveIcon += "</adaptive-icon>\n";
        if (!writeOrFail(adaptiveIconDir / "ic_launcher.xml", adaptiveIcon)) return false;
        iconReference = "@mipmap/ic_launcher";
    }

    // INTERNET: Internet Access "require", the permission ticked, or Http in the exported code
    // (Lua, HTML UI Lua, C++ scripts)
    const bool internetTicked = android.permissions.count("internet") > 0;
    bool internet = internetTicked || android.internetAccess == AndroidInternetAccess::Require;
    if (!internet) {
        // what the build packs and compiles: the Lua and asset trees (HTML UI documents) and the
        // C++ scripts, as they are in the export folder (a file an earlier export left there
        // ships too); not the generated sources, which call Http themselves
        const fs::path exportRoot = getExportProjectRoot();
        std::vector<fs::path> shipped;
        ExportFeatureScan::collect(exportRoot, exportRoot / "lua", shipped);
        ExportFeatureScan::collect(exportRoot, exportRoot / "assets", shipped);
        ExportFeatureScan::collect(exportRoot, exportRoot / "scripts", shipped);
        const std::vector<ExportFeatureScan::Use> uses = ExportFeatureScan::scanFiles(exportRoot, shipped, {ExportFeatureScan::http()});
        if (!uses.empty()) {
            internet = true;
            Out::info("Android: added INTERNET (Http used in %s%s)", uses[0].file.c_str(),
                      uses[0].files > 1 ? (" and " + std::to_string(uses[0].files - 1) + " more").c_str() : "");
        } else if (const std::string video = videoUrlScene(); !video.empty()) {
            // a Video Player playing an http(s) URL (its media cache downloads it)
            internet = true;
            Out::info("Android: added INTERNET (a Video Player plays a URL in %s)", video.c_str());
        } else if (const std::string script = scriptVideoUrlFile(exportRoot, shipped); !script.empty()) {
            // a script setting a Video Player's url, or an HTML <video> of an http(s) URL
            internet = true;
            Out::info("Android: added INTERNET (a video URL may be played from %s)", script.c_str());
        } else {
            // a native plugin's own networking is invisible to the scan
            std::error_code pluginError;
            if (fs::is_directory(project->getProjectPath() / "plugins" / "native", pluginError)) {
                Out::warning("project.yaml:1: warning: Android export without the INTERNET permission: no Lua or C++ uses Http, "
                             "but the project has native plugins (plugins/native). If they use the network, set "
                             "Project Settings > Android > Internet Access to Require (android.internet_access: require)");
            }
        }
    }

    std::string manifest;
    manifest += "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n";
    manifest += nativeExport.needsToolsNamespace
        ? "<manifest xmlns:android=\"http://schemas.android.com/apk/res/android\"\n    xmlns:tools=\"http://schemas.android.com/tools\">\n\n"
        : "<manifest xmlns:android=\"http://schemas.android.com/apk/res/android\">\n\n";
    bool wrotePermission = false;
    // what the scripts' platform services need, on top of the chosen permissions
    std::set<std::string> permissions = android.permissions;
    const ScriptServiceUse serviceUse = scanScriptServiceUse(webSourceRoots(project));
    if (serviceUse.vibrate && permissions.insert("vibrate").second) {
        Out::info("Android: added the VIBRATE permission (System.vibrate in %s)", serviceUse.vibrateFile.filename().string().c_str());
    }
    // with INTERNET, ACCESS_NETWORK_STATE is written anyway (below)
    if (serviceUse.reachability && !internet && permissions.insert("access_network_state").second) {
        Out::info("Android: added the ACCESS_NETWORK_STATE permission (System.getInternetReachability in %s)",
                  serviceUse.reachabilityFile.filename().string().c_str());
    }
    if (internet) {
        manifest += "    <uses-permission android:name=\"android.permission.INTERNET\" />\n";
        // a normal permission (no prompt): lets Http tell "offline" from a DNS failure
        manifest += "    <uses-permission android:name=\"android.permission.ACCESS_NETWORK_STATE\" />\n";
        wrotePermission = true;
    }
    for (const std::string& permission : permissions) {
        // written above, once
        if (permission == "internet" || (internet && permission == "access_network_state")) continue;
        const char* manifestName = androidPermissionManifestName(permission);
        if (!manifestName) continue;
        manifest += "    <uses-permission android:name=\"android.permission.";
        manifest += manifestName;
        manifest += "\" />\n";
        wrotePermission = true;
    }
    // Project Settings > Native Plugins > Android: permissions by full name, then raw XML
    if (!nativeExport.permissionsXml.empty()) {
        manifest += nativeExport.permissionsXml;
        wrotePermission = true;
    }
    if (wrotePermission) manifest += "\n";
    if (!nativeExport.manifestXml.empty()) manifest += nativeExport.manifestXml + "\n";
    manifest += "    <application\n";
    manifest += std::string("        android:allowBackup=\"") + (android.allowBackup ? "true" : "false") + "\"\n";
    manifest += "        android:icon=\"" + iconReference + "\"\n";
    // plain http:// (cleartext): Project Settings > Player > Allow Insecure HTTP; localhost always
    manifest += "        android:networkSecurityConfig=\"@xml/network_security_config\"\n";
    manifest += "        android:label=\"@string/app_name\">\n\n";
    manifest += "        <activity android:name=\".MainActivity\"\n";
    manifest += "            android:label=\"@string/app_name\"\n";
    // rotations, locale and display changes reach the running game, which never restarts
    manifest += "            android:configChanges=\"mcc|mnc|locale|touchscreen|keyboard|keyboardHidden|navigation|orientation|"
                "screenLayout|uiMode|screenSize|smallestScreenSize|fontScale|layoutDirection|density\"\n";
    manifest += "            android:theme=\"@style/AppTheme\"\n";
    // Player > Resolution and Presentation: the default orientation, or auto-rotation within the
    // allowed ones (the user's rotation lock kept); scripts change it with Screen.orientation
    {
        const PresentationSettings& presentation = project->getPresentationSettings();
        bool unsupported = false;
        const int requested = PlatformRules::androidRequestedOrientation(presentation.defaultOrientation,
            presentation.effectiveAllowedOrientations(), &unsupported);
        if (unsupported) {
            Out::warning("Android cannot restrict auto-rotation to the allowed orientations of Player settings; "
                         "the export allows all four (fullUser)");
        }
        manifest += std::string("            android:screenOrientation=\"") + PlatformRules::androidManifestOrientation(requested) + "\"\n";
    }
    manifest += "            android:exported=\"true\">\n";
    manifest += "            <meta-data android:name=\"android.app.lib_name\" android:value=\"jasperengine-android\" />\n";
    manifest += "            <intent-filter>\n";
    manifest += "                <action android:name=\"android.intent.action.MAIN\" />\n";
    manifest += "                <category android:name=\"android.intent.category.LAUNCHER\" />\n";
    manifest += "            </intent-filter>\n";
    manifest += "        </activity>\n";
    if (!nativeExport.applicationXml.empty()) manifest += "\n" + nativeExport.applicationXml;
    manifest += "    </application>\n\n";
    manifest += "    <uses-feature android:glEsVersion=\"0x00030000\" android:required=\"true\" />\n";
    // the mobile family's ASTC (S14 R11); with the ETC2 fallback devices without it load
    // android_etc2 instead, so the store filters none out
    if (!project->getTextureSettings().androidEtc2Fallback) {
        manifest += "    <supports-gl-texture android:name=\"GL_KHR_texture_compression_astc_ldr\" />\n";
    }
    manifest += "\n";
    manifest += "</manifest>";
    if (!writeOrFail(manifestPath, manifest)) return false;

    // The cleartext policy Http enforces itself (player.allow_insecure_http), declared to Android
    // too: localhost / 127.0.0.1 always, other hosts only when insecure HTTP is allowed
    {
        const bool cleartext = project->getApplicationSettings().allowInsecureHttp;
        std::string config;
        config += "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n";
        config += "<!-- Written by the export from Project Settings > Player > Allow Insecure HTTP -->\n";
        config += "<network-security-config>\n";
        config += std::string("    <base-config cleartextTrafficPermitted=\"") + (cleartext ? "true" : "false") + "\"/>\n";
        config += "    <domain-config cleartextTrafficPermitted=\"true\">\n";
        config += "        <domain includeSubdomains=\"false\">localhost</domain>\n";
        config += "        <domain includeSubdomains=\"false\">127.0.0.1</domain>\n";
        config += "        <domain includeSubdomains=\"false\">::1</domain>\n";
        config += "    </domain-config>\n";
        config += "</network-security-config>\n";
        const fs::path xmlDir = androidAppDir / "src" / "main" / "res" / "xml";
        std::error_code xmlError;
        fs::create_directories(xmlDir, xmlError);
        if (!writeOrFail(xmlDir / "network_security_config.xml", config)) return false;
    }

    std::string strings;
    strings += "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n\n";
    strings += "<resources>\n";
    strings += "    <string name=\"app_name\" formatted=\"false\">" + escapeAndroidStringResource(appName) + "</string>\n";
    strings += "</resources>";
    if (!writeOrFail(stringsPath, strings)) return false;

    std::string styles;
    styles += "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n\n";
    styles += "<resources xmlns:android=\"http://schemas.android.com/apk/res/android\">\n";
    styles += "    <style name=\"AppTheme\" parent=\"Theme.AppCompat.Light.NoActionBar\">\n";
    styles += std::string("        <item name=\"android:windowFullscreen\">") + (android.fullscreen ? "true" : "false") + "</item>\n";
    styles += "    </style>\n";
    styles += "</resources>";
    if (!writeOrFail(stylesPath, styles)) return false;

    std::string activity;
    if (!readText(mainActivityPath, activity)) return false;
    if (!android.fullscreen && (activity.find("WindowCompat.setDecorFitsSystemWindows(getWindow(), false);") == std::string::npos
            || activity.find("\t\thideSystemUI();\n") == std::string::npos)) {
        setError("Incompatible Android Activity export template");
        return false;
    }
    if (!android.fullscreen) {
        replaceAll(activity,
            "\t\t// When true, the app will fit inside any system UI windows.\n"
            "\t\t// When false, we render behind any system UI windows.\n"
            "\t\tWindowCompat.setDecorFitsSystemWindows(getWindow(), false);\n"
            "\t\thideSystemUI();",
            "\t\t// When true, the app will fit inside any system UI windows.\n"
            "\t\t// When false, we render behind any system UI windows.\n"
            "\t\tWindowCompat.setDecorFitsSystemWindows(getWindow(), true);");
        replaceAll(activity, "\t\thideSystemUI();\n", "");
        // the native side reads it for Screen.safeArea: a windowed game keeps clear of the bars too
        replaceAll(activity, "public boolean isFullscreenGame() {\n\t\treturn true;", "public boolean isFullscreenGame() {\n\t\treturn false;");
    }
    // the screen stays on through Screen.sleepTimeout (Player > Keep Screen On), not a patch here
    if (!writeOrFail(mainActivityPath, activity)) return false;

    return true;
}

bool editor::Exporter::applyXcodeCxxStandard(std::string& pbxproj, int standard) {
    const std::string key = "CLANG_CXX_LANGUAGE_STANDARD = ";
    const std::string value = "\"gnu++" + std::to_string(standard) + "\"";
    bool found = false;
    size_t pos = 0;
    while ((pos = pbxproj.find(key, pos)) != std::string::npos) {
        const size_t start = pos + key.size();
        const size_t end = pbxproj.find_first_of(";\n", start);
        if (end == std::string::npos || pbxproj[end] != ';') {
            pos = start;
            continue;
        }
        pbxproj.replace(start, end - start, value);
        pos = start + value.size();
        found = true;
    }
    return found;
}

bool editor::Exporter::writeAppleProjectSettings() {
    const fs::path xcodeDir = config.targetDir / "workspaces" / "xcode";
    const fs::path iosPlistPath = xcodeDir / "ios" / "Info.plist";
    const fs::path macOSPlistPath = xcodeDir / "macos" / "Info.plist";
    const fs::path appIconSetDir = xcodeDir / "Assets.xcassets" / "AppIcon.appiconset";

    auto readText = [&](const fs::path& path, std::string& out) -> bool {
        std::error_code ec;
        if (!fs::exists(path, ec)) {
            out.clear();
            return true;
        }
        std::ifstream ifs(path, std::ios::binary);
        if (!ifs) {
            setError("Failed to read Apple export file: " + path.string());
            return false;
        }
        out.assign(std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>());
        normalizeNewlines(out);
        return true;
    };

    auto writeAppleIconPng = [&](const fs::path& sourcePath, const fs::path& outputPath, int size) -> bool {
        if (sourcePath.empty()) return true;

        fs::path source = resolveProjectFile(project, sourcePath);
        int srcWidth = 0, srcHeight = 0, srcChannels = 0;
        unsigned char* srcPixels = stbi_load(source.string().c_str(), &srcWidth, &srcHeight, &srcChannels, 4);
        if (!srcPixels) {
            setError("Apple icon could not be loaded: " + source.string());
            return false;
        }

        std::vector<unsigned char> pixels((size_t)size * size * 4);
        if (srcWidth == size && srcHeight == size) {
            memcpy(pixels.data(), srcPixels, pixels.size());
        } else {
            stbir_resize_uint8_srgb(srcPixels, srcWidth, srcHeight, 0,
                                    pixels.data(), size, size, 0, STBIR_RGBA);
        }
        stbi_image_free(srcPixels);

        std::error_code ec;
        fs::create_directories(outputPath.parent_path(), ec);
        if (ec) {
            setError("Failed to create Apple icon directory: " + ec.message());
            return false;
        }
        if (!writePng(outputPath, size, size, 4, pixels.data(), size * 4, io::Durability::Cache)) {
            setError("Failed to write Apple icon: " + outputPath.string());
            return false;
        }
        return true;
    };

    const MacOSProjectSettings& macOS = project->getMacOSProjectSettings();
    std::string macOSPlist;
    if (!readText(macOSPlistPath, macOSPlist)) {
        return false;
    }
    if (!macOSPlist.empty()) {
        const std::string appName = project->getApplicationName(macOS.applicationName);
        replacePlistStringValue(macOSPlist, "CFBundleName", appName);
        replacePlistStringValue(macOSPlist, "CFBundleDisplayName", appName);
        replacePlistStringValue(macOSPlist, "CFBundleIdentifier", project->getApplicationIdentifier(macOS.bundleIdentifier));
        replacePlistStringValue(macOSPlist, "CFBundleShortVersionString",
            Project::toAppleVersion(project->getApplicationVersion(macOS.versionName)));
        replacePlistStringValue(macOSPlist, "CFBundleVersion",
            Project::toAppleVersion(project->getApplicationBuild(macOS.buildNumber)));
        replacePlistBoolValue(macOSPlist, "NSHighResolutionCapable", macOS.highDpi);
        // Project Settings > Native Plugins > macOS (macos.plist_entries)
        writeNativePlistEntries(macOSPlist, macOS.plistEntries, std::string());
        writeAppTransportSecurity(macOSPlist, project->getApplicationSettings().allowInsecureHttp);
        if (!writeOrFail(macOSPlistPath, macOSPlist)) return false;
    }

    const IOSProjectSettings& ios = project->getIOSProjectSettings();
    // Keep signing/build settings consistent with the generated Info.plist.
    const fs::path projectFile = xcodeDir / "JasperEngine.xcodeproj" / "project.pbxproj";
    std::string xcodeProject;
    if (!readText(projectFile, xcodeProject)) return false;

    // This workspace builds the engine from its own file list, not engine/CMakeLists.txt,
    // so the physics switches have to be mirrored here too.
    auto removeProjectLinesContaining = [&](const std::string& needle) {
        size_t match = 0;
        while ((match = xcodeProject.find(needle, match)) != std::string::npos) {
            const size_t lineStart = xcodeProject.rfind('\n', match);
            const size_t eraseStart = lineStart == std::string::npos ? 0 : lineStart + 1;
            const size_t lineEnd = xcodeProject.find('\n', match);
            const size_t eraseEnd = lineEnd == std::string::npos ? xcodeProject.size() : lineEnd + 1;
            xcodeProject.erase(eraseStart, eraseEnd - eraseStart);
            match = eraseStart;
        }
    };
    auto removeProjectEntriesWithId = [&](const std::string& id) {
        bool removedObject = false;
        bool removedReference = false;
        size_t match = 0;
        while ((match = xcodeProject.find(id, match)) != std::string::npos) {
            const size_t lineStart = xcodeProject.rfind('\n', match);
            const size_t eraseStart = lineStart == std::string::npos ? 0 : lineStart + 1;
            const size_t lineEnd = xcodeProject.find('\n', match);
            const size_t currentLineEnd = lineEnd == std::string::npos ? xcodeProject.size() : lineEnd;
            const size_t assignment = xcodeProject.find('=', match);
            const bool isObject = assignment < currentLineEnd;

            size_t eraseEnd = lineEnd == std::string::npos ? xcodeProject.size() : lineEnd + 1;
            if (isObject) {
                // These PBXBuildFile/PBXTargetDependency objects have no nested dictionaries.
                const size_t objectStart = xcodeProject.find_first_not_of(" \t\r\n", assignment + 1);
                const size_t objectEnd = xcodeProject.find('}', objectStart);
                const size_t terminator = objectEnd == std::string::npos ? std::string::npos
                    : xcodeProject.find_first_not_of(" \t\r\n", objectEnd + 1);
                if (objectStart == std::string::npos || xcodeProject[objectStart] != '{'
                    || objectEnd == std::string::npos || terminator == std::string::npos
                    || xcodeProject[terminator] != ';'
                    || xcodeProject.find('{', objectStart + 1) < objectEnd) {
                    setError("Apple export template contains an incomplete PBX object: " + id);
                    return false;
                }
                const size_t objectLineEnd = xcodeProject.find('\n', terminator + 1);
                eraseEnd = objectLineEnd == std::string::npos ? xcodeProject.size() : objectLineEnd + 1;
                removedObject = true;
            } else {
                removedReference = true;
            }
            xcodeProject.erase(eraseStart, eraseEnd - eraseStart);
            match = eraseStart;
        }
        if (!removedObject || !removedReference) {
            setError("Apple export template is missing a PBX object or reference: " + id);
            return false;
        }
        return true;
    };

    // Object ids drift whenever the workspace is edited in Xcode, so they are looked
    // up through the names Xcode keeps in the trailing comments.
    auto leadingObjectId = [](const std::string& line) {
        const size_t idStart = line.find_first_not_of(" \t");
        if (idStart == std::string::npos) return std::string();
        const size_t idEnd = line.find(' ', idStart);
        if (idEnd == std::string::npos) return std::string();
        return line.substr(idStart, idEnd - idStart);
    };
    auto collectBackendObjectIds = [&](const std::string& backendTarget) {
        // "<id> /* libjoltphysics-macos.a in Frameworks */ = {isa = PBXBuildFile; ...}"
        const std::string libraryComment = "/* lib" + backendTarget + "-";
        // "target = <id> /* joltphysics-macos */;" inside a PBXTargetDependency object
        const std::string targetComment = "/* " + backendTarget + "-";

        std::vector<std::string> ids;
        std::string pendingDependencyId;
        size_t lineStart = 0;
        while (lineStart < xcodeProject.size()) {
            size_t lineEnd = xcodeProject.find('\n', lineStart);
            if (lineEnd == std::string::npos) lineEnd = xcodeProject.size();
            const std::string line = xcodeProject.substr(lineStart, lineEnd - lineStart);
            lineStart = lineEnd + 1;

            if (line.find(libraryComment) != std::string::npos
                && line.find(".a in Frameworks */ = {isa = PBXBuildFile;") != std::string::npos) {
                const std::string id = leadingObjectId(line);
                if (!id.empty()) ids.push_back(id);
                continue;
            }
            if (line.find("/* PBXTargetDependency */ = {") != std::string::npos) {
                pendingDependencyId = leadingObjectId(line);
                continue;
            }
            if (pendingDependencyId.empty()) continue;
            if (line.find("target = ") != std::string::npos && line.find(targetComment) != std::string::npos) {
                ids.push_back(pendingDependencyId);
                pendingDependencyId.clear();
            } else if (line.find("};") != std::string::npos) {
                pendingDependencyId.clear();
            }
        }
        return ids;
    };
    auto unlinkBackend = [&](const std::string& backendTarget) {
        const std::vector<std::string> ids = collectBackendObjectIds(backendTarget);
        if (ids.empty()) {
            setError("Apple export template has no " + backendTarget + " link or dependency entries to remove");
            return false;
        }
        for (const std::string& id : ids) {
            if (!removeProjectEntriesWithId(id)) return false;
        }
        return true;
    };

    std::vector<std::string> excludedPhysicsSources;
    if (!project->isPhysics2DEnabled()) {
        removeProjectLinesContaining("JASPERENGINE_PHYSICS_2D,");
        excludedPhysicsSources.insert(excludedPhysicsSources.end(), {
            "Rigidbody2D.cpp", "Contact2D.cpp", "Joint2D.cpp", "Manifold2D.cpp"
        });
        if (!unlinkBackend("box2d")) return false;
    }
    if (!project->isPhysics3DEnabled()) {
        removeProjectLinesContaining("JASPERENGINE_PHYSICS_3D,");
        excludedPhysicsSources.insert(excludedPhysicsSources.end(), {
            "Rigidbody.cpp", "CollideShapeResult3D.cpp", "Contact3D.cpp", "Joint3D.cpp"
        });
        if (!unlinkBackend("joltphysics")) return false;
    }

    // Without runtime shader compilation the engine compiles without the compiler
    // (ShaderVariantCompiler reports it unavailable) and does not link it
    if (!project->isRuntimeShadersEnabled()) {
        removeProjectLinesContaining("JASPERENGINE_RUNTIME_SHADERS,");
        if (!unlinkBackend("shadercompiler")) return false;
    }

    // Navigation off (Project Settings > Navigation > Include in Builds): Recast/Detour and the
    // engine's Detour glue stay out, and the engine compiles its navmesh stubs (S19 R42)
    if (!project->getNavigationSettings().includeInBuilds) {
        replaceAll(xcodeProject, "\t\t\t\t\tRMLUI_STATIC_LIB,\n", "\t\t\t\t\t\"JASPERENGINE_NAVIGATION=0\",\n\t\t\t\t\tRMLUI_STATIC_LIB,\n");
        excludedPhysicsSources.insert(excludedPhysicsSources.end(), {
            "NavDetourSupport.cpp",
            "Recast.cpp", "RecastAlloc.cpp", "RecastArea.cpp", "RecastAssert.cpp", "RecastContour.cpp", "RecastFilter.cpp",
            "RecastLayers.cpp", "RecastMesh.cpp", "RecastMeshDetail.cpp", "RecastRasterization.cpp", "RecastRegion.cpp",
            "DetourAlloc.cpp", "DetourAssert.cpp", "DetourCommon.cpp", "DetourNavMesh.cpp", "DetourNavMeshBuilder.cpp",
            "DetourNavMeshQuery.cpp", "DetourNode.cpp", "DetourCrowd.cpp", "DetourLocalBoundary.cpp", "DetourObstacleAvoidance.cpp",
            "DetourPathCorridor.cpp", "DetourPathQueue.cpp", "DetourProximityGrid.cpp", "DetourTileCache.cpp", "DetourTileCacheBuilder.cpp"
        });
    }

    std::string excludedSources;
    for (const std::string& source : excludedPhysicsSources) {
        if (!excludedSources.empty()) excludedSources += " ";
        excludedSources += source;
    }
    if (!excludedSources.empty()) {
        replaceAll(xcodeProject, "EXCLUDED_SOURCE_FILE_NAMES = \"\";",
            "EXCLUDED_SOURCE_FILE_NAMES = \"" + excludedSources + "\";");
    }

    size_t blockStart = 0;
    while ((blockStart = xcodeProject.find("buildSettings = {", blockStart)) != std::string::npos) {
        const size_t blockEnd = xcodeProject.find("};", blockStart);
        if (blockEnd == std::string::npos) break;
        std::string block = xcodeProject.substr(blockStart, blockEnd - blockStart);
        const bool isIOS = block.find("INFOPLIST_FILE = ios/Info.plist;") != std::string::npos;
        const bool isMacOS = block.find("INFOPLIST_FILE = macos/Info.plist;") != std::string::npos;
        if (isIOS || isMacOS) {
            const std::string key = "PRODUCT_BUNDLE_IDENTIFIER = ";
            const size_t start = block.find(key);
            const size_t end = block.find(';', start);
            if (start == std::string::npos || end == std::string::npos) {
                setError("Apple export template is missing PRODUCT_BUNDLE_IDENTIFIER");
                return false;
            }
            block.replace(start + key.size(), end - start - key.size(),
                "\"" + project->getApplicationIdentifier(isIOS ? ios.bundleIdentifier : macOS.bundleIdentifier) + "\"");
            xcodeProject.replace(blockStart, blockEnd - blockStart, block);
        }
        blockStart += block.size() + 2;
    }
    // The workspace compiles the engine and the project's scripts itself, so it needs the
    // project's C++ standard too (CMake builds get it from JASPERENGINE_CXX_STANDARD)
    if (!xcodeProject.empty() && !applyXcodeCxxStandard(xcodeProject, project->getCxxStandard())) {
        setError("Apple export template has no CLANG_CXX_LANGUAGE_STANDARD setting");
        return false;
    }
    XcodeNativeLayout layout;
    layout.nativeRoot = getExportProjectRoot() / NativePlugins::relativeRoot();
    layout.pluginsPath = "$(SRCROOT)/../../project/plugins/native";
    layout.engineHeaders = "$(SRCROOT)/../../platform/apple";
    layout.entitlements = "ios/JasperEngine.entitlements";
    if (!addXcodeProjectSources(xcodeProject) || !writeXcodeBuildSettings(xcodeProject) || !addNativePluginsToXcode(xcodeProject, layout)) return false;
    if (!writeNativeAppleFiles()) return false;
    // the privacy manifests (the template's resources): the engine's, the project's and the plugins'
    for (const char* platform : {"ios", "macos"}) {
        const ApplePrivacy::Composed composed = ApplePrivacy::compose(project->getProjectPath(), project->getApplePrivacy(), platform,
                                                                      iosDeclaresTrackingUsage());
        warnPrivacy(std::string(platform) == "ios" ? "iOS" : "macOS", composed.warnings);
        const fs::path manifest = config.targetDir / "workspaces" / "xcode" / platform / "PrivacyInfo.xcprivacy";
        // unchanged files keep their time, so Xcode does not rebuild; a failed write fails the export naming the file
        if (!FileUtils::writeIfChanged(manifest, ApplePrivacy::toXml(composed.manifest))) {
            setError("Failed to write " + manifest.string());
            return false;
        }
    }
    if (!FileUtils::writeIfChanged(projectFile, xcodeProject)) {
        setError("Failed to write " + projectFile.string());
        return false;
    }
    std::string iosPlist;
    if (!readText(iosPlistPath, iosPlist)) {
        return false;
    }
    if (!iosPlist.empty()) {
        const std::string appName = project->getApplicationName(ios.applicationName);
        replacePlistStringValue(iosPlist, "CFBundleName", appName);
        replacePlistStringValue(iosPlist, "CFBundleDisplayName", appName);
        replacePlistStringValue(iosPlist, "CFBundleIdentifier", project->getApplicationIdentifier(ios.bundleIdentifier));
        replacePlistStringValue(iosPlist, "CFBundleShortVersionString",
            Project::toAppleVersion(project->getApplicationVersion(ios.versionName)));
        replacePlistStringValue(iosPlist, "CFBundleVersion",
            Project::toAppleVersion(project->getApplicationBuild(ios.buildNumber)));
        replacePlistBoolValue(iosPlist, "UIStatusBarHidden", ios.hideStatusBar);
        // Player > Resolution and Presentation: the allowed set, the default first (a fixed
        // default alone); iPad multitasking ignores orientation locks unless the app is full screen
        {
            const PresentationSettings& presentation = project->getPresentationSettings();
            const uint8_t allowed = presentation.effectiveAllowedOrientations();
            const std::vector<std::string> orientations =
                PlatformRules::iosPlistOrientations(presentation.defaultOrientation, allowed);
            replacePlistStringArray(iosPlist, "UISupportedInterfaceOrientations", orientations);
            replacePlistStringArray(iosPlist, "UISupportedInterfaceOrientations~ipad", orientations);
            const bool allFour = !PlatformRules::isFixedOrientation(presentation.defaultOrientation) &&
                allowed == PlatformRules::ALL_ORIENTATIONS;
            replacePlistBoolValue(iosPlist, "UIRequiresFullScreen", !allFour);
            if (!allFour) {
                Out::info("iOS: UIRequiresFullScreen is on (iPad Split View would ignore the orientations of Player settings "
                          "unless all four are allowed)");
            }
        }
        replacePlistBoolValue(iosPlist, "CADisableMinimumFrameDurationOnPhone", ios.supportsHighRefreshRate);
        writeAppTransportSecurity(iosPlist, project->getApplicationSettings().allowInsecureHttp);
        // Project Settings > Native Plugins > iOS (ios.plist_entries, ios.plist_xml)
        const std::string plistError = NativePlugins::xmlFragmentError(ios.plistXml);
        if (!plistError.empty()) {
            setError("Project Settings > Native Plugins > iOS > Info.plist XML (ios.plist_xml) is " + plistError);
            return false;
        }
        writeNativePlistEntries(iosPlist, ios.plistEntries, ios.plistXml);
        if (!writeOrFail(iosPlistPath, iosPlist)) return false;
    }

    const fs::path iosViewControllerPath = config.targetDir / "platform" / "apple" / "ios" / "ViewController.m";
    std::string viewController;
    if (!readText(iosViewControllerPath, viewController)) {
        return false;
    }
    if (!viewController.empty()) {
        if (viewController.find("prefersHomeIndicatorAutoHidden") == std::string::npos) {
            const std::string method = std::string("\n- (BOOL)prefersHomeIndicatorAutoHidden\n{\n    return ")
                + (ios.hideHomeIndicator ? "YES" : "NO") + ";\n}\n";
            const size_t endPos = viewController.rfind("@end");
            if (endPos != std::string::npos) {
                viewController.insert(endPos, method);
                if (!writeOrFail(iosViewControllerPath, viewController)) return false;
            }
        } else {
            replaceAll(viewController, "return YES;\n}\n\n@end", std::string("return ") + (ios.hideHomeIndicator ? "YES" : "NO") + ";\n}\n\n@end");
            replaceAll(viewController, "return NO;\n}\n\n@end", std::string("return ") + (ios.hideHomeIndicator ? "YES" : "NO") + ";\n}\n\n@end");
            if (!writeOrFail(iosViewControllerPath, viewController)) return false;
        }
    }

    if (!ios.icon.empty() || !macOS.icon.empty()) {
        struct AppleIconSlot {
            const char* filename;
            const char* idiom;
            const char* platform;
            const char* sizeText;
            const char* scale;
            int pixelSize;
            bool ios;
        };
        const AppleIconSlot slots[] = {
            {"AppIcon-iOS-1024.png", "universal", "ios", "1024x1024", nullptr, 1024, true},
            {"AppIcon-mac-16.png", "mac", nullptr, "16x16", "1x", 16, false},
            {"AppIcon-mac-16@2x.png", "mac", nullptr, "16x16", "2x", 32, false},
            {"AppIcon-mac-32.png", "mac", nullptr, "32x32", "1x", 32, false},
            {"AppIcon-mac-32@2x.png", "mac", nullptr, "32x32", "2x", 64, false},
            {"AppIcon-mac-128.png", "mac", nullptr, "128x128", "1x", 128, false},
            {"AppIcon-mac-128@2x.png", "mac", nullptr, "128x128", "2x", 256, false},
            {"AppIcon-mac-256.png", "mac", nullptr, "256x256", "1x", 256, false},
            {"AppIcon-mac-256@2x.png", "mac", nullptr, "256x256", "2x", 512, false},
            {"AppIcon-mac-512.png", "mac", nullptr, "512x512", "1x", 512, false},
            {"AppIcon-mac-512@2x.png", "mac", nullptr, "512x512", "2x", 1024, false},
        };

        std::string contents;
        contents += "{\n";
        contents += "  \"images\" : [\n";
        bool first = true;
        for (const AppleIconSlot& slot : slots) {
            const fs::path source = slot.ios ? ios.icon : macOS.icon;
            if (!source.empty() && !writeAppleIconPng(source, appIconSetDir / slot.filename, slot.pixelSize)) {
                return false;
            }
            if (!first) contents += ",\n";
            first = false;
            contents += "    {\n";
            if (!source.empty()) contents += "      \"filename\" : \"" + std::string(slot.filename) + "\",\n";
            contents += "      \"idiom\" : \"" + std::string(slot.idiom) + "\",\n";
            if (slot.platform) contents += "      \"platform\" : \"" + std::string(slot.platform) + "\",\n";
            if (slot.scale) contents += "      \"scale\" : \"" + std::string(slot.scale) + "\",\n";
            contents += "      \"size\" : \"" + std::string(slot.sizeText) + "\"\n";
            contents += "    }";
        }
        contents += "\n  ],\n";
        contents += "  \"info\" : {\n";
        contents += "    \"author\" : \"xcode\",\n";
        contents += "    \"version\" : 1\n";
        contents += "  }\n";
        contents += "}\n";
        if (!writeOrFail(appIconSetDir / "Contents.json", contents)) return false;
    }

    return true;
}

bool editor::Exporter::addXcodeProjectSources(std::string& xcodeProject) {
    const fs::path projectRoot = getExportProjectRoot();
    // Two paths can name one file on a case-insensitive file system. Only files of
    // the same size can be the same one, so just those are compared.
    std::map<uintmax_t, std::vector<fs::path>> filesBySize;
    std::vector<std::string> paths;
    for (const fs::path& path : cppExportedPaths) {
        // Headers stay out: Xcode maps every header of a project by name for all targets,
        // so a project Log.h would shadow the engine's in the engine's own build
        if (!isCppSourceFile(path) || isCppHeaderFile(path)) continue;
        const fs::path file = projectRoot / path;
        std::error_code ec;
        std::vector<fs::path>& sameSize = filesBySize[fs::file_size(file, ec)];
        const bool listed = std::any_of(sameSize.begin(), sameSize.end(),
                                        [&](const fs::path& other) { return fs::equivalent(other, file, ec); });
        if (listed) continue;
        sameSize.push_back(file);
        paths.push_back(genericUtf8Path(path));
    }
    std::sort(paths.begin(), paths.end());

    struct AppTarget {
        std::string name;
        std::string sourcesPhaseId;
        std::vector<std::string> configurationIds;
        std::vector<std::string> sources;
    };
    std::vector<AppTarget> targets = {{"JasperEngine iOS"}, {"JasperEngine macOS"}};
    for (AppTarget& target : targets) {
        if (!findXcodeTarget(xcodeProject, target.name, target.sourcesPhaseId, target.configurationIds)) {
            setError("Apple export template has no \"" + target.name + "\" target");
            return false;
        }
    }

    std::set<std::string> usedIds;
    for (size_t start = xcodeProject.find_first_of("0123456789ABCDEF"); start != std::string::npos;) {
        const size_t end = std::min(xcodeProject.find_first_not_of("0123456789ABCDEF", start), xcodeProject.size());
        if (end - start == 24) usedIds.insert(xcodeProject.substr(start, 24));
        start = xcodeProject.find_first_of("0123456789ABCDEF", end);
    }
    // Ids come from the paths, so re-exports match. A taken id is skipped, unless an
    // earlier run of this patch left the same object there.
    auto addObject = [&](const std::string& key, std::string& objects, auto&& define) {
        for (int attempt = 0;; attempt++) {
            const std::string id = xcodeObjectId(attempt == 0 ? key : key + "#" + std::to_string(attempt));
            const std::string object = define(id);
            if (usedIds.insert(id).second) {
                objects += object;
                return id;
            }
            if (xcodeProject.find(object) != std::string::npos) return id;
        }
    };

    auto baseName = [](const std::string& path) { return path.substr(path.rfind('/') + 1); };
    auto parentDir = [](const std::string& path) {
        const size_t slash = path.rfind('/');
        return slash == std::string::npos ? std::string() : path.substr(0, slash);
    };

    // A group per folder, "" being the template's project group
    struct Folder {
        std::set<std::string> folders;
        std::vector<std::string> files;
    };
    std::map<std::string, Folder> folders;
    std::string fileReferences, buildFiles, groups;
    for (const std::string& path : paths) {
        const std::string name = baseName(path);
        const char* type = FileUtils::pathFromUtf8(name).extension() == ".c" ? "sourcecode.c.c" : "sourcecode.cpp.cpp";
        const std::string fileId = addObject("file:" + path, fileReferences, [&](const std::string& id) {
            return "\t\t" + id + " /* " + name + " */ = {isa = PBXFileReference; fileEncoding = 4; lastKnownFileType = "
                + type + "; path = " + pbxString(name) + "; sourceTree = \"<group>\"; };\n";
        });
        for (AppTarget& target : targets) {
            const std::string buildFileId = addObject("build:" + target.name + ":" + path, buildFiles, [&](const std::string& id) {
                return "\t\t" + id + " /* " + name + " in Sources */ = {isa = PBXBuildFile; fileRef = " + fileId
                    + " /* " + name + " */; };\n";
            });
            target.sources.push_back(buildFileId + " /* " + name + " in Sources */");
        }
        folders[parentDir(path)].files.push_back(fileId + " /* " + name + " */");
        for (std::string dir = parentDir(path); !dir.empty(); dir = parentDir(dir)) {
            folders[parentDir(dir)].folders.insert(dir);
        }
    }

    std::map<std::string, std::string> groupIds;
    auto children = [&](const Folder& folder) {
        std::vector<std::string> entries;
        for (const std::string& dir : folder.folders) entries.push_back(groupIds[dir] + " /* " + baseName(dir) + " */");
        entries.insert(entries.end(), folder.files.begin(), folder.files.end());
        return entries;
    };
    // In reverse, subfolders come before their folder and "" comes last
    for (auto it = folders.rbegin(); it != folders.rend() && !it->first.empty(); ++it) {
        const std::string name = baseName(it->first);
        const std::vector<std::string> entries = children(it->second);
        groupIds[it->first] = addObject("group:" + it->first, groups, [&](const std::string& id) {
            std::string group = "\t\t" + id + " /* " + name + " */ = {\n\t\t\tisa = PBXGroup;\n\t\t\tchildren = (\n";
            for (const std::string& entry : entries) group += "\t\t\t\t" + entry + ",\n";
            return group + "\t\t\t);\n\t\t\tpath = " + pbxString(name) + ";\n\t\t\tsourceTree = \"<group>\";\n\t\t};\n";
        });
    }

    auto insertObjects = [&](const std::string& section, const std::string& objects) {
        const size_t sectionEnd = xcodeProject.find("/* End " + section + " section */");
        if (sectionEnd == std::string::npos) {
            setError("Apple export template has no " + section + " section");
            return false;
        }
        xcodeProject.insert(sectionEnd, objects);
        return true;
    };
    if (!insertObjects("PBXBuildFile", buildFiles) || !insertObjects("PBXFileReference", fileReferences)
        || !insertObjects("PBXGroup", groups)) {
        return false;
    }
    const PbxRange projectGroup = findPbxObjectContaining(xcodeProject, "PBXGroup", "\n\t\t\tpath = ../../project;\n");
    if (!addPbxListEntries(xcodeProject, projectGroup, "children", children(folders[""]))) {
        setError("Apple export template has no project group");
        return false;
    }

    // The include directories CMake gives the game target
    std::vector<std::string> includeDirs = {"project", "project/scripts"};
    for (const std::string& root : getExportedScriptRoots()) includeDirs.push_back("project/" + root);
    std::vector<std::string> searchPaths;
    for (const std::string& dir : includeDirs) {
        std::string path = "$(SRCROOT)/../../" + dir;
        // Xcode splits a list item like a shell does. It writes a backslash as is into the
        // response file, where clang reads it as an escape, so backslashes are doubled again.
        if (path.find_first_of(" \t'\"\\") != std::string::npos) {
            std::string quoted = "\"";
            for (char c : path) {
                if (c == '\\') quoted += "\\\\\\\\";
                else if (c == '"') quoted += "\\\"";
                else quoted += c;
            }
            path = quoted + "\"";
        }
        searchPaths.push_back(pbxString(path));
    }
    for (const AppTarget& target : targets) {
        if (!addPbxListEntries(xcodeProject, findPbxObject(xcodeProject, target.sourcesPhaseId), "files", target.sources)) {
            setError("Apple export template has no Sources phase in \"" + target.name + "\"");
            return false;
        }
        for (const std::string& configurationId : target.configurationIds) {
            if (!addPbxListEntries(xcodeProject, findPbxBuildSettings(xcodeProject, configurationId), "HEADER_SEARCH_PATHS", searchPaths)) {
                setError("Apple export template has no HEADER_SEARCH_PATHS in \"" + target.name + "\"");
                return false;
            }
        }
    }
    return true;
}

// ---- native plugins on Apple (feature native-bridge): plugins/native/{apple,ios,macos} sources,
// SDK frameworks and libraries, Swift, system frameworks, Swift packages, deployment target

namespace {

    std::string lowerExtension(const fs::path& path) {
        std::string ext = path.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return ext;
    }

    // Sets the scalar build setting `key` in a buildSettings range (added when missing)
    void setPbxBuildSetting(std::string& pbx, const PbxRange& settings, const std::string& key, const std::string& value) {
        if (!settings.found()) return;
        const std::string line = "\t\t\t\t" + key + " = ";
        const size_t at = pbx.find(line, settings.start);
        if (at < settings.end) {
            const size_t valueStart = at + line.size();
            pbx.replace(valueStart, pbx.find(";\n", valueStart) - valueStart, value);
            return;
        }
        const size_t insertAt = pbx.find('\n', settings.start) + 1;
        pbx.insert(insertAt, line + value + ";\n");
    }

    // Adds `entries` to the list setting `key` (made, keeping a scalar value it had, when it is not a list yet)
    void addPbxBuildSettingList(std::string& pbx, const PbxRange& settings, const std::string& key, const std::vector<std::string>& entries) {
        if (!settings.found() || entries.empty()) return;
        if (addPbxListEntries(pbx, settings, key, entries)) return;
        const std::string line = "\t\t\t\t" + key + " = ";
        std::vector<std::string> values = {"\"$(inherited)\""};
        const size_t at = pbx.find(line, settings.start);
        if (at < settings.end) {
            const size_t valueStart = at + line.size();
            const size_t valueEnd = pbx.find(";\n", valueStart);
            const std::string old = pbx.substr(valueStart, valueEnd - valueStart);
            if (old != "\"$(inherited)\"") values.push_back(old);
            pbx.erase(at, valueEnd + 2 - at);
        }
        for (const std::string& entry : entries) {
            if (std::find(values.begin(), values.end(), entry) == values.end()) values.push_back(entry);
        }
        std::string list = line + "(\n";
        for (const std::string& value : values) list += "\t\t\t\t\t" + value + ",\n";
        list += "\t\t\t\t);\n";
        pbx.insert(pbx.find('\n', settings.start) + 1, list);
    }

    // Inserts `lines` (an object's key lines) right after its "isa = ...;" line
    bool addPbxObjectKey(std::string& pbx, const PbxRange& object, const std::string& key, const std::string& lines) {
        if (!object.found()) return false;
        if (pbx.find("\t\t\t" + key + " = ", object.start) < object.end) return true;
        const size_t isa = pbx.find("\t\t\tisa = ", object.start);
        if (isa >= object.end) return false;
        pbx.insert(pbx.find('\n', isa) + 1, lines);
        return true;
    }

    std::string pbxPath(const std::string& path) {
        // the same quoting rules as the template's search paths
        return pbxString(path);
    }

}

// The system frameworks the engine's own code calls into, per app target: sokol and the renderer
// (Metal, MetalKit, QuartzCore), miniaudio (AVFoundation's AVAudioSession and AudioToolbox on iOS;
// CoreAudio and AudioToolbox on macOS, also loaded at run time), vibration (AudioToolbox), video
// decoding (AVFoundation, CoreMedia, CoreVideo), haptics and the view (UIKit / AppKit), the
// display (CoreGraphics), gamepads (GameController); the native bridge needs Foundation and
// UIKit / AppKit. What the template's phase links already is kept.
void editor::Exporter::applyNativePlistEntries(std::string& plist, const std::map<std::string, NativePlistValue>& entries,
                                               const std::string& rawXml) {
    writeNativePlistEntries(plist, entries, rawXml);
}

const std::vector<std::string>& editor::Exporter::appleEngineFrameworks(bool ios) {
    static const std::vector<std::string> iosFrameworks = {"AVFoundation", "AudioToolbox", "CoreGraphics", "CoreMedia", "CoreVideo", "Foundation",
                                                           "GameController", "Metal", "MetalKit", "QuartzCore", "UIKit"};
    static const std::vector<std::string> macosFrameworks = {"AVFoundation", "AppKit", "AudioToolbox", "CoreAudio", "CoreGraphics", "CoreMedia",
                                                             "CoreVideo", "Foundation", "GameController", "Metal", "MetalKit", "QuartzCore"};
    return ios ? iosFrameworks : macosFrameworks;
}

bool editor::Exporter::addNativePluginsToXcode(std::string& xcodeProject, const XcodeNativeLayout& layout) {
    const IOSProjectSettings& ios = project->getIOSProjectSettings();
    const MacOSProjectSettings& macOS = project->getMacOSProjectSettings();
    const fs::path& nativeRoot = layout.nativeRoot;
    std::error_code ec;

    // the files of each Apple folder, relative to plugins/native, sorted; bundles are one entry
    std::map<std::string, std::vector<std::string>> folderFiles;
    for (const char* folder : {"apple", "ios", "macos"}) {
        if (layout.player && std::string(folder) == "macos") continue;
        const fs::path dir = nativeRoot / folder;
        if (!fs::is_directory(dir, ec)) continue;
        std::vector<std::string>& list = folderFiles[folder];
        for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
             !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            const std::string name = it->path().filename().string();
            const bool directory = it->is_directory(ec);
            if (name.empty() || name[0] == '.' || (directory && name == "build")) {
                if (directory) it.disable_recursion_pending();
                continue;
            }
            const std::string ext = lowerExtension(it->path());
            // an SDK's resource bundle (Foo_Privacy.bundle) is copied into the app whole
            const bool bundle = directory && (ext == ".framework" || ext == ".xcframework" || ext == ".bundle");
            if (bundle) it.disable_recursion_pending();
            if (bundle || (!directory && it->is_regular_file(ec))) {
                list.push_back(std::string(folder) + "/" + genericUtf8Path(it->path().lexically_relative(dir)));
            }
        }
        std::sort(list.begin(), list.end());
    }

    // ios.deployment_target (validated by the settings: at least 15.0)
    {
        std::string target = ios.deploymentTarget;
        const bool valid = std::regex_match(target, std::regex("^[0-9]+\\.[0-9]+$"));
        if (!valid || std::stod(target) < 15.0) {
            if (!target.empty() && target != "15.0") {
                Out::warning("iOS deployment target \"%s\" is not a version of 15.0 or later; the export uses 15.0", target.c_str());
            }
            target = "15.0";
        }
        if (target != "15.0") replaceAll(xcodeProject, "IPHONEOS_DEPLOYMENT_TARGET = 15.0;", "IPHONEOS_DEPLOYMENT_TARGET = " + target + ";");
    }


    struct AppTarget {
        std::string name;
        std::string sourcesPhaseId, frameworksPhaseId, resourcesPhaseId;
        std::vector<std::string> configurationIds;
        std::vector<std::string> folders;         // its plugin folders
        const std::vector<std::string>* systemFrameworks = nullptr;
        std::vector<std::string> sources, frameworks, embedded, resources;
        std::set<std::string> frameworkDirs, libraryDirs, headerDirs;
        bool swift = false;
        bool ios = true;
        std::string runpath;
        std::string bridgingHeader;
    };
    // the source export's two app targets, or the prebuilt iOS template's one (the iOS target first)
    std::vector<AppTarget> targets(layout.player ? 1 : 2);
    targets[0].name = layout.player ? "JasperEnginePlayer" : "JasperEngine iOS";
    targets[0].folders = {"apple", "ios"};
    targets[0].systemFrameworks = &ios.frameworks;
    targets[0].runpath = "\"@executable_path/Frameworks\"";
    targets[0].bridgingHeader = "JasperNative-Bridging-Header-iOS.h";
    if (!layout.player) {
        targets[1].name = "JasperEngine macOS";
        targets[1].ios = false;
        targets[1].folders = {"apple", "macos"};
        targets[1].systemFrameworks = &macOS.frameworks;
        targets[1].runpath = "\"@executable_path/../Frameworks\"";
        targets[1].bridgingHeader = "JasperNative-Bridging-Header-macOS.h";
    }
    for (AppTarget& target : targets) {
        if (!findXcodeTarget(xcodeProject, target.name, target.sourcesPhaseId, target.configurationIds)) {
            setError("Apple export template has no \"" + target.name + "\" target");
            return false;
        }
        const PbxRange object = findPbxObjectContaining(xcodeProject, "PBXNativeTarget", "\n\t\t\tname = " + pbxString(target.name) + ";\n");
        for (const std::string& phaseId : pbxListIds(xcodeProject, object, "buildPhases")) {
            const PbxRange phase = findPbxObject(xcodeProject, phaseId);
            if (phase.found() && xcodeProject.find("\t\t\tisa = PBXFrameworksBuildPhase;\n", phase.start) < phase.end) target.frameworksPhaseId = phaseId;
            if (phase.found() && xcodeProject.find("\t\t\tisa = PBXResourcesBuildPhase;\n", phase.start) < phase.end) target.resourcesPhaseId = phaseId;
        }
        if (target.frameworksPhaseId.empty()) {
            setError("Apple export template has no Frameworks phase in \"" + target.name + "\"");
            return false;
        }
    }

    std::set<std::string> usedIds;
    for (size_t start = xcodeProject.find_first_of("0123456789ABCDEF"); start != std::string::npos;) {
        const size_t end = std::min(xcodeProject.find_first_not_of("0123456789ABCDEF", start), xcodeProject.size());
        if (end - start == 24) usedIds.insert(xcodeProject.substr(start, 24));
        start = xcodeProject.find_first_of("0123456789ABCDEF", end);
    }
    // ids from the paths (re-exports match)
    auto addObject = [&](const std::string& key, std::string& objects, auto&& define) {
        for (int attempt = 0;; attempt++) {
            const std::string id = xcodeObjectId("native:" + (attempt == 0 ? key : key + "#" + std::to_string(attempt)));
            const std::string object = define(id);
            if (usedIds.insert(id).second) {
                objects += object;
                return id;
            }
            if (xcodeProject.find(object) != std::string::npos) return id;
        }
    };
    auto baseName = [](const std::string& path) { return path.substr(path.rfind('/') + 1); };
    auto parentDir = [](const std::string& path) {
        const size_t slash = path.rfind('/');
        return slash == std::string::npos ? std::string() : path.substr(0, slash);
    };

    std::string fileReferences, buildFiles, groups, copyPhases, packageObjects;
    // a group per folder below plugins/native ("" is plugins/native itself)
    std::map<std::string, std::pair<std::set<std::string>, std::vector<std::string>>> tree;
    auto addToTree = [&](const std::string& path, const std::string& entry) {
        tree[parentDir(path)].second.push_back(entry);
        for (std::string dir = parentDir(path); !dir.empty(); dir = parentDir(dir)) tree[parentDir(dir)].first.insert(dir);
    };

    for (const auto& [folder, files] : folderFiles) {
        for (const std::string& path : files) {
            const std::string name = baseName(path);
            const std::string ext = lowerExtension(FileUtils::pathFromUtf8(name));
            std::string type;
            bool source = true, linked = false, resource = false;
            if (ext == ".m") type = "sourcecode.c.objc";
            else if (ext == ".mm") type = "sourcecode.cpp.objcpp";
            else if (ext == ".swift") type = "sourcecode.swift";
            else if (ext == ".c") type = "sourcecode.c.c";
            else if (ext == ".cpp" || ext == ".cc" || ext == ".cxx") type = "sourcecode.cpp.cpp";
            else if (ext == ".h" || ext == ".hpp") {
                // no file reference: Xcode maps a project's headers by name for every target, so a
                // plugin's Camera.h would shadow the engine's. Their folders are search paths, after
                // the engine's.
                for (AppTarget& target : targets) {
                    if (std::find(target.folders.begin(), target.folders.end(), folder) != target.folders.end()) {
                        target.headerDirs.insert(pbxPath(layout.pluginsPath + "/" + parentDir(path)));
                    }
                }
                continue;
            }
            else if (ext == ".framework") { type = "wrapper.framework"; source = false; linked = true; }
            else if (ext == ".xcframework") { type = "wrapper.xcframework"; source = false; linked = true; }
            else if (ext == ".a") { type = "archive.ar"; source = false; linked = true; }
            else if (ext == ".bundle") { type = "wrapper.plug-in"; source = false; resource = true; }
            else continue;
            const std::string fileId = addObject("file:" + path, fileReferences, [&](const std::string& id) {
                return "\t\t" + id + " /* " + name + " */ = {isa = PBXFileReference; " + (linked || resource ? "" : "fileEncoding = 4; ") +
                    "lastKnownFileType = " + type + "; path = " + pbxString(name) + "; sourceTree = \"<group>\"; };\n";
            });
            addToTree(path, fileId + " /* " + name + " */");
            // a dynamic SDK is embedded too (a static one is only linked)
            bool embed = false;
            if (linked && ext != ".a") {
                const fs::path bundle = nativeRoot / FileUtils::pathFromUtf8(path);
                const fs::path binary = ext == ".framework" ? NativePlugins::frameworkBinary(bundle) : NativePlugins::xcframeworkBinary(bundle);
                embed = !binary.empty() && !NativePlugins::isStaticArchive(binary);
            }
            for (AppTarget& target : targets) {
                if (std::find(target.folders.begin(), target.folders.end(), folder) == target.folders.end()) continue;
                const std::string phase = resource ? "Resources" : source ? "Sources" : "Frameworks";
                const std::string buildId = addObject("build:" + target.name + ":" + path, buildFiles, [&](const std::string& id) {
                    return "\t\t" + id + " /* " + name + " in " + phase + " */ = {isa = PBXBuildFile; fileRef = " + fileId + " /* " + name + " */; };\n";
                });
                if (resource) {
                    target.resources.push_back(buildId + " /* " + name + " in Resources */");
                    continue;
                }
                if (source) {
                    target.sources.push_back(buildId + " /* " + name + " in Sources */");
                    if (ext == ".swift") target.swift = true;
                    continue;
                }
                target.frameworks.push_back(buildId + " /* " + name + " in Frameworks */");
                const std::string dir = "\"" + layout.pluginsPath + "/" + parentDir(path) + "\"";
                if (ext == ".a") target.libraryDirs.insert(dir);
                else target.frameworkDirs.insert(dir);
                if (embed) {
                    const std::string embedId = addObject("embed:" + target.name + ":" + path, buildFiles, [&](const std::string& id) {
                        return "\t\t" + id + " /* " + name + " in Embed Frameworks */ = {isa = PBXBuildFile; fileRef = " + fileId + " /* " + name +
                            " */; settings = {ATTRIBUTES = (CodeSignOnCopy, RemoveHeadersOnCopy, ); }; };\n";
                    });
                    target.embedded.push_back(embedId + " /* " + name + " in Embed Frameworks */");
                }
            }
        }
    }

    // system frameworks: the engine's own (linked explicitly: its C++ and Objective-C++ files
    // import no modules, so clang autolinks nothing for them; miniaudio's AVAudioSession symbols
    // once came only through an ads SDK's module, and an iOS export without it failed to link),
    // then ios.frameworks / macos.frameworks: "StoreKit", "AppTrackingTransparency?" (weak). One
    // entry per name (strong wins), none for what the template's phase links already.
    std::vector<std::string> systemFrameworkRefs;
    for (AppTarget& target : targets) {
        std::vector<std::pair<std::string, bool>> wanted;
        if (layout.engineFrameworks) {
            for (const std::string& framework : appleEngineFrameworks(target.ios)) wanted.push_back({framework, false});
        }
        for (std::string framework : *target.systemFrameworks) {
            framework.erase(0, framework.find_first_not_of(" \t"));
            framework.erase(framework.find_last_not_of(" \t") + 1);
            const bool weak = !framework.empty() && framework.back() == '?';
            if (weak) framework.pop_back();
            if (framework.size() > 10 && framework.compare(framework.size() - 10, 10, ".framework") == 0) framework.resize(framework.size() - 10);
            if (framework.empty() || framework.find_first_of("/\"\\ ") != std::string::npos) continue;
            auto same = std::find_if(wanted.begin(), wanted.end(), [&](const auto& w) { return w.first == framework; });
            if (same == wanted.end()) wanted.push_back({framework, weak});
            else same->second = same->second && weak;
        }
        const PbxRange phase = findPbxObject(xcodeProject, target.frameworksPhaseId);
        const std::string phaseText = phase.found() ? xcodeProject.substr(phase.start, phase.end - phase.start) : std::string();
        for (const auto& [framework, weak] : wanted) {
            const std::string name = framework + ".framework";
            if (phaseText.find("/* " + name + " in Frameworks */") != std::string::npos) continue;
            const std::string sdk = target.ios ? "ios" : "macos";
            const std::string fileId = addObject("system:" + sdk + ":" + name, fileReferences, [&](const std::string& id) {
                return "\t\t" + id + " /* " + name + " */ = {isa = PBXFileReference; lastKnownFileType = wrapper.framework; name = " + pbxString(name) +
                    "; path = " + pbxString("System/Library/Frameworks/" + name) + "; sourceTree = SDKROOT; };\n";
            });
            systemFrameworkRefs.push_back(fileId + " /* " + name + " */");
            const std::string buildId = addObject("build:" + target.name + ":system:" + name, buildFiles, [&](const std::string& id) {
                return "\t\t" + id + " /* " + name + " in Frameworks */ = {isa = PBXBuildFile; fileRef = " + fileId + " /* " + name + " */;" +
                    (weak ? " settings = {ATTRIBUTES = (Weak, ); };" : "") + " };\n";
            });
            target.frameworks.push_back(buildId + " /* " + name + " in Frameworks */");
        }
    }

    // Swift packages: one reference each, a product dependency per product, linked into iOS
    std::vector<std::string> packageRefs, productDeps;
    for (const NativeSwiftPackage& package : ios.swiftPackages) {
        if (package.url.empty()) continue;
        std::string repo = package.url;
        while (!repo.empty() && repo.back() == '/') repo.pop_back();
        std::string label = repo.substr(repo.rfind('/') + 1);
        if (label.size() > 4 && label.compare(label.size() - 4, 4, ".git") == 0) label.resize(label.size() - 4);
        // Xcode wants x.y.z: "4" and "4.1" are padded
        std::string version = package.version.empty() ? "1.0.0" : package.version;
        {
            const size_t suffix = version.find_first_of("-+");
            std::string numbers = version.substr(0, suffix);
            const std::string rest = suffix == std::string::npos ? std::string() : version.substr(suffix);
            while (std::count(numbers.begin(), numbers.end(), '.') < 2) numbers += ".0";
            version = numbers + rest;
        }
        const std::string refComment = "XCRemoteSwiftPackageReference \"" + label + "\"";
        const std::string refId = addObject("package:" + package.url, packageObjects, [&](const std::string& id) {
            return "\t\t" + id + " /* " + refComment + " */ = {\n\t\t\tisa = XCRemoteSwiftPackageReference;\n\t\t\trepositoryURL = " +
                pbxString(package.url) + ";\n\t\t\trequirement = {\n\t\t\t\tkind = upToNextMajorVersion;\n\t\t\t\tminimumVersion = " +
                pbxString(version) + ";\n\t\t\t};\n\t\t};\n";
        });
        packageRefs.push_back(refId + " /* " + refComment + " */");
        for (const std::string& product : package.products) {
            if (product.empty()) continue;
            const std::string productId = addObject("product:" + package.url + ":" + product, packageObjects, [&](const std::string& id) {
                return "\t\t" + id + " /* " + product + " */ = {\n\t\t\tisa = XCSwiftPackageProductDependency;\n\t\t\tpackage = " + refId +
                    " /* " + refComment + " */;\n\t\t\tproductName = " + pbxString(product) + ";\n\t\t};\n";
            });
            productDeps.push_back(productId + " /* " + product + " */");
            const std::string buildId = addObject("build:JasperEngine iOS:product:" + package.url + ":" + product, buildFiles, [&](const std::string& id) {
                return "\t\t" + id + " /* " + product + " in Frameworks */ = {isa = PBXBuildFile; productRef = " + productId + " /* " + product + " */; };\n";
            });
            targets[0].frameworks.push_back(buildId + " /* " + product + " in Frameworks */");
        }
    }

    // the groups: plugins > native > apple / ios / macos > ...
    std::map<std::string, std::string> groupIds;
    for (auto it = tree.rbegin(); it != tree.rend(); ++it) {
        if (it->first.empty()) continue;
        std::vector<std::string> entries;
        for (const std::string& dir : it->second.first) entries.push_back(groupIds[dir] + " /* " + baseName(dir) + " */");
        entries.insert(entries.end(), it->second.second.begin(), it->second.second.end());
        const std::string name = baseName(it->first);
        groupIds[it->first] = addObject("group:" + it->first, groups, [&](const std::string& id) {
            std::string group = "\t\t" + id + " /* " + name + " */ = {\n\t\t\tisa = PBXGroup;\n\t\t\tchildren = (\n";
            for (const std::string& entry : entries) group += "\t\t\t\t" + entry + ",\n";
            return group + "\t\t\t);\n\t\t\tpath = " + pbxString(name) + ";\n\t\t\tsourceTree = \"<group>\";\n\t\t};\n";
        });
    }
    std::string nativeGroupId;
    if (!tree.empty()) {
        std::vector<std::string> entries;
        for (const std::string& dir : tree[""].first) entries.push_back(groupIds[dir] + " /* " + baseName(dir) + " */");
        nativeGroupId = addObject("group:plugins/native", groups, [&](const std::string& id) {
            std::string group = "\t\t" + id + " /* native plugins */ = {\n\t\t\tisa = PBXGroup;\n\t\t\tchildren = (\n";
            for (const std::string& entry : entries) group += "\t\t\t\t" + entry + ",\n";
            return group + "\t\t\t);\n\t\t\tname = \"native plugins\";\n\t\t\tpath = plugins/native;\n\t\t\tsourceTree = \"<group>\";\n\t\t};\n";
        });
    }

    // the embed phases
    for (AppTarget& target : targets) {
        if (target.embedded.empty()) continue;
        const std::string phaseId = addObject("embedphase:" + target.name, copyPhases, [&](const std::string& id) {
            std::string phase = "\t\t" + id + " /* Embed Frameworks */ = {\n\t\t\tisa = PBXCopyFilesBuildPhase;\n\t\t\tbuildActionMask = 2147483647;\n"
                "\t\t\tdstPath = \"\";\n\t\t\tdstSubfolderSpec = 10;\n\t\t\tfiles = (\n";
            for (const std::string& entry : target.embedded) phase += "\t\t\t\t" + entry + ",\n";
            return phase + "\t\t\t);\n\t\t\tname = \"Embed Frameworks\";\n\t\t\trunOnlyForDeploymentPostprocessing = 0;\n\t\t};\n";
        });
        target.embedded = {phaseId + " /* Embed Frameworks */"};
    }

    auto insertObjects = [&](const std::string& section, const std::string& objects) {
        if (objects.empty()) return true;
        size_t sectionEnd = xcodeProject.find("/* End " + section + " section */");
        if (sectionEnd == std::string::npos) {
            // a section the template does not have yet: before the build configurations
            const size_t before = xcodeProject.find("/* Begin XCBuildConfiguration section */");
            if (before == std::string::npos) {
                setError("Apple export template has no XCBuildConfiguration section");
                return false;
            }
            xcodeProject.insert(before, "/* Begin " + section + " section */\n/* End " + section + " section */\n\n");
            sectionEnd = xcodeProject.find("/* End " + section + " section */");
        }
        xcodeProject.insert(sectionEnd, objects);
        return true;
    };
    std::string remoteRefs, productObjects;
    {
        // the two kinds share one string above: split them by isa for their sections
        size_t pos = 0;
        while (pos < packageObjects.size()) {
            const size_t end = packageObjects.find("\n\t\t};\n", pos) + 6;
            const std::string object = packageObjects.substr(pos, end - pos);
            (object.find("isa = XCRemoteSwiftPackageReference;") != std::string::npos ? remoteRefs : productObjects) += object;
            pos = end;
        }
    }
    if (!insertObjects("PBXBuildFile", buildFiles) || !insertObjects("PBXFileReference", fileReferences) ||
        !insertObjects("PBXGroup", groups) || !insertObjects("PBXCopyFilesBuildPhase", copyPhases) ||
        !insertObjects("XCRemoteSwiftPackageReference", remoteRefs) || !insertObjects("XCSwiftPackageProductDependency", productObjects)) {
        return false;
    }
    if (!nativeGroupId.empty()) {
        // the source export's project group; the template's main group
        const PbxRange projectGroup = layout.player
            ? findPbxObject(xcodeProject, pbxValueId(xcodeProject, findPbxObject(xcodeProject, pbxValueId(xcodeProject, {0, xcodeProject.size()}, "rootObject")),
                                                     "mainGroup"))
            : findPbxObjectContaining(xcodeProject, "PBXGroup", "\n\t\t\tpath = ../../project;\n");
        if (!addPbxListEntries(xcodeProject, projectGroup, "children", {nativeGroupId + " /* native plugins */"})) {
            setError("Apple export template has no project group");
            return false;
        }
    }
    if (!systemFrameworkRefs.empty()) {
        const PbxRange frameworksGroup = findPbxObjectContaining(xcodeProject, "PBXGroup", "\n\t\t\tname = Frameworks;\n");
        if (!addPbxListEntries(xcodeProject, frameworksGroup, "children", systemFrameworkRefs)) {
            setError("Apple export template has no Frameworks group");
            return false;
        }
    }
    if (!packageRefs.empty()) {
        const PbxRange projectObject = findPbxObject(xcodeProject, pbxValueId(xcodeProject, {0, xcodeProject.size()}, "rootObject"));
        if (!addPbxObjectKey(xcodeProject, projectObject, "packageReferences", "\t\t\tpackageReferences = (\n\t\t\t);\n") ||
            !addPbxListEntries(xcodeProject, findPbxObject(xcodeProject, pbxValueId(xcodeProject, {0, xcodeProject.size()}, "rootObject")),
                               "packageReferences", packageRefs)) {
            setError("Apple export template has no project object for the Swift packages");
            return false;
        }
        const PbxRange iosTarget = findPbxObjectContaining(xcodeProject, "PBXNativeTarget", "\n\t\t\tname = " + pbxString(targets[0].name) + ";\n");
        addPbxObjectKey(xcodeProject, iosTarget, "packageProductDependencies", "\t\t\tpackageProductDependencies = (\n\t\t\t);\n");
        addPbxListEntries(xcodeProject, findPbxObjectContaining(xcodeProject, "PBXNativeTarget", "\n\t\t\tname = " + pbxString(targets[0].name) + ";\n"),
                          "packageProductDependencies", productDeps);
    }

    for (AppTarget& target : targets) {
        if (!target.sources.empty() && !addPbxListEntries(xcodeProject, findPbxObject(xcodeProject, target.sourcesPhaseId), "files", target.sources)) {
            setError("Apple export template has no Sources phase in \"" + target.name + "\"");
            return false;
        }
        if (!target.frameworks.empty() && !addPbxListEntries(xcodeProject, findPbxObject(xcodeProject, target.frameworksPhaseId), "files", target.frameworks)) {
            setError("Apple export template has no Frameworks phase in \"" + target.name + "\"");
            return false;
        }
        if (!target.resources.empty() &&
            (target.resourcesPhaseId.empty() || !addPbxListEntries(xcodeProject, findPbxObject(xcodeProject, target.resourcesPhaseId), "files", target.resources))) {
            setError("Apple export template has no Resources phase in \"" + target.name + "\"");
            return false;
        }
        if (!target.embedded.empty()) {
            addPbxListEntries(xcodeProject, findPbxObjectContaining(xcodeProject, "PBXNativeTarget", "\n\t\t\tname = " + pbxString(target.name) + ";\n"),
                              "buildPhases", target.embedded);
        }
        bool hasSources = false;
        for (const std::string& folder : target.folders) hasSources = hasSources || folderFiles.count(folder);
        for (const std::string& configurationId : target.configurationIds) {
            // the settings range moves with every edit: found again each time
            auto settings = [&]() { return findPbxBuildSettings(xcodeProject, configurationId); };
            if (hasSources) {
                // plugin code imports "JasperNative.h"
                addPbxBuildSettingList(xcodeProject, settings(), "HEADER_SEARCH_PATHS", {pbxPath(layout.engineHeaders)});
            }
            addPbxBuildSettingList(xcodeProject, settings(), "HEADER_SEARCH_PATHS", {target.headerDirs.begin(), target.headerDirs.end()});
            addPbxBuildSettingList(xcodeProject, settings(), "FRAMEWORK_SEARCH_PATHS", {target.frameworkDirs.begin(), target.frameworkDirs.end()});
            addPbxBuildSettingList(xcodeProject, settings(), "LIBRARY_SEARCH_PATHS", {target.libraryDirs.begin(), target.libraryDirs.end()});
            if (target.swift) {
                setPbxBuildSetting(xcodeProject, settings(), "SWIFT_VERSION", "5.0");
                setPbxBuildSetting(xcodeProject, settings(), "CLANG_ENABLE_MODULES", "YES");
                setPbxBuildSetting(xcodeProject, settings(), "SWIFT_OBJC_BRIDGING_HEADER",
                                   pbxString(layout.pluginsPath + "/" + target.bridgingHeader));
            }
            if (target.swift || !target.embedded.empty() || (&target == &targets[0] && !productDeps.empty())) {
                addPbxBuildSettingList(xcodeProject, settings(), "LD_RUNPATH_SEARCH_PATHS", {target.runpath});
            }
            if (&target == &targets[0] && ios.entitlementsXml.find_first_not_of(" \t\r\n") != std::string::npos) {
                setPbxBuildSetting(xcodeProject, settings(), "CODE_SIGN_ENTITLEMENTS", pbxString(layout.entitlements));
            }
        }
        // the Swift bridging header: JasperNative.h and every plugin header of the target
        const fs::path bridgingPath = nativeRoot / target.bridgingHeader;
        if (target.swift) {
            std::string header = "// Written by the export: what the Swift native plugins of " + target.name + " see from Objective-C\n";
            header += "#import \"JasperNative.h\"\n";
            for (const std::string& folder : target.folders) {
                auto files = folderFiles.find(folder);
                if (files == folderFiles.end()) continue;
                for (const std::string& path : files->second) {
                    if (lowerExtension(FileUtils::pathFromUtf8(path)) == ".h") header += "#import \"" + path + "\"\n";
                }
            }
            if (!writeOrFail(bridgingPath, header)) return false;
        } else if (fs::exists(bridgingPath, ec) && !removeStale(bridgingPath)) {
            return false;
        }
    }
    return true;
}

bool editor::Exporter::addNativeToIosPlayer(const fs::path& projectDir) {
    const IOSProjectSettings& ios = project->getIOSProjectSettings();
    // the plugins next to the project, as the source export has them next to its workspace
    const fs::path nativeRoot = projectDir / NativePlugins::relativeRoot();
    if (!copyNativePluginFolders(nativeRoot, {"apple", "ios"})) return false;

    const fs::path pbxFile = projectDir / "JasperEnginePlayer.xcodeproj" / "project.pbxproj";
    std::string pbx;
    {
        std::ifstream in(pbxFile, std::ios::binary);
        if (!in) {
            setError("Cannot read the iOS player's " + pbxFile.string());
            return false;
        }
        pbx.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    XcodeNativeLayout layout;
    layout.nativeRoot = nativeRoot;
    layout.pluginsPath = "$(SRCROOT)/plugins/native";
    layout.engineHeaders = "$(SRCROOT)/include";
    layout.entitlements = "JasperEnginePlayer.entitlements";
    layout.player = true;
    layout.engineFrameworks = false;
    if (!addNativePluginsToXcode(pbx, layout)) return false;

    // the privacy manifest's resource: a runtime packaged before privacy manifests lacks it
    if (pbx.find("/* PrivacyInfo.xcprivacy in Resources */") == std::string::npos) {
        const std::string refId = xcodeObjectId("privacy:ref");
        const std::string buildId = xcodeObjectId("privacy:build");
        const PbxRange target = findPbxObjectContaining(pbx, "PBXNativeTarget", "\n\t\t\tname = JasperEnginePlayer;\n");
        std::string resourcesId;
        for (const std::string& phaseId : pbxListIds(pbx, target, "buildPhases")) {
            const PbxRange phase = findPbxObject(pbx, phaseId);
            if (phase.found() && pbx.find("\t\t\tisa = PBXResourcesBuildPhase;\n", phase.start) < phase.end) resourcesId = phaseId;
        }
        const size_t buildFiles = pbx.find("/* End PBXBuildFile section */");
        const size_t fileRefs = pbx.find("/* End PBXFileReference section */");
        if (resourcesId.empty() || buildFiles == std::string::npos || fileRefs == std::string::npos) {
            setError("The iOS player template has no Resources phase for PrivacyInfo.xcprivacy");
            return false;
        }
        pbx.insert(fileRefs, "\t\t" + refId + " /* PrivacyInfo.xcprivacy */ = {isa = PBXFileReference; lastKnownFileType = text.xml; "
                             "path = PrivacyInfo.xcprivacy; sourceTree = \"<group>\"; };\n");
        pbx.insert(pbx.find("/* End PBXBuildFile section */"), "\t\t" + buildId + " /* PrivacyInfo.xcprivacy in Resources */ = {isa = PBXBuildFile; "
                                                                 "fileRef = " + refId + " /* PrivacyInfo.xcprivacy */; };\n");
        const PbxRange mainGroup = findPbxObject(pbx, pbxValueId(pbx, findPbxObject(pbx, pbxValueId(pbx, {0, pbx.size()}, "rootObject")), "mainGroup"));
        addPbxListEntries(pbx, mainGroup, "children", {refId + " /* PrivacyInfo.xcprivacy */"});
        addPbxListEntries(pbx, findPbxObject(pbx, resourcesId), "files", {buildId + " /* PrivacyInfo.xcprivacy in Resources */"});
        warn("W-IOS-PRIVACY-RUNTIME", "the iOS player runtime predates privacy manifests (its template has no PrivacyInfo.xcprivacy): "
                                      "the build adds one; reinstall the runtime");
    }
    if (FileUtils::writeFile(pbxFile, pbx) == FileUtils::WriteResult::Failed) {
        setError("Cannot write " + pbxFile.string());
        return false;
    }
    if (!writeEntitlements(projectDir / "JasperEnginePlayer.entitlements")) return false;

    // Info.plist: Project Settings > Native Plugins > iOS (ios.plist_entries, ios.plist_xml)
    const fs::path plistFile = projectDir / "Info.plist";
    std::string plist;
    {
        std::ifstream in(plistFile, std::ios::binary);
        plist.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    const std::string plistError = NativePlugins::xmlFragmentError(ios.plistXml);
    if (!plistError.empty()) {
        setError("Project Settings > Native Plugins > iOS > Info.plist XML (ios.plist_xml) is " + plistError);
        return false;
    }
    if (!plist.empty()) {
        writeNativePlistEntries(plist, ios.plistEntries, ios.plistXml);
        if (FileUtils::writeFile(plistFile, plist) == FileUtils::WriteResult::Failed) {
            setError("Cannot write " + plistFile.string());
            return false;
        }
    }

    // the privacy manifest: the engine's, the project's and the plugins'
    const ApplePrivacy::Composed composed = ApplePrivacy::compose(project->getProjectPath(), project->getApplePrivacy(), "ios",
                                                                  iosDeclaresTrackingUsage());
    warnPrivacy("iOS", composed.warnings);
    if (FileUtils::writeFile(projectDir / "PrivacyInfo.xcprivacy", ApplePrivacy::toXml(composed.manifest)) == FileUtils::WriteResult::Failed) {
        setError("Cannot write the iOS privacy manifest");
        return false;
    }
    return true;
}

bool editor::Exporter::writeEntitlements(const fs::path& entitlementsPath) {
    const IOSProjectSettings& ios = project->getIOSProjectSettings();
    std::error_code ec;
    const std::string entitlementsError = NativePlugins::xmlFragmentError(ios.entitlementsXml);
    if (!entitlementsError.empty()) {
        setError("Project Settings > Native Plugins > iOS > Entitlements (ios.entitlements_xml) is " + entitlementsError);
        return false;
    }
    if (ios.entitlementsXml.find_first_not_of(" \t\r\n") != std::string::npos) {
        std::string body = ios.entitlementsXml;
        body.erase(std::remove(body.begin(), body.end(), '\r'), body.end());
        if (body.back() != '\n') body += '\n';
        std::string text = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                           "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
                           "<plist version=\"1.0\">\n<dict>\n" + body + "</dict>\n</plist>\n";
        if (!writeOrFail(entitlementsPath, text)) return false;
    } else if (fs::exists(entitlementsPath, ec) && !removeStale(entitlementsPath)) {
        return false;
    }
    return true;
}

bool editor::Exporter::writeNativeAppleFiles() {
    const IOSProjectSettings& ios = project->getIOSProjectSettings();
    const fs::path xcodeDir = config.targetDir / "workspaces" / "xcode";
    std::error_code ec;
    if (!writeEntitlements(xcodeDir / "ios" / "JasperEngine.entitlements")) return false;

    // CocoaPods
    const fs::path podfilePath = xcodeDir / "Podfile";
    const fs::path readmePath = xcodeDir / "README-pods.txt";
    std::vector<NativeCocoaPod> pods;
    for (const NativeCocoaPod& pod : ios.cocoapods) {
        if (!pod.name.empty()) pods.push_back(pod);
    }
    if (pods.empty()) {
        // an earlier export's: a Podfile left there would make the next pod install add the old pods
        return removeStale(podfilePath) && removeStale(readmePath);
    }
    auto rubyString = [](const std::string& value) {
        std::string out = "'";
        for (char c : value) {
            if (c == '\'' || c == '\\') out += '\\';
            out += c;
        }
        return out + "'";
    };
    std::string target = ios.deploymentTarget;
    if (!std::regex_match(target, std::regex("^[0-9]+\\.[0-9]+$")) || std::stod(target) < 15.0) target = "15.0";
    std::string podfile;
    podfile += "# Written by the export from Project Settings > Native Plugins > iOS > CocoaPods (ios.cocoapods).\n";
    podfile += "# In this folder: pod install, then open JasperEngine.xcworkspace (not the .xcodeproj).\n";
    std::set<std::string> sources;
    for (const NativeCocoaPod& pod : pods) {
        if (!pod.source.empty()) sources.insert(pod.source);
    }
    // a custom spec repository hides the trunk unless it is listed too
    if (!sources.empty()) sources.insert("https://cdn.cocoapods.org/");
    for (const std::string& source : sources) podfile += "source " + rubyString(source) + "\n";
    if (!sources.empty()) podfile += "\n";
    podfile += "platform :ios, " + rubyString(target) + "\n";
    podfile += "project 'JasperEngine.xcodeproj'\n\n";
    podfile += "target 'JasperEngine iOS' do\n";
    podfile += "  use_frameworks! :linkage => :static\n";
    for (const NativeCocoaPod& pod : pods) {
        podfile += "  pod " + rubyString(pod.name);
        if (!pod.version.empty()) podfile += ", " + rubyString(pod.version);
        podfile += "\n";
    }
    podfile += "end\n";
    if (!writeOrFail(podfilePath, podfile)) return false;
    if (!writeOrFail(readmePath,
        "This iOS project uses CocoaPods (Project Settings > Native Plugins > iOS > CocoaPods).\n\n"
        "1. Install CocoaPods once: sudo gem install cocoapods (or brew install cocoapods)\n"
        "2. In this folder (workspaces/xcode): pod install\n"
        "3. Open JasperEngine.xcworkspace, not JasperEngine.xcodeproj, and build \"JasperEngine iOS\".\n\n"
        "Run pod install again after every export that changes the pods.\n")) return false;
    Out::info("iOS: %zu CocoaPods in workspaces/xcode/Podfile: run `pod install` there, then open JasperEngine.xcworkspace", pods.size());
    return true;
}

bool editor::Exporter::writeXcodeBuildSettings(std::string& xcodeProject) {
    // (the project's C++ standard is applyXcodeCxxStandard's, on every configuration)
    for (const std::string name : {"jasperengine-ios", "jasperengine-macos", "JasperEngine iOS", "JasperEngine macOS"}) {
        std::string sourcesPhaseId;
        std::vector<std::string> configurationIds;
        if (!findXcodeTarget(xcodeProject, name, sourcesPhaseId, configurationIds)) {
            setError("Apple export template has no \"" + name + "\" target");
            return false;
        }
    }

    std::error_code ec;
    if (fs::exists(getExportProjectRoot() / "ProjectBuild.cmake", ec)) {
        Out::warning("ProjectBuild.cmake applies to CMake builds only; the Xcode workspace ignores it");
    }
    return true;
}

bool editor::Exporter::writeWindowsResourceFile(bool includeIcon) {
    const WindowsProjectSettings& windows = project->getWindowsProjectSettings();
    const std::string productName = project->getApplicationName(windows.productName);
    const std::string companyName = windows.companyName;
    const std::string fileVersionText = project->getApplicationFileVersion(windows.fileVersion);
    const std::string productVersionText = project->getApplicationFileVersion(windows.productVersion);
    const std::array<int, 4> fileVersion = parseWindowsVersion(fileVersionText);
    const std::array<int, 4> productVersion = parseWindowsVersion(productVersionText);

    const fs::path projectRoot = getExportProjectRoot();
    std::error_code ec;
    fs::create_directories(projectRoot, ec);
    if (ec) {
        setError("Failed to create Windows resource directory: " + ec.message());
        return false;
    }

    // written in one atomic replace when f goes out of scope
    AtomicFileStream f(projectRoot / "app_icon.rc", io::Durability::Cache);

    if (includeIcon) {
        f << "1 ICON \"app_icon.ico\"\n\n";
    }

    f << "#include <windows.h>\n\n";
    f << "1 VERSIONINFO\n";
    f << "FILEVERSION " << fileVersion[0] << "," << fileVersion[1] << "," << fileVersion[2] << "," << fileVersion[3] << "\n";
    f << "PRODUCTVERSION " << productVersion[0] << "," << productVersion[1] << "," << productVersion[2] << "," << productVersion[3] << "\n";
    f << "FILEFLAGSMASK 0x3fL\n";
    f << "FILEFLAGS 0x0L\n";
    f << "FILEOS 0x40004L\n";
    f << "FILETYPE 0x1L\n";
    f << "FILESUBTYPE 0x0L\n";
    f << "BEGIN\n";
    f << "    BLOCK \"StringFileInfo\"\n";
    f << "    BEGIN\n";
    f << "        BLOCK \"040904b0\"\n";
    f << "        BEGIN\n";
    if (!companyName.empty()) {
        f << "            VALUE \"CompanyName\", \"" << escapeWindowsRcString(companyName) << "\\0\"\n";
    }
    f << "            VALUE \"FileDescription\", \"" << escapeWindowsRcString(productName) << "\\0\"\n";
    f << "            VALUE \"FileVersion\", \"" << escapeWindowsRcString(fileVersionText) << "\\0\"\n";
    f << "            VALUE \"InternalName\", \"" << escapeWindowsRcString(getAppName()) << "\\0\"\n";
    f << "            VALUE \"OriginalFilename\", \"" << escapeWindowsRcString(getAppName()) << ".exe\\0\"\n";
    f << "            VALUE \"ProductName\", \"" << escapeWindowsRcString(productName) << "\\0\"\n";
    f << "            VALUE \"ProductVersion\", \"" << escapeWindowsRcString(productVersionText) << "\\0\"\n";
    f << "        END\n";
    f << "    END\n";
    f << "    BLOCK \"VarFileInfo\"\n";
    f << "    BEGIN\n";
    f << "        VALUE \"Translation\", 0x409, 1200\n";
    f << "    END\n";
    f << "END\n";

    return true;
}

bool editor::Exporter::writeAppIcon() {
    fs::path iconPath = project->getWindowIcon();
    if (iconPath.is_relative()) {
        iconPath = project->getProjectPath() / iconPath;
    }

    int srcWidth = 0, srcHeight = 0, srcChannels = 0;
    unsigned char* srcPixels = stbi_load(iconPath.string().c_str(), &srcWidth, &srcHeight, &srcChannels, 4);
    if (!srcPixels) {
        Out::warning("Project icon could not be loaded, exporting without icon: %s", iconPath.string().c_str());
        return false;
    }

    const int maxDimension = std::max(srcWidth, srcHeight);

    // RGBA at a given square size, resized from the source (stretch: icons are
    // expected to be square). Gamma-aware so downscaled icons keep their tone.
    auto makeSize = [&](int size) -> std::vector<unsigned char> {
        std::vector<unsigned char> pixels((size_t)size * size * 4);
        if (size == srcWidth && size == srcHeight) {
            memcpy(pixels.data(), srcPixels, pixels.size());
        } else {
            stbir_resize_uint8_srgb(srcPixels, srcWidth, srcHeight, 0,
                                    pixels.data(), size, size, 0, STBIR_RGBA);
        }
        return pixels;
    };

    // Skip sizes larger than the source (upscaling only blurs); keep at least one.
    auto filterSizes = [&](std::initializer_list<int> wanted) {
        std::vector<int> sizes;
        for (int size : wanted) {
            if (size <= maxDimension) sizes.push_back(size);
        }
        if (sizes.empty()) sizes.push_back(*wanted.begin());
        return sizes;
    };

    // --- app_icon.h: RGBA images the window backends set at startup ---
    const std::vector<int> runtimeSizes = filterSizes({16, 32, 64, 128});
    std::string header;
    header += "// Generated by the JasperEngine Editor export from the project icon. Do not edit.\n";
    header += "#ifndef JASPERENGINE_APP_ICON_H\n#define JASPERENGINE_APP_ICON_H\n\n";
    header += "typedef struct JasperEngineAppIconImage { int width; int height; const unsigned char* pixels; } JasperEngineAppIconImage;\n\n";
    for (int size : runtimeSizes) {
        std::vector<unsigned char> pixels = makeSize(size);
        header += "static const unsigned char jasperengine_app_icon_" + std::to_string(size) + "[] = {";
        char buf[8];
        for (size_t i = 0; i < pixels.size(); i++) {
            if (i % 32 == 0) header += "\n";
            snprintf(buf, sizeof(buf), "%u,", pixels[i]);
            header += buf;
        }
        header += "\n};\n\n";
    }
    header += "static const JasperEngineAppIconImage JASPERENGINE_APP_ICON_IMAGES[] = {\n";
    for (int size : runtimeSizes) {
        std::string s = std::to_string(size);
        header += "    {" + s + ", " + s + ", jasperengine_app_icon_" + s + "},\n";
    }
    header += "};\n\n";
    header += "#define JASPERENGINE_APP_ICON_COUNT " + std::to_string(runtimeSizes.size()) + "\n\n";
    header += "#endif // JASPERENGINE_APP_ICON_H\n";

    // --- app_icon.ico: PNG-compressed entries, embedded into the Windows exe ---
    const std::vector<int> icoSizes = filterSizes({16, 32, 48, 256});
    std::string ico;
    std::vector<std::string> icoPngs;
    auto appendPng = [](void* context, void* data, int size) {
        ((std::string*)context)->append((const char*)data, (size_t)size);
    };
    for (int size : icoSizes) {
        std::vector<unsigned char> pixels = makeSize(size);
        std::string png;
        if (!stbi_write_png_to_func(appendPng, &png, size, size, 4, pixels.data(), size * 4)) {
            Out::warning("Failed to encode project icon at %dx%d, exporting without icon", size, size);
            stbi_image_free(srcPixels);
            return false;
        }
        icoPngs.push_back(std::move(png));
    }

    // --- app_icon.png: launcher-size PNG for the Linux .desktop entry ---
    std::string launcherPng;
    {
        const int launcherSize = std::min(maxDimension, 256);
        std::vector<unsigned char> pixels = makeSize(launcherSize);
        if (!stbi_write_png_to_func(appendPng, &launcherPng, launcherSize, launcherSize, 4, pixels.data(), launcherSize * 4)) {
            Out::warning("Failed to encode project icon at %dx%d, exporting without icon", launcherSize, launcherSize);
            stbi_image_free(srcPixels);
            return false;
        }
    }
    stbi_image_free(srcPixels);

    auto appendU16 = [&ico](uint16_t value) {
        ico += (char)(value & 0xFF);
        ico += (char)((value >> 8) & 0xFF);
    };
    auto appendU32 = [&ico](uint32_t value) {
        ico += (char)(value & 0xFF);
        ico += (char)((value >> 8) & 0xFF);
        ico += (char)((value >> 16) & 0xFF);
        ico += (char)((value >> 24) & 0xFF);
    };

    appendU16(0);                            // reserved
    appendU16(1);                            // type: icon
    appendU16((uint16_t)icoSizes.size());    // image count
    uint32_t dataOffset = 6 + 16 * (uint32_t)icoSizes.size();
    for (size_t i = 0; i < icoSizes.size(); i++) {
        int size = icoSizes[i];
        ico += (char)(size >= 256 ? 0 : size); // width (0 = 256)
        ico += (char)(size >= 256 ? 0 : size); // height
        ico += (char)0;                        // palette colors
        ico += (char)0;                        // reserved
        appendU16(1);                          // color planes
        appendU16(32);                         // bits per pixel
        appendU32((uint32_t)icoPngs[i].size());
        appendU32(dataOffset);
        dataOffset += (uint32_t)icoPngs[i].size();
    }
    for (const std::string& png : icoPngs) {
        ico += png;
    }

    const fs::path projectRoot = getExportProjectRoot();
    std::error_code ec;
    fs::create_directories(projectRoot, ec);

    const std::pair<const char*, std::string_view> files[] = {
        {"app_icon.h", header},
        {"app_icon.ico", std::string_view(ico.data(), ico.size())},
        {"app_icon.png", std::string_view(launcherPng.data(), launcherPng.size())},
    };
    for (const auto& [name, bytes] : files) {
        if (FileUtils::writeFile(projectRoot / name, std::string(bytes)) == FileUtils::WriteResult::Failed) {
            Out::warning("Failed to write %s, exporting without icon", name);
            return false;
        }
    }

    return true;
}

bool editor::Exporter::buildAndSaveShaders() {
    setProgress("Building shaders...", 0.6f);

    if (config.selectedShaderKeys.empty()) {
        setError("No shaders selected");
        return false;
    }

    // Drop retired/reserved property bits so keys collected before a feature bit was
    // retired collapse onto their current equivalent (the std::set dedupes them),
    // instead of exporting stale duplicate variants with '?' in their names.
    {
        std::set<ShaderKey> normalizedKeys;
        for (const ShaderKey& key : config.selectedShaderKeys) {
            normalizedKeys.insert(ShaderPool::normalizeKey(key));
        }
        config.selectedShaderKeys = std::move(normalizedKeys);
    }

    // Shader-only generation writes straight into the requested dir
    fs::path shadersDst = project ? config.targetDir / "shaders" : config.targetDir;

    std::error_code ec;
    fs::create_directories(shadersDst, ec);
    if (ec) {
        setError("Failed to create shaders directory: " + ec.message());
        return false;
    }

    shadersDst = fs::absolute(shadersDst, ec);
    if (ec) {
        setError("Failed to resolve shaders directory: " + ec.message());
        return false;
    }

    // Default to glsl410 if no backend was selected
    std::vector<ShaderBackend> backends(config.selectedBackends.begin(), config.selectedBackends.end());
    if (backends.empty()) {
        backends.push_back(ShaderBackend::GLCore);
    }

    int total = (int)config.selectedShaderKeys.size() * backends.size();
    int current = 0;
    // each shader that failed, named with its language and the builder's reason
    std::vector<BuildDiagnostic> failures;
    auto fail = [&](const std::string& shader, const std::string& reason) {
        Out::warning("Failed to build shader %s: %s", shader.c_str(), reason.c_str());
        BuildDiagnostic failure;
        failure.severity = "error";
        failure.code = "shader";
        failure.message = shader + ": " + reason;
        failure.text = failure.message;
        failures.push_back(std::move(failure));
    };
    // D3D11: bytecode where this computer can compile it (Windows), else the HLSL source the game compiles
    bool d3d11Source = false;
    std::map<std::string, std::vector<ShaderHeaderBuilder::HeaderShader>> headerShaders;

    for (const ShaderKey& shaderKey : config.selectedShaderKeys) {
        ShaderType type = ShaderPool::getShaderTypeFromKey(shaderKey);
        uint64_t props = ShaderPool::getPropertiesFromKey(shaderKey);
        uint16_t customId = ShaderPool::getCustomIdFromKey(shaderKey);
        std::string shaderStr = ShaderPool::getShaderStr(type, props, customId);

        for (ShaderBackend backend : backends) {
            // a project's export cooks the High shadow filters ('Shq') only for the backends of
            // platforms with a High tier or above; there the game renders Medium at most
            // (RenderSystem::updateShadowTier)
            if (project && type == ShaderType::MESH && (props & ShaderPool::getShadowsHQProperty()) &&
                    !ShadowTiers::cooksHighFilters(static_cast<int>(backend))) {
                current++;
                continue;
            }
            float shaderProgress = 0.6f + (0.3f * (float)current / (float)std::max(total, 1));
            std::string fmtStr = ShaderPool::getShaderLangStr(backend);
            setProgress("Building shader: " + shaderStr + " (" + fmtStr + ")", shaderProgress);

            try {
                // Synchronous build without cache
                ShaderData resultData = shaderBuilder.buildShaderForExport(shaderKey, project, backend);
                if (backend == ShaderBackend::D3D11 && config.shaderOutputFormat != ShaderOutputFormat::Json &&
                        !ShaderBuilder::compileHlslBytecode(resultData)) {
                    d3d11Source = true;
                }

                std::string basename = shaderStr + "_" + fmtStr;
                std::string err;

                // Persist with the storage key (customId stripped): the runtime assigns
                // its own session-local id, so the embedded key must not depend on it.
                ShaderKey storageKey = ShaderPool::getStorageKey(shaderKey);

                if (config.shaderOutputFormat == ShaderOutputFormat::Header) {
                    std::vector<unsigned char> sdatBytes;
                    if (!ShaderDataSerializer::writeToBytes(sdatBytes, storageKey, resultData, &err)) {
                        fail(basename, "encoding: " + err);
                    } else {
                        headerShaders[fmtStr].push_back({basename, Base64::encode(sdatBytes.data(), sdatBytes.size())});
                    }
                } else if (config.shaderOutputFormat == ShaderOutputFormat::Json) {
                    std::string filename = basename + ".json";
                    fs::path outputPath = shadersDst / filename;
                    if (!ShaderDataSerializer::writeJsonToFile(outputPath.string(), storageKey, resultData, &err)) {
                        fail(filename, "saving: " + err);
                    }
                } else {
                    std::string filename = basename + ".sdat";
                    fs::path outputPath = shadersDst / filename;
                    if (!ShaderDataSerializer::writeToFile(outputPath.string(), storageKey, resultData, &err)) {
                        fail(filename, "saving: " + err);
                    }
                }
            } catch (const std::exception& e) {
                fail(shaderStr + " (" + fmtStr + ")", e.what());
            }

            current++;
        }
    }

    // the warm-up list of what this export built (spec S32 R11): a file next to the shaders, or an
    // entry ("warmup") of each shader header
    std::string warmupList;
    if (project && project->getShaderCreationSettings().includeInBuilds && loadShaderWarmup()) {
        warmupList = ShaderWarmup::filteredString(std::vector<ShaderKey>(config.selectedShaderKeys.begin(), config.selectedShaderKeys.end()));
        if (config.shaderOutputFormat == ShaderOutputFormat::Header) {
            const std::string encoded = Base64::encode(reinterpret_cast<const unsigned char*>(warmupList.data()), warmupList.size());
            for (ShaderBackend backend : backends) {
                headerShaders[ShaderPool::getShaderLangStr(backend)].push_back({"warmup", encoded});
            }
        } else {
            const io::WriteResult written = io::writeFileAtomic(shadersDst / "warmup.json", warmupList + "\n");
            if (!written.ok()) {
                setError("Failed to write the shader warm-up list: " + written.message);
                return false;
            }
        }
    }

    if (config.shaderOutputFormat == ShaderOutputFormat::Header) {
        setProgress("Writing shader headers...", 0.95f);
        for (ShaderBackend backend : backends) {
            const std::string fmtStr = ShaderPool::getShaderLangStr(backend);
            std::string err;
            const auto it = headerShaders.find(fmtStr);
            const std::vector<ShaderHeaderBuilder::HeaderShader> emptyShaders;
            const std::vector<ShaderHeaderBuilder::HeaderShader>& shaders = it == headerShaders.end() ? emptyShaders : it->second;
            if (!ShaderHeaderBuilder::writeShaderHeader(shadersDst / (fmtStr + ".h"), fmtStr, shaders, err)) {
                fail(fmtStr + ".h", "writing the header: " + err);
            }
        }
    }

    if (!failures.empty()) {
        {
            std::lock_guard<std::mutex> lock(progressMutex);
            progress.errorCount = failures.size();
            progress.errors.assign(failures.begin(), failures.begin() + std::min(failures.size(), BuildErrorLog::kMaxKept));
        }
        setError(std::to_string(failures.size()) + " shader(s) failed to build. First: " + failures.front().message);
        return false;
    }
    if (d3d11Source) {
        std::lock_guard<std::mutex> lock(progressMutex);
        result.notes.push_back("D3D11 shaders ship as HLSL source: the game compiles each when first used. Build on Windows to ship bytecode.");
    }

    return true;
}

// ---- third-party notices of the shipped shaders (licenses/)

const char* editor::Exporter::effectLicensePath() {
    return "licenses/shaders.com-LICENSE.txt";
}

const char* editor::Exporter::fluidLicensePath() {
    return "licenses/webgl-fluid-simulation-LICENSE.txt";
}

bool editor::Exporter::shipsFluidComponent() const {
    // ShaderVariantCollector adds the FLUID programs for every FluidSimulationComponent of the
    // exported scenes and their prefabs
    return std::any_of(config.selectedShaderKeys.begin(), config.selectedShaderKeys.end(), [](ShaderKey key) {
        return ShaderPool::getShaderTypeFromKey(key) == ShaderType::FLUID;
    });
}

bool editor::Exporter::shipsFluidEffect() const {
    // the HTML UI `fluid` effect runs the same kernels (effects/lib/fam_fluid_*.glsl): any of its
    // variants (the built-in effect's custom shader "@effects/fluid")
    return std::any_of(config.selectedShaderKeys.begin(), config.selectedShaderKeys.end(), [](ShaderKey key) {
        return ShaderPool::getShaderTypeFromKey(key) == ShaderType::UI_EFFECT &&
               ShaderPool::getCustomShaderName(ShaderPool::getCustomIdFromKey(key)) == "@effects/fluid";
    });
}

bool editor::Exporter::shipsWebGLFluid() const {
    return shipsFluidComponent() || shipsFluidEffect();
}

const char* editor::Exporter::postFxLicensePath() {
    return "licenses/post-fx-LICENSE.txt";
}

const char* editor::Exporter::openEngineLicensePath() {
    return "licenses/OpenEngine-LICENSE.txt";
}

namespace {

    // the notice at `relative` (under the export's root) when `ship`, copied from `source`;
    // else removed (and licenses/ with it when empty)
    bool writeNotice(bool ship, const fs::path& targetDir, const char* relative, const fs::path& source, std::string& error) {
        const fs::path target = targetDir / relative;
        std::error_code ec;
        if (!ship) {
            fs::remove(target, ec);
            if (!ec) fs::remove(target.parent_path(), ec);   // only when empty
            return true;
        }
        if (!fs::is_regular_file(source, ec)) {
            error = "missing: " + source.string();
            return false;
        }
        fs::create_directories(target.parent_path(), ec);
        if (!ec) fs::copy_file(source, target, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            error = "Failed to write " + target.string() + ": " + ec.message();
            return false;
        }
        return true;
    }

}

bool editor::Exporter::writeEffectLicense() {
    // copyEngine put the engine's shaderlib next to the export (a prebuilt build has none: the
    // editor's own)
    const fs::path shaderlib = shaderlibSourceDir();
    const fs::path effects = shaderlib / "effects";
    std::string error;
    // Every UI effect is compiled with the effects include library, which ports functions of
    // the shaders.com kit (engine/shaderlib/effects/LICENSE.shaders-com.txt lists it with the
    // ported effects): a build with any UI effect variant ships the notice
    const bool effectVariants = std::any_of(config.selectedShaderKeys.begin(), config.selectedShaderKeys.end(), [](ShaderKey key) {
        return ShaderPool::getShaderTypeFromKey(key) == ShaderType::UI_EFFECT;
    });
    if (!writeNotice(effectVariants, config.targetDir, effectLicensePath(), effects / "LICENSE.shaders-com.txt", error)) {
        setError("The UI effects notice: " + error);
        return false;
    }
    // Pavel Dobryakov's WebGL-Fluid-Simulation (engine/shaderlib/fluid.*, effects/lib/fam_fluid_*,
    // engine/core/fluid; NOTICE.md)
    if (!writeNotice(shipsWebGLFluid(), config.targetDir, fluidLicensePath(), effects / "LICENSE.webgl-fluid-simulation.txt", error)) {
        setError("The fluid simulation notice: " + error);
        return false;
    }
    // The uber pass's VHS tape and film simulation adapt MIT code (punikonta's VHS shader,
    // OpenEngine's film-simulation package with AMD's FidelityFX Lens grain): a build with such
    // a variant ships their notices (engine/shaderlib/includes/LICENSE.post-fx.txt)
    constexpr uint64_t postFxBits = UBER_VHS | UBER_FILM_DAMAGE | UBER_GRAIN_LENS | UBER_GRAIN_FILMIC;
    const bool postFx = std::any_of(config.selectedShaderKeys.begin(), config.selectedShaderKeys.end(), [](ShaderKey key) {
        return ShaderPool::getShaderTypeFromKey(key) == ShaderType::BLIT && (ShaderPool::getPropertiesFromKey(key) & postFxBits) != 0;
    });
    if (!writeNotice(postFx, config.targetDir, postFxLicensePath(), shaderlib / "includes" / "LICENSE.post-fx.txt", error)) {
        setError("The post FX notice: " + error);
        return false;
    }
    // every game links the spline module (core/spline), adapted from OpenEngine (NOTICE.md)
    if (!writeNotice(true, config.targetDir, openEngineLicensePath(), engineSourceDir() / "core" / "spline" / "LICENSE.OpenEngine.txt", error)) {
        setError("The OpenEngine notice: " + error);
        return false;
    }
    return writeAudioCredits();
}

bool editor::Exporter::copyLicenses() {
    for (const char* relative : {effectLicensePath(), fluidLicensePath(), postFxLicensePath(), openEngineLicensePath(), "licenses/THIRD-PARTY-AUDIO.txt"}) {
        const fs::path source = config.targetDir / relative;
        const fs::path target = config.destinationDir / relative;
        std::error_code ec;
        if (!fs::is_regular_file(source, ec)) {
            fs::remove(target, ec);
            if (!ec) fs::remove(target.parent_path(), ec);   // only when empty
            continue;
        }
        fs::create_directories(target.parent_path(), ec);
        if (!ec) fs::copy_file(source, target, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            setError("Failed to copy " + target.filename().string() + ": " + ec.message());
            return false;
        }
    }
    return true;
}

// Static helpers for UI display

std::string editor::Exporter::getShaderDisplayName(ShaderType type, uint64_t properties, uint16_t customId) {
    std::string name = ShaderPool::getShaderTypeName(type);

    // Distinguish forked shaders (same type/props, different source) in the list.
    std::string customName = ShaderPool::getCustomShaderName(customId);
    if (!customName.empty()) {
        name += " [" + customName + "]";
    }

    int propCount = ShaderPool::getShaderPropertyCount(type);

    std::string props;
    for (int i = 0; i < propCount; i++) {
        if (properties & shaderPropertyBit(i)) {
            if (!props.empty()) props += ", ";
            props += ShaderPool::getShaderPropertyName(type, i, true);
        }
    }

    if (!props.empty()) {
        name += " (" + props + ")";
    }

    return name;
}

std::string editor::Exporter::getCMakeGraphicBackend(::jasperengine::ShaderBackend backend) {
    switch (backend) {
        case ShaderBackend::GLES3:      return "gles3";
        case ShaderBackend::D3D11:      return "d3d11";
        case ShaderBackend::MetalMacOS:
        case ShaderBackend::MetalIOS:   return "metal";
        case ShaderBackend::Vulkan:     return "vulkan";
        default:                        return "glcore";
    }
}

const char* editor::Exporter::androidPermissionName(const std::string& key) {
    return androidPermissionManifestName(key);
}

std::string editor::Exporter::escapeXml(const std::string& value) {
    return escapeXmlAttribute(value);
}

namespace {
    // A folder holding Emscripten itself: its CMake toolchain file and emcc
    bool isEmscriptenDir(const fs::path& dir) {
        std::error_code ec;
        // (Emscripten 6 on Windows has emcc.exe launchers where earlier versions had emcc.bat)
        return fs::is_regular_file(dir / "cmake" / "Modules" / "Platform" / "Emscripten.cmake", ec) &&
               (fs::exists(dir / "emcc", ec) || fs::exists(dir / "emcc.exe", ec) || fs::exists(dir / "emcc.bat", ec) ||
                fs::exists(dir / "emcc.py", ec));
    }

    bool isEmsdkRoot(const fs::path& dir) {
        std::error_code ec;
        return !dir.empty() && (fs::exists(dir / "emsdk.py", ec) || fs::exists(dir / "emsdk", ec) || fs::exists(dir / "emsdk.bat", ec));
    }

    // The newest <base>/<version>/<rel> (emsdk installs tools as node/20.18.0_64bit/bin/node ...)
    fs::path newestTool(const fs::path& base, const fs::path& rel) {
        std::error_code ec;
        if (!fs::is_directory(base, ec)) return fs::path();
        std::vector<fs::path> found;
        for (const auto& entry : fs::directory_iterator(base, ec)) {
            const fs::path candidate = entry.path() / rel;
            if (fs::is_regular_file(candidate, ec)) found.push_back(candidate);
        }
        if (found.empty()) return fs::path();
        std::sort(found.begin(), found.end());
        return found.back();
    }

    // NODE_JS from a .emscripten config (Python): a quoted path, or emsdk_path + '/...' (what emsdk
    // writes; emsdk_path is the config's folder). "" when it says something else.
    fs::path configNode(const fs::path& configFile) {
        std::ifstream in(configFile);
        std::string line;
        while (std::getline(in, line)) {
            size_t at = line.find_first_not_of(" \t");
            if (at == std::string::npos || line.compare(at, 7, "NODE_JS") != 0) continue;
            const size_t eq = line.find('=', at);
            if (eq == std::string::npos) continue;
            std::string value = line.substr(eq + 1);
            auto quoted = [](const std::string& text) -> std::string {
                const size_t open = text.find_first_of("'\"");
                if (open == std::string::npos) return std::string();
                const size_t close = text.find(text[open], open + 1);
                return close == std::string::npos ? std::string() : text.substr(open + 1, close - open - 1);
            };
            const size_t first = value.find_first_not_of(" \t");
            if (first == std::string::npos) continue;
            value = value.substr(first);
            if (value.rfind("emsdk_path", 0) == 0) {
                const std::string rest = quoted(value);
                if (!rest.empty()) return configFile.parent_path() / fs::path(rest).relative_path();
            } else if (value[0] == '\'' || value[0] == '"') {
                const std::string path = quoted(value);
                if (!path.empty()) return fs::path(path);
            }
        }
        return fs::path();
    }

    void fillEmsdk(editor::EmsdkInfo& info, const fs::path& emscripten, const fs::path& emsdkHint, const std::string& source) {
        std::error_code ec;
        info.found = true;
        info.emscripten = emscripten.string();
        info.toolchainFile = (emscripten / "cmake" / "Modules" / "Platform" / "Emscripten.cmake").string();
        fs::path root = emsdkHint;
        if (root.empty() || !isEmsdkRoot(root)) {
            const fs::path above = emscripten.parent_path().parent_path();   // <emsdk>/upstream/emscripten
            root = isEmsdkRoot(above) ? above : fs::path();
        }
        info.emsdkRoot = root.string();
        info.description = "from " + source + " (" + emscripten.string() + ")";

        // the config emcc reads: EM_CONFIG (a file), else <emscripten>/.emscripten, the emsdk's, ~/.emscripten
        fs::path config;
        if (const char* env = std::getenv("EM_CONFIG"); env && *env && std::string(env).find_first_of("\n;") == std::string::npos &&
            fs::is_regular_file(fs::path(env), ec)) {
            config = fs::path(env);
        }
        const fs::path home = editor::ShellEnv::homeDirectory();
        for (const fs::path& candidate : {emscripten / ".emscripten", root.empty() ? fs::path() : root / ".emscripten",
                                          home.empty() ? fs::path() : home / ".emscripten"}) {
            if (!config.empty()) break;
            if (!candidate.empty() && fs::is_regular_file(candidate, ec)) config = candidate;
        }
        info.configFile = config.string();

#ifdef _WIN32
        const fs::path nodeRel = fs::path("bin") / "node.exe";
        const fs::path pythonRel = "python.exe";
#else
        const fs::path nodeRel = fs::path("bin") / "node";
        const fs::path pythonRel = fs::path("bin") / "python3";
#endif
        fs::path node = config.empty() ? fs::path() : configNode(config);
        if (!node.empty() && !fs::is_regular_file(node, ec)) node.clear();
        if (node.empty() && !root.empty()) node = newestTool(root / "node", nodeRel);
        if (node.empty()) {
            if (const char* env = std::getenv("EMSDK_NODE"); env && *env && fs::is_regular_file(fs::path(env), ec)) node = fs::path(env);
        }
        info.node = node.string();
        const fs::path python = root.empty() ? fs::path() : newestTool(root / "python", pythonRel);
        info.python = python.string();

        info.pathDirs.clear();
        auto addDir = [&info](const fs::path& dir) {
            if (dir.empty()) return;
            const std::string text = dir.string();
            if (std::find(info.pathDirs.begin(), info.pathDirs.end(), text) == info.pathDirs.end()) info.pathDirs.push_back(text);
        };
        addDir(root);
        addDir(emscripten);
        if (!node.empty()) addDir(node.parent_path());
        if (!python.empty()) addDir(python.parent_path());
    }
}

editor::EmsdkInfo editor::Exporter::detectEmsdk(const std::string& overridePath) {
    EmsdkInfo info;

    auto tryRoot = [&](const fs::path& root, const std::string& source) -> bool {
        if (root.empty()) return false;
        // Accept both the emsdk root and the emscripten dir pointed at directly.
        if (isEmscriptenDir(root / "upstream" / "emscripten")) {
            fillEmsdk(info, root / "upstream" / "emscripten", root, source);
            return true;
        }
        if (isEmscriptenDir(root)) {
            fillEmsdk(info, root, fs::path(), source);
            return true;
        }
        return false;
    };

    // An explicit override is an explicit choice: when set, it alone decides.
    if (!overridePath.empty()) {
        tryRoot(fs::path(overridePath), "configured path");
        return info;
    }

    const char* emsdkEnv = std::getenv("EMSDK");
    if (emsdkEnv && tryRoot(fs::path(emsdkEnv), "EMSDK environment variable")) {
        return info;
    }

    // emcc on PATH (no shell: PATH searched in-process), its real folder (a distribution's
    // /usr/bin/emcc links into /usr/share/emscripten)
    const std::string emcc = ShellEnv::findExecutable("emcc");
    if (!emcc.empty()) {
        std::error_code ec;
        const fs::path real = fs::weakly_canonical(fs::path(emcc), ec);
        for (const fs::path& dir : {real.parent_path(), fs::path(emcc).parent_path()}) {
            if (isEmscriptenDir(dir)) {
                fillEmsdk(info, dir, fs::path(), "PATH");
                break;
            }
        }
    }

    return info;
}

std::vector<std::string> editor::Exporter::emscriptenWindowsGeneratorArgs(const CommandRunner::Environment& env, bool generatorGiven,
                                                                          std::string& error, const fs::path& visualStudioNinja) {
    error.clear();
    // emcmake.py: only when neither -G nor CMAKE_GENERATOR picks one (CMake reads that itself)
    if (generatorGiven || !CommandRunner::variable(env, "CMAKE_GENERATOR").empty()) return {};
    std::vector<fs::path> dirs;
    const std::string path = CommandRunner::variable(env, "PATH");
    size_t pos = 0;
    while (pos <= path.size()) {
        size_t end = path.find(';', pos);
        if (end == std::string::npos) end = path.size();
        std::string dir = path.substr(pos, end - pos);
        pos = end + 1;
        if (dir.size() >= 2 && dir.front() == '"' && dir.back() == '"') dir = dir.substr(1, dir.size() - 2);
        if (!dir.empty()) dirs.push_back(fs::path(dir));
    }
    fs::path found;
    if (CommandRunner::resolveWindowsProgram("mingw32-make", dirs, found).empty()) return {"-G", "MinGW Makefiles"};
    if (CommandRunner::resolveWindowsProgram("ninja", dirs, found).empty()) return {"-G", "Ninja"};
    // Visual Studio's own Ninja, which it keeps off PATH
    if (!visualStudioNinja.empty()) return {"-G", "Ninja", "-DCMAKE_MAKE_PROGRAM=" + visualStudioNinja.generic_string()};
    error = "no CMake generator for Emscripten: install Ninja (https://ninja-build.org) or mingw32-make and put it on PATH, "
            "install Visual Studio's C++ CMake tools, or set CMAKE_GENERATOR";
    return {};
}

std::vector<std::string> editor::Exporter::emscriptenCMakeArgs(const EmsdkInfo& sdk, bool generatorGiven,
                                                               const CommandRunner::Environment& env, std::string& error) {
    error.clear();
    std::vector<std::string> args;
    auto cmakePath = [](std::string path) {
        std::replace(path.begin(), path.end(), '\\', '/');
        return path;
    };
    // what emcmake.py adds (emscripten/emcmake.py)
    args.push_back("-DCMAKE_TOOLCHAIN_FILE=" + cmakePath(sdk.toolchainFile));
    if (!sdk.node.empty()) args.push_back("-DCMAKE_CROSSCOMPILING_EMULATOR=" + cmakePath(sdk.node));
#ifdef _WIN32
    // keep CMake from picking Visual Studio, as emcmake does (looked up on the SDK environment's PATH);
    // Visual Studio's own Ninja (a vswhere run) only when that finds no generator
    std::vector<std::string> generatorArgs = emscriptenWindowsGeneratorArgs(env, generatorGiven, error);
    if (!error.empty()) generatorArgs = emscriptenWindowsGeneratorArgs(env, generatorGiven, error, CommandRunner::findVisualStudioNinja());
    for (const std::string& arg : generatorArgs) args.push_back(arg);
#else
    (void)generatorGiven;
    (void)env;
#endif
    return args;
}

std::string editor::Exporter::webTreePathRefusal(const std::vector<fs::path>& roots, size_t maxEntries) {
    size_t seen = 0;
    for (const fs::path& root : roots) {
        std::error_code ec;
        if (root.empty() || !fs::is_directory(root, ec)) continue;
        if (const std::string refusal = webPathRefusal({root}); !refusal.empty()) return refusal;
        watch::LinkedDirectoryIterator it(root, fs::directory_options::skip_permission_denied, ec), end;
        for (; !ec && it != end; it.increment(ec)) {
            if (++seen > maxEntries) {
                return "Web builds on Windows check every file name for % and this project has more than " + std::to_string(maxEntries) +
                       " files in " + root.string() + ": too many to check";
            }
            const std::string name = it->path().filename().string();
            if (name.find_first_of("%\"") != std::string::npos) {
                return "Web builds on Windows can't use a file or folder whose name contains % (or \"): rename " + it->path().string();
            }
        }
    }
    return std::string();
}

std::string editor::Exporter::webTreeRefusal(const std::vector<fs::path>& roots) {
#ifdef _WIN32
    return webTreePathRefusal(roots);
#else
    (void)roots;
    return std::string();
#endif
}

std::vector<fs::path> editor::Exporter::webSourceRoots(Project* project) {
    std::vector<fs::path> roots = {project->getAssetsPath(), project->getLuaPath()};
    for (const fs::path& dir : project->getBuildScriptDirs()) {
        roots.push_back(dir.is_absolute() ? dir : (project->getProjectPath() / dir).lexically_normal());
    }
    return roots;
}

editor::CommandRunner::Environment editor::Exporter::emscriptenEnvironment(const EmsdkInfo& sdk) {
    CommandRunner::Environment env = CommandRunner::currentEnvironment();
#ifdef _WIN32
    const char separator = ';';
#else
    const char separator = ':';
#endif
    std::string path;
    for (const std::string& dir : sdk.pathDirs) {
        path += dir;
        path.push_back(separator);
    }
    const std::string current = CommandRunner::variable(env, "PATH");
    path += current;
    if (!path.empty() && path.back() == separator) path.pop_back();
    CommandRunner::setVariable(env, "PATH", path);
    if (!sdk.emsdkRoot.empty()) CommandRunner::setVariable(env, "EMSDK", sdk.emsdkRoot);
    if (!sdk.configFile.empty()) CommandRunner::setVariable(env, "EM_CONFIG", sdk.configFile);
    if (!sdk.node.empty()) CommandRunner::setVariable(env, "EMSDK_NODE", sdk.node);
    if (!sdk.python.empty()) CommandRunner::setVariable(env, "EMSDK_PYTHON", sdk.python);
    return env;
}
