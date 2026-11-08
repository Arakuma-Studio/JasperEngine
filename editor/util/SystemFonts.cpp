// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SystemFonts.h"

#include "util/CommandRunner.h"
#include "util/FileUtils.h"
#include "util/StringUtils.h"
#include "jobs/JobSystem.h"

#include "imgui.h"

#include <atomic>
#include <cctype>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <set>
#include <sstream>

#ifdef _WIN32
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
    #include <shlobj.h>
#else
    #include <fcntl.h>
    #include <sys/mman.h>
    #include <sys/stat.h>
    #include <unistd.h>
#endif
#ifdef __APPLE__
    #include <CoreFoundation/CoreFoundation.h>
    #include <CoreText/CoreText.h>
#endif

// ImGui's copy of stb_truetype, private to this file (its own unity unit: CMakeLists.txt)
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "imstb_truetype.h"

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

    // A font file to try for a language: its path and face (-1: the face is found by name)
    struct Candidate {
        std::string path;
        int faceIndex = 0;
        // for a collection (Noto Sans CJK): the face whose family name ends with this
        std::string familySuffix;
        // macOS: the face whose PostScript name is this
        std::string postScriptName;
    };

    // What the search found, shared with the main thread. Never destroyed: the search thread
    // may still run when the editor exits.
    struct SearchState {
        std::mutex mutex;
        std::atomic<SystemFonts::State> state{SystemFonts::State::Idle};
        std::atomic<bool> merged{false};
        std::vector<SystemFonts::Font> fonts;
        CjkLanguage language = CjkLanguage::SimplifiedChinese;
    };

    SearchState& searchState() {
        static SearchState* state = new SearchState();
        return *state;
    }

    std::string utf16BeToUtf8(const char* bytes, int length) {
        std::string out;
        for (int i = 0; i + 1 < length; i += 2) {
            const uint32_t unit = (static_cast<uint32_t>(static_cast<unsigned char>(bytes[i])) << 8) |
                                  static_cast<unsigned char>(bytes[i + 1]);
            out += StringUtils::toUTF8(static_cast<char32_t>(unit));
        }
        return out;
    }

    std::string nameString(const stbtt_fontinfo& info, int nameId) {
        int length = 0;
        const char* text = stbtt_GetFontNameString(&info, &length, STBTT_PLATFORM_ID_MICROSOFT, STBTT_MS_EID_UNICODE_BMP,
                                                   STBTT_MS_LANG_ENGLISH, nameId);
        if (text && length > 0) return utf16BeToUtf8(text, length);
        text = stbtt_GetFontNameString(&info, &length, STBTT_PLATFORM_ID_MAC, STBTT_MAC_EID_ROMAN, STBTT_MAC_LANG_ENGLISH, nameId);
        return text && length > 0 ? std::string(text, static_cast<size_t>(length)) : std::string();
    }

    bool fontInfo(const void* data, size_t size, int faceIndex, stbtt_fontinfo& info) {
        if (!data || size < 12 || size > static_cast<size_t>(INT_MAX) || faceIndex < 0) return false;
        const unsigned char* bytes = static_cast<const unsigned char*>(data);
        const int count = stbtt_GetNumberOfFonts(bytes);
        if (count <= 0 || faceIndex >= count) return false;
        const int offset = stbtt_GetFontOffsetForIndex(bytes, faceIndex);
        return offset >= 0 && static_cast<size_t>(offset) < size && stbtt_InitFont(&info, bytes, offset) != 0;
    }

    SystemFonts::Font mapFile(const std::string& path) {
        SystemFonts::Font font;
        font.path = path;
#ifdef _WIN32
        const std::wstring wide = FileUtils::pathFromUtf8(path).wstring();
        HANDLE handle = CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE) return font;
        LARGE_INTEGER size{};
        HANDLE mapping = nullptr;
        if (GetFileSizeEx(handle, &size) && size.QuadPart > 0 && size.QuadPart <= INT_MAX) {
            mapping = CreateFileMappingW(handle, nullptr, PAGE_READONLY, 0, 0, nullptr);
        }
        CloseHandle(handle);
        if (mapping) {
            font.data = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
            if (font.data) font.size = static_cast<size_t>(size.QuadPart);
            CloseHandle(mapping);
        }
#else
        const int handle = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (handle < 0) return font;
        struct stat info{};
        if (::fstat(handle, &info) == 0 && info.st_size > 0 && info.st_size <= INT_MAX) {
            void* data = ::mmap(nullptr, static_cast<size_t>(info.st_size), PROT_READ, MAP_PRIVATE, handle, 0);
            if (data != MAP_FAILED) {
                font.data = data;
                font.size = static_cast<size_t>(info.st_size);
            }
        }
        ::close(handle);
#endif
        return font;
    }

    void unmapFile(SystemFonts::Font& font) {
        if (!font.data) return;
#ifdef _WIN32
        UnmapViewOfFile(font.data);
#else
        ::munmap(const_cast<void*>(font.data), font.size);
#endif
        font.data = nullptr;
        font.size = 0;
    }

    std::string userLocale() {
#ifdef _WIN32
        wchar_t name[LOCALE_NAME_MAX_LENGTH] = {};
        std::string locale;
        if (GetUserDefaultLocaleName(name, LOCALE_NAME_MAX_LENGTH) > 0) {
            for (const wchar_t* c = name; *c; c++) locale += static_cast<char>(*c);
        }
        return locale;
#elif defined(__APPLE__)
        std::string locale;
        if (CFArrayRef languages = CFLocaleCopyPreferredLanguages()) {
            if (CFArrayGetCount(languages) > 0) {
                const CFStringRef first = static_cast<CFStringRef>(CFArrayGetValueAtIndex(languages, 0));
                char buffer[64] = {};
                if (CFStringGetCString(first, buffer, sizeof(buffer), kCFStringEncodingUTF8)) locale = buffer;
            }
            CFRelease(languages);
        }
        return locale;
#else
        for (const char* variable : {"LC_ALL", "LC_MESSAGES", "LANG"}) {
            const char* value = std::getenv(variable);
            if (value && *value) return value;
        }
        return std::string();
#endif
    }

    // The candidates of one language, best first
    std::vector<Candidate> candidates(CjkLanguage language) {
        std::vector<Candidate> found;
#ifdef _WIN32
        PWSTR folder = nullptr;
        fs::path fonts;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Fonts, 0, nullptr, &folder)) && folder) fonts = fs::path(folder);
        if (folder) CoTaskMemFree(folder);
        if (fonts.empty()) return found;
        std::vector<const char*> files;
        switch (language) {
            case CjkLanguage::Japanese:           files = {"YuGothM.ttc", "meiryo.ttc", "msgothic.ttc"}; break;
            case CjkLanguage::Korean:             files = {"malgun.ttf", "gulim.ttc"}; break;
            case CjkLanguage::TraditionalChinese: files = {"msjh.ttc", "mingliub.ttc"}; break;
            default:                              files = {"msyh.ttc", "simsun.ttc"}; break;
        }
        for (const char* file : files) found.push_back({FileUtils::pathToUtf8(fonts / file), 0, {}, {}});
#elif defined(__APPLE__)
        std::vector<const char*> families;
        switch (language) {
            case CjkLanguage::Japanese:           families = {"Hiragino Sans", "Hiragino Kaku Gothic ProN"}; break;
            case CjkLanguage::Korean:             families = {"Apple SD Gothic Neo"}; break;
            case CjkLanguage::TraditionalChinese: families = {"PingFang TC", "Heiti TC"}; break;
            default:                              families = {"PingFang SC", "Hiragino Sans GB", "STHeiti"}; break;
        }
        for (const char* family : families) {
            CFStringRef name = CFStringCreateWithCString(nullptr, family, kCFStringEncodingUTF8);
            CTFontRef font = name ? CTFontCreateWithName(name, 12.0, nullptr) : nullptr;
            if (font) {
                // CoreText answers with a substitute for a family it lacks: keep only the family asked for
                CFStringRef familyName = CTFontCopyFamilyName(font);
                const bool same = familyName && CFStringCompare(familyName, name, 0) == kCFCompareEqualTo;
                CFURLRef url = same ? static_cast<CFURLRef>(CTFontCopyAttribute(font, kCTFontURLAttribute)) : nullptr;
                CFStringRef postScript = same ? CTFontCopyPostScriptName(font) : nullptr;
                char path[4096] = {};
                char postScriptName[256] = {};
                if (url && postScript && CFURLGetFileSystemRepresentation(url, true, reinterpret_cast<UInt8*>(path), sizeof(path)) &&
                    CFStringGetCString(postScript, postScriptName, sizeof(postScriptName), kCFStringEncodingUTF8)) {
                    found.push_back({path, -1, {}, postScriptName});
                }
                if (postScript) CFRelease(postScript);
                if (url) CFRelease(url);
                if (familyName) CFRelease(familyName);
                CFRelease(font);
            }
            if (name) CFRelease(name);
        }
#else
        const char* lang = "zh-cn";
        const char* suffix = "SC";
        switch (language) {
            case CjkLanguage::Japanese:           lang = "ja"; suffix = "JP"; break;
            case CjkLanguage::Korean:             lang = "ko"; suffix = "KR"; break;
            case CjkLanguage::TraditionalChinese: lang = "zh-tw"; suffix = "TC"; break;
            default: break;
        }
        // fontconfig's fonts for the language, best first (no shell; at most 3 s)
        const std::string output = CommandRunner::capture(
            {"fc-match", "-s", "-f", "%{file}\t%{index}\t%{lang}\n", std::string("sans-serif:lang=") + lang}, 3000);
        for (const auto& [path, face] : SystemFonts::parseFontconfig(output, lang)) found.push_back({path, face, {}, {}});
        // without fontconfig: where the distributions put Noto CJK, WenQuanYi and Droid
        for (const char* path : {"/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
                                 "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
                                 "/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc",
                                 "/usr/share/fonts/opentype/noto/NotoSerifCJK-Regular.ttc"}) {
            found.push_back({path, -1, suffix, {}});
        }
        for (const char* path : {"/usr/share/fonts/truetype/wqy/wqy-microhei.ttc", "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc",
                                 "/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf"}) {
            found.push_back({path, 0, {}, {}});
        }
#endif
        return found;
    }

    // Outline emoji and symbol fonts (stb_truetype draws no COLR, sbix or CBDT glyphs)
    std::vector<Candidate> emojiCandidates() {
        std::vector<Candidate> found;
#ifdef _WIN32
        PWSTR folder = nullptr;
        fs::path fonts;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Fonts, 0, nullptr, &folder)) && folder) fonts = fs::path(folder);
        if (folder) CoTaskMemFree(folder);
        if (!fonts.empty()) {
            found.push_back({FileUtils::pathToUtf8(fonts / "seguiemj.ttf"), 0, {}, {}});
            found.push_back({FileUtils::pathToUtf8(fonts / "seguisym.ttf"), 0, {}, {}});
        }
#elif !defined(__APPLE__)
        const std::string output = CommandRunner::capture({"fc-match", "-s", "-f", "%{file}\t%{index}\t%{lang}\n", "emoji"}, 3000);
        for (const auto& [path, face] : SystemFonts::parseFontconfig(output, "")) found.push_back({path, face, {}, {}});
#endif
        return found;
    }

    // The face of a candidate in a mapped file, -1 when none fits
    int chooseFace(const Candidate& candidate, const SystemFonts::Font& file) {
        if (candidate.faceIndex >= 0) return candidate.faceIndex;
        const std::vector<std::pair<std::string, std::string>> names = SystemFonts::faceNames(file.data, file.size);
        for (size_t i = 0; i < names.size(); i++) {
            const std::string& family = names[i].first;
            if (!candidate.postScriptName.empty() && names[i].second == candidate.postScriptName) return static_cast<int>(i);
            if (!candidate.familySuffix.empty() && family.size() >= candidate.familySuffix.size() &&
                family.compare(family.size() - candidate.familySuffix.size(), std::string::npos, candidate.familySuffix) == 0) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    // The first loadable candidate not chosen yet, mapped; nothing when none is
    bool pick(const std::vector<Candidate>& list, const std::string& covers, uint32_t probe,
              std::set<std::pair<std::string, int>>& chosen, std::vector<SystemFonts::Font>& out) {
        for (const Candidate& candidate : list) {
            std::error_code ec;
            if (!fs::is_regular_file(FileUtils::pathFromUtf8(candidate.path), ec)) continue;
            SystemFonts::Font font = mapFile(candidate.path);
            const int face = font.data ? chooseFace(candidate, font) : -1;
            if (face >= 0 && chosen.count({candidate.path, face})) {
                // a font chosen for an earlier language covers this one too
                unmapFile(font);
                return false;
            }
            if (face >= 0 && SystemFonts::isLoadable(font.data, font.size, face) &&
                (probe == 0 || SystemFonts::hasOutline(font.data, font.size, face, probe))) {
                font.faceIndex = face;
                font.covers = covers;
                chosen.insert({candidate.path, face});
                out.push_back(font);
                return true;
            }
            unmapFile(font);
        }
        return false;
    }

    void search(std::vector<CjkLanguage> order) {
        SearchState& s = searchState();
        std::vector<SystemFonts::Font> found;
        std::set<std::pair<std::string, int>> chosen;
        for (CjkLanguage language : order) {
            // a character each language's fonts have: kana, Hangul, a Han character
            const uint32_t probe = language == CjkLanguage::Japanese ? 0x3042 : language == CjkLanguage::Korean ? 0xD55C : 0x4E2D;
            pick(candidates(language), SystemFonts::languageTag(language), probe, chosen, found);
        }
        pick(emojiCandidates(), "emoji", 0x1F600, chosen, found);
        {
            std::lock_guard<std::mutex> lock(s.mutex);
            s.fonts = std::move(found);
            s.state.store(s.fonts.empty() ? SystemFonts::State::None : SystemFonts::State::Ready, std::memory_order_release);
        }
    }

}

void SystemFonts::startSearch(CjkLanguage preference) {
    SearchState& s = searchState();
    State idle = State::Idle;
    if (!s.state.compare_exchange_strong(idle, State::Searching)) return;
    std::vector<CjkLanguage> order = searchOrder(preference, userLocale());
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        s.language = order.front();
    }
    // off the main thread, on the jobs' Background lane: fontconfig and CoreText can take a
    // while on a cold start (and fc-match is given at most 3 s)
    jobs::enqueueDetached([order = std::move(order)]() { search(order); }, jobs::Lane::Background, "system-fonts");
}

SystemFonts::State SystemFonts::state() {
    return searchState().state.load(std::memory_order_acquire);
}

std::vector<SystemFonts::Font> SystemFonts::fonts() {
    SearchState& s = searchState();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.fonts;
}

CjkLanguage SystemFonts::effectiveLanguage() {
    SearchState& s = searchState();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.language;
}

bool SystemFonts::applyPending(const std::vector<Target>& targets) {
    SearchState& s = searchState();
    if (s.state.load(std::memory_order_acquire) != State::Ready || s.merged.load(std::memory_order_relaxed)) return false;
    s.merged.store(true, std::memory_order_relaxed);
    mergeInto(fonts(), targets);
    return true;
}

void SystemFonts::mergeInto(const std::vector<Font>& fonts, const std::vector<Target>& targets) {
    ImGuiIO& io = ImGui::GetIO();
    for (const Target& target : targets) {
        if (!target.font) continue;
        for (const Font& font : fonts) {
            ImFontConfig config;
            config.MergeMode = true;
            config.DstFont = target.font;
            config.FontDataOwnedByAtlas = false;
            config.FontNo = static_cast<ImU32>(font.faceIndex);
            // CJK fonts hold tens of thousands of glyphs: no oversampling (ImGui's advice), the
            // weight of the font it falls back from
            config.OversampleH = 1;
            config.OversampleV = 1;
            config.RasterizerMultiply = target.rasterizerMultiply;
            std::snprintf(config.Name, sizeof(config.Name), "%s", FileUtils::pathToUtf8(FileUtils::pathFromUtf8(font.path).filename()).c_str());
            io.Fonts->AddFontFromMemoryTTF(const_cast<void*>(font.data), static_cast<int>(font.size), target.sizePixels, &config);
        }
    }
}

const char* SystemFonts::settingName(CjkLanguage language) {
    switch (language) {
        case CjkLanguage::Japanese: return "japanese";
        case CjkLanguage::Korean: return "korean";
        case CjkLanguage::SimplifiedChinese: return "simplified_chinese";
        case CjkLanguage::TraditionalChinese: return "traditional_chinese";
        default: return "auto";
    }
}

CjkLanguage SystemFonts::fromSettingName(std::string_view name) {
    for (CjkLanguage language : {CjkLanguage::Japanese, CjkLanguage::Korean, CjkLanguage::SimplifiedChinese,
                                 CjkLanguage::TraditionalChinese}) {
        if (name == settingName(language)) return language;
    }
    return CjkLanguage::Auto;
}

bool SystemFonts::isLanguageSetting(std::string_view name) {
    return name == "auto" || fromSettingName(name) != CjkLanguage::Auto;
}

const char* SystemFonts::languageTag(CjkLanguage language) {
    switch (language) {
        case CjkLanguage::Japanese: return "ja";
        case CjkLanguage::Korean: return "ko";
        case CjkLanguage::SimplifiedChinese: return "zh-Hans";
        case CjkLanguage::TraditionalChinese: return "zh-Hant";
        default: return "auto";
    }
}

CjkLanguage SystemFonts::languageOfLocale(std::string_view locale) {
    const auto startsWith = [&locale](std::string_view prefix) {
        return locale.size() >= prefix.size() && locale.compare(0, prefix.size(), prefix) == 0 &&
               (locale.size() == prefix.size() || locale[prefix.size()] == '_' || locale[prefix.size()] == '-' ||
                locale[prefix.size()] == '.');
    };
    if (startsWith("ja")) return CjkLanguage::Japanese;
    if (startsWith("ko")) return CjkLanguage::Korean;
    if (startsWith("zh")) {
        for (std::string_view traditional : {"Hant", "TW", "HK", "MO"}) {
            if (locale.find(traditional) != std::string_view::npos) return CjkLanguage::TraditionalChinese;
        }
    }
    return CjkLanguage::SimplifiedChinese;
}

std::vector<CjkLanguage> SystemFonts::searchOrder(CjkLanguage preference, std::string_view userLocale) {
    const CjkLanguage first = preference == CjkLanguage::Auto ? languageOfLocale(userLocale) : preference;
    std::vector<CjkLanguage> order{first};
    for (CjkLanguage language : {CjkLanguage::SimplifiedChinese, CjkLanguage::Korean, CjkLanguage::Japanese}) {
        if (language != first) order.push_back(language);
    }
    return order;
}

std::vector<std::pair<std::string, int>> SystemFonts::parseFontconfig(std::string_view output, std::string_view lang) {
    std::vector<std::pair<std::string, int>> fonts;
    std::istringstream lines{std::string(output)};
    std::string line;
    while (std::getline(lines, line)) {
        std::istringstream fields(line);
        std::string path, index, languages;
        std::getline(fields, path, '\t');
        std::getline(fields, index, '\t');
        std::getline(fields, languages);
        if (path.empty()) continue;
        // the upper 16 bits name a variable font instance, which stb_truetype cannot select
        const long face = std::strtol(index.c_str(), nullptr, 10);
        std::string extension = fs::path(path).extension().string();
        for (char& c : extension) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (face < 0 || (face >> 16) != 0 || (extension != ".ttf" && extension != ".ttc" && extension != ".otf")) continue;
        if (!lang.empty() && ("|" + languages + "|").find("|" + std::string(lang) + "|") == std::string::npos) continue;
        fonts.emplace_back(path, static_cast<int>(face));
    }
    return fonts;
}

bool SystemFonts::isLoadable(const void* data, size_t size, int faceIndex) {
    stbtt_fontinfo info;
    return fontInfo(data, size, faceIndex, info);
}

std::vector<std::pair<std::string, std::string>> SystemFonts::faceNames(const void* data, size_t size) {
    std::vector<std::pair<std::string, std::string>> names;
    if (!data || size < 12) return names;
    const int count = stbtt_GetNumberOfFonts(static_cast<const unsigned char*>(data));
    for (int i = 0; i < count; i++) {
        stbtt_fontinfo info;
        if (fontInfo(data, size, i, info)) names.emplace_back(nameString(info, 1), nameString(info, 6));
        else names.emplace_back();
    }
    return names;
}

bool SystemFonts::hasOutline(const void* data, size_t size, int faceIndex, uint32_t codepoint) {
    stbtt_fontinfo info;
    if (!fontInfo(data, size, faceIndex, info)) return false;
    const int glyph = stbtt_FindGlyphIndex(&info, static_cast<int>(codepoint));
    return glyph > 0 && !stbtt_IsGlyphEmpty(&info, glyph);
}
