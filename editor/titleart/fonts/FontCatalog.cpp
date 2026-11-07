// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "FontCatalog.h"

#include "WebFontDecode.h"

#include "App.h"
#include "Project.h"
#include "htmlui/GoogleFontsImporter.h"
#include "htmlui/HttpClient.h"
#include "htmlui/RemoteImporter.h"
#include "htmlui/StylesheetImporter.h"
#include "io/AtomicFile.h"
#include "jobs/JobSystem.h"
#include "titleart/TitleArtRenderHost.h"
#include "util/FileUtils.h"

#include "json.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>
#include <set>
#include <system_error>
#include <utility>

namespace jasperengine::editor::titlefonts {

    namespace fs = std::filesystem;
    using TJson = jasperengine::titleart::Json;

    // ===================================================================== free functions

    const char* fontSectionLabel(FontSection section) {
        switch (section) {
            case FontSection::Library: return "Library";
            case FontSection::Google: return "Google Fonts";
            case FontSection::System: return "System";
            case FontSection::Project: break;
        }
        return "Project";
    }

    const char* fontCategoryLabel(FontCategory category) {
        switch (category) {
            case FontCategory::Sans: return "Sans";
            case FontCategory::Serif: return "Serif";
            case FontCategory::Display: return "Display";
            case FontCategory::Handwriting: return "Handwriting";
            case FontCategory::Monospace: return "Monospace";
            case FontCategory::Any: break;
        }
        return "Any";
    }

    FontCategory categoryOfFont(const Os2Info& os2) {
        const uint8_t familyKind = os2.panose[0];
        const uint8_t serifStyle = os2.panose[1];
        const uint8_t proportion = os2.panose[3];
        const int familyClass = (os2.familyClass >> 8) & 0xFF;
        if (os2.monospacedHint || (familyKind == 2 && proportion == 9)) return FontCategory::Monospace;
        if (familyKind == 3 || familyClass == 10) return FontCategory::Handwriting;
        if (familyKind == 4 || familyClass == 9) return FontCategory::Display;
        if (familyClass == 8) return FontCategory::Sans;
        if ((familyClass >= 1 && familyClass <= 5) || familyClass == 7) return FontCategory::Serif;
        if (familyKind == 2) {
            // PANOSE serif style: 2-10 serifs (cove, square, thin, triangle ...), 11-15 sans (normal,
            // obtuse, perpendicular, flared, rounded)
            if (serifStyle >= 11 && serifStyle <= 15) return FontCategory::Sans;
            if (serifStyle >= 2 && serifStyle <= 10) return FontCategory::Serif;
        }
        return FontCategory::Any;
    }

    FontCategory categoryOfGoogle(const std::string& category) {
        if (category == "Sans Serif") return FontCategory::Sans;
        if (category == "Serif") return FontCategory::Serif;
        if (category == "Display") return FontCategory::Display;
        if (category == "Handwriting") return FontCategory::Handwriting;
        if (category == "Monospace") return FontCategory::Monospace;
        return FontCategory::Any;
    }

    namespace {

        LibraryFontProvider& libraryProvider() {
            static LibraryFontProvider provider;
            return provider;
        }

        std::string lowerAscii(std::string text) {
            for (char& c : text) c = (char)(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
            return text;
        }

        bool lessFold(const std::string& a, const std::string& b) {
            const std::string la = lowerAscii(a), lb = lowerAscii(b);
            return la != lb ? la < lb : a < b;
        }

        void sortSection(std::vector<FontEntry>& entries) {
            std::sort(entries.begin(), entries.end(), [](const FontEntry& a, const FontEntry& b) {
                if (a.family != b.family) return lessFold(a.family, b.family);
                if (a.style != b.style) return lessFold(a.style, b.style);
                return a.key < b.key;
            });
        }

        std::string detailKey(const std::string& path, int faceIndex) { return path + "#" + std::to_string(faceIndex); }

        // ---------- partial reads of font files ----------

        // Reads ranges of a file (the tables a question needs, not the whole font)
        class FileRanges {
        public:
            explicit FileRanges(const std::string& path) : in(FileUtils::pathFromUtf8(path), std::ios::binary) {
                if (in) {
                    in.seekg(0, std::ios::end);
                    size = (uint64_t)std::max<std::streamoff>(0, (std::streamoff)in.tellg());
                }
            }
            bool ok() const { return (bool)in && size > 0; }
            bool read(uint64_t offset, size_t length, std::vector<uint8_t>& out) {
                out.clear();
                if (offset > size || length > size - offset) return false;
                out.resize(length);
                in.clear();
                in.seekg((std::streamoff)offset);
                if (length) in.read(reinterpret_cast<char*>(out.data()), (std::streamsize)length);
                return (bool)in;
            }
            uint32_t u32(uint64_t offset) {
                std::vector<uint8_t> b;
                return read(offset, 4, b) ? (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3] : 0;
            }
            uint64_t fileSize() const { return size; }

        private:
            std::ifstream in;
            uint64_t size = 0;
        };

        constexpr size_t MAX_TABLE_READ = 32u << 20;

        // the faces' directory offsets of a file
        std::vector<uint32_t> faceOffsetsOf(FileRanges& file) {
            std::vector<uint8_t> head;
            if (!file.read(0, 12, head)) return {};
            const uint32_t version = (uint32_t)head[0] << 24 | (uint32_t)head[1] << 16 | (uint32_t)head[2] << 8 | head[3];
            if (version == sfntTag("ttcf")) {
                const uint32_t count = (uint32_t)head[8] << 24 | (uint32_t)head[9] << 16 | (uint32_t)head[10] << 8 | head[11];
                if (count == 0 || count > 0xFFFF) return {};
                std::vector<uint8_t> list;
                if (!file.read(12, (size_t)count * 4, list)) return {};
                std::vector<uint32_t> offsets;
                for (uint32_t i = 0; i < count; i++) {
                    const uint8_t* p = &list[(size_t)i * 4];
                    offsets.push_back((uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]);
                }
                return offsets;
            }
            if (version == 0x00010000u || version == sfntTag("OTTO") || version == sfntTag("true")) return {0};
            return {};
        }

        // One face of the file as a compact sfnt holding only the tables `wanted` names (their
        // offsets rewritten), so FontTables reads it as face 0; `outline` from the real directory
        bool compactFace(FileRanges& file, int faceIndex, const std::vector<uint32_t>& wanted, std::vector<uint8_t>& out, OutlineKind& outline,
                         size_t& faceCount) {
            const std::vector<uint32_t> faces = faceOffsetsOf(file);
            faceCount = faces.size();
            if (faceIndex < 0 || (size_t)faceIndex >= faces.size()) return false;
            const uint64_t base = faces[(size_t)faceIndex];
            std::vector<uint8_t> header;
            if (!file.read(base, 12, header)) return false;
            const uint32_t version = (uint32_t)header[0] << 24 | (uint32_t)header[1] << 16 | (uint32_t)header[2] << 8 | header[3];
            const size_t count = (size_t)(header[4] << 8 | header[5]);
            std::vector<uint8_t> directory;
            if (!file.read(base + 12, count * 16, directory)) return false;
            std::vector<SfntTable> keep;
            bool glyf = false, loca = false, cff = false, cff2 = false;
            for (size_t i = 0; i < count; i++) {
                const uint8_t* r = &directory[i * 16];
                SfntTable t;
                t.tag = (uint32_t)r[0] << 24 | (uint32_t)r[1] << 16 | (uint32_t)r[2] << 8 | r[3];
                t.checksum = (uint32_t)r[4] << 24 | (uint32_t)r[5] << 16 | (uint32_t)r[6] << 8 | r[7];
                t.offset = (uint32_t)r[8] << 24 | (uint32_t)r[9] << 16 | (uint32_t)r[10] << 8 | r[11];
                t.length = (uint32_t)r[12] << 24 | (uint32_t)r[13] << 16 | (uint32_t)r[14] << 8 | r[15];
                glyf = glyf || (t.tag == sfntTag("glyf") && t.length > 0);
                loca = loca || t.tag == sfntTag("loca");
                cff = cff || (t.tag == sfntTag("CFF ") && t.length > 0);
                cff2 = cff2 || (t.tag == sfntTag("CFF2") && t.length > 0);
                if (std::find(wanted.begin(), wanted.end(), t.tag) != wanted.end() && t.length <= MAX_TABLE_READ) keep.push_back(t);
            }
            outline = cff2 ? OutlineKind::Cff2 : cff ? OutlineKind::Cff : (glyf && loca) ? OutlineKind::TrueType : OutlineKind::None;
            out.assign(12 + 16 * keep.size(), 0);
            const auto put = [&out](size_t o, uint32_t v, int bytes) {
                for (int i = 0; i < bytes; i++) out[o + i] = (uint8_t)(v >> (8 * (bytes - 1 - i)));
            };
            put(0, version, 4);
            put(4, (uint32_t)keep.size(), 2);
            for (size_t i = 0; i < keep.size(); i++) {
                std::vector<uint8_t> data;
                if (!file.read(keep[i].offset, keep[i].length, data)) return false;
                while (out.size() % 4) out.push_back(0);
                const size_t offset = out.size();
                put(12 + 16 * i, keep[i].tag, 4);
                put(12 + 16 * i + 4, keep[i].checksum, 4);
                put(12 + 16 * i + 8, (uint32_t)offset, 4);
                put(12 + 16 * i + 12, keep[i].length, 4);
                out.insert(out.end(), data.begin(), data.end());
            }
            return true;
        }

        // each face's family and style, read from the name tables only
        std::vector<FontNames> faceNamesOf(const std::string& path) {
            std::vector<FontNames> names;
            FileRanges file(path);
            if (!file.ok()) return names;
            const size_t faces = faceOffsetsOf(file).size();
            for (size_t i = 0; i < faces; i++) {
                std::vector<uint8_t> compact;
                OutlineKind outline = OutlineKind::None;
                size_t count = 0;
                if (compactFace(file, (int)i, {sfntTag("name")}, compact, outline, count)) names.push_back(fontNames(compact.data(), compact.size(), 0));
                else names.push_back(FontNames());
            }
            return names;
        }

        bool readWholeFile(const fs::path& path, std::string& out) {
            std::ifstream in(path, std::ios::binary);
            if (!in) return false;
            out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            return true;
        }

        // the catalog cache and fonts copied into the project: atomic, a failed write keeps the old file
        bool writeFileAtomic(const fs::path& path, const std::string& content) {
            return io::writeFileAtomic(path, content).ok();
        }

        int64_t systemNow() {
            return (int64_t)std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        }

        FontCatalog::HttpResult curlGet(const std::string& url) {
            FontCatalog::HttpResult result;
            htmlui::HttpClient client;
            const htmlui::HttpResponse response = client.get(url);
            if (!response.ok()) {
                result.error = response.error.empty() ? "HTTP " + std::to_string(response.status) : response.error;
                return result;
            }
            if (!htmlui::HttpClient::readBody(response, result.body)) {
                result.error = "the response could not be read";
                return result;
            }
            result.finalUrl = response.finalUrl;
            result.ok = true;
            return result;
        }

        // ---------- Google Fonts ----------

        std::string googleKey(const std::string& family) { return "google:" + family; }

        // Google's family list (fonts.google.com/metadata/fonts; the ")]}'" guard line removed)
        bool parseGoogleMetadata(std::string body, std::vector<FontEntry>& out) {
            out.clear();
            if (body.compare(0, 4, ")]}'") == 0) {
                const size_t newline = body.find('\n');
                body.erase(0, newline == std::string::npos ? body.size() : newline + 1);
            }
            const nlohmann::json json = nlohmann::json::parse(body, nullptr, false);
            if (!json.is_object()) return false;
            const auto list = json.find("familyMetadataList");
            if (list == json.end() || !list->is_array()) return false;
            for (const nlohmann::json& item : *list) {
                if (!item.is_object() || !item.contains("family") || !item["family"].is_string()) continue;
                FontEntry entry;
                entry.section = FontSection::Google;
                entry.family = item["family"].get<std::string>();
                entry.key = googleKey(entry.family);
                entry.style = "Regular";
                if (item.contains("category") && item["category"].is_string()) entry.category = categoryOfGoogle(item["category"].get<std::string>());
                if (item.contains("subsets") && item["subsets"].is_array()) {
                    for (const nlohmann::json& subset : item["subsets"]) {
                        if (subset.is_string()) entry.googleSubsets.push_back(subset.get<std::string>());
                    }
                }
                if (item.contains("fonts") && item["fonts"].is_object()) {
                    for (const auto& font : item["fonts"].items()) {
                        const std::string& variant = font.key();
                        if (!variant.empty() && variant.back() != 'i') {
                            const int weight = std::atoi(variant.c_str());
                            if (weight > 0) entry.googleWeights.push_back(weight);
                        }
                    }
                    std::sort(entry.googleWeights.begin(), entry.googleWeights.end());
                }
                entry.googleVariable = item.contains("axes") && item["axes"].is_array() && !item["axes"].empty();
                out.push_back(std::move(entry));
            }
            return !out.empty();
        }

        // the families of a session, by cache folder: fetched once per session
        struct GoogleSession {
            std::mutex mutex;
            std::map<std::string, std::vector<FontEntry>> families;
        };
        GoogleSession& googleSession() {
            static GoogleSession session;
            return session;
        }

        struct UnicodeRange {
            uint32_t first, last;
        };
        struct SubsetRanges {
            const char* subset;
            std::vector<UnicodeRange> ranges;
        };
        // Google's unicode-range subsets (the parts of them that tell scripts apart)
        const std::vector<SubsetRanges>& subsetTable() {
            static const std::vector<SubsetRanges> table = {
                {"latin", {{0x0000, 0x00FF}, {0x0131, 0x0131}, {0x0152, 0x0153}, {0x02BB, 0x02BC}, {0x02C6, 0x02C6}, {0x02DA, 0x02DA}, {0x02DC, 0x02DC},
                           {0x2000, 0x206F}, {0x20AC, 0x20AC}, {0x2122, 0x2122}, {0x2191, 0x2193}, {0x2212, 0x2215}, {0xFEFF, 0xFEFF}, {0xFFFD, 0xFFFD}}},
                {"latin-ext", {{0x0100, 0x024F}, {0x1E00, 0x1EFF}, {0x2C60, 0x2C7F}, {0xA720, 0xA7FF}}},
                {"vietnamese", {{0x0102, 0x0103}, {0x0110, 0x0111}, {0x0128, 0x0129}, {0x0168, 0x0169}, {0x01A0, 0x01A1}, {0x01AF, 0x01B0}, {0x1EA0, 0x1EF9}, {0x20AB, 0x20AB}}},
                {"cyrillic", {{0x0400, 0x045F}, {0x0490, 0x0491}, {0x04B0, 0x04B1}, {0x2116, 0x2116}}},
                {"cyrillic-ext", {{0x0460, 0x052F}, {0x1C80, 0x1C88}, {0x2DE0, 0x2DFF}, {0xA640, 0xA69F}}},
                {"greek", {{0x0370, 0x03FF}}},
                {"greek-ext", {{0x1F00, 0x1FFF}}},
                {"japanese", {{0x3000, 0x30FF}, {0x3400, 0x4DBF}, {0x4E00, 0x9FFF}, {0xFF00, 0xFFEF}}},
                {"korean", {{0x1100, 0x11FF}, {0x3000, 0x303F}, {0x3130, 0x318F}, {0x4E00, 0x9FFF}, {0xAC00, 0xD7AF}}},
                {"chinese-simplified", {{0x3000, 0x303F}, {0x3400, 0x4DBF}, {0x4E00, 0x9FFF}, {0xFF00, 0xFFEF}}},
                {"chinese-traditional", {{0x3000, 0x303F}, {0x3100, 0x312F}, {0x3400, 0x4DBF}, {0x4E00, 0x9FFF}, {0xFF00, 0xFFEF}}},
                {"chinese-hongkong", {{0x3000, 0x303F}, {0x3400, 0x4DBF}, {0x4E00, 0x9FFF}, {0xFF00, 0xFFEF}}},
                {"arabic", {{0x0600, 0x06FF}, {0x0750, 0x077F}, {0xFB50, 0xFDFF}, {0xFE70, 0xFEFF}}},
                {"hebrew", {{0x0590, 0x05FF}, {0xFB1D, 0xFB4F}}},
                {"thai", {{0x0E00, 0x0E7F}}},
                {"devanagari", {{0x0900, 0x097F}, {0xA8E0, 0xA8FF}}},
            };
            return table;
        }

        // ---------- inbox ----------

        struct DownloadResult {
            std::string key;
            bool ok = false;
            std::string path;
            std::string error;
        };

        struct DetailResult {
            std::string key;
            std::shared_ptr<const FontDetails> details;
        };

    }

    bool googleSubsetsCover(const std::vector<std::string>& subsets, uint32_t codepoint) {
        for (const std::string& subset : subsets) {
            for (const SubsetRanges& known : subsetTable()) {
                if (subset != known.subset) continue;
                for (const UnicodeRange& range : known.ranges) {
                    if (codepoint >= range.first && codepoint <= range.last) return true;
                }
            }
        }
        return false;
    }

    void setLibraryFontProvider(LibraryFontProvider provider) {
        libraryProvider() = std::move(provider);
    }

    bool loadFontDetails(const std::string& path, int faceIndex, FontDetails& out, std::string& error) {
        FileRanges file(path);
        if (!file.ok()) {
            error = "cannot read '" + path + "'";
            return false;
        }
        std::vector<uint8_t> compact;
        size_t faces = 0;
        if (!compactFace(file, faceIndex, {sfntTag("cmap"), sfntTag("name"), sfntTag("OS/2"), sfntTag("post"), sfntTag("fvar")}, compact,
                         out.outlines, faces)) {
            error = faces == 0 ? "'" + path + "' is not a TrueType or OpenType font" : "'" + path + "' has no face " + std::to_string(faceIndex);
            return false;
        }
        const uint8_t* data = compact.data();
        const size_t size = compact.size();
        out.names = fontNames(data, size, 0);
        out.os2 = fontOs2(data, size, 0);
        out.category = categoryOfFont(out.os2);
        out.licence = out.os2.present ? classifyFsType(out.os2.fsType) : FontLicenceInfo();
        out.axes = fontAxes(data, size, 0);
        out.instances = fontNamedInstances(data, size, 0);
        out.cmap.load(data, size, 0);
        return true;
    }

    std::string previewTextFor(const std::string& layerText) {
        const std::string firstLine = layerText.substr(0, layerText.find('\n'));
        const std::vector<uint32_t> codepoints = utf8CodePoints(firstLine);
        std::string out;
        size_t kept = 0;
        for (uint32_t cp : codepoints) {
            if (kept == 24) break;
            if (cp == '\r') continue;
            appendUtf8(out, cp);
            kept++;
        }
        bool visible = false;
        for (uint32_t cp : utf8CodePoints(out)) visible = visible || !isIgnorableForCoverage(cp);
        return visible ? out : std::string(DEFAULT_PREVIEW_TEXT);
    }

    bool coversMyTextByDefault(const std::string& text) {
        for (unsigned char c : text) {
            if (c >= 0x80) return true;
        }
        return false;
    }

    AxesAfterFontChange keepAxesOfNewFont(const TJson& variations, const std::vector<FontAxisInfo>& newAxes) {
        AxesAfterFontChange result;
        if (!variations.is_object()) return result;
        for (auto it = variations.begin(); it != variations.end(); ++it) {
            const auto axis = std::find_if(newAxes.begin(), newAxes.end(), [&it](const FontAxisInfo& a) { return a.tag == it.key(); });
            if (axis == newAxes.end() || !it.value().is_number()) {
                result.dropped.push_back(it.key());
                continue;
            }
            const double value = std::clamp(it.value().get<double>(), (double)axis->minValue, (double)axis->maxValue);
            result.variations[it.key()] = value;
        }
        return result;
    }

    std::string changeFontUndoName(const std::string& family, bool axesReset) {
        return "Change Font to " + family + (axesReset ? " (Axes Reset)" : "");
    }

    // ===================================================================== FontCatalog

    struct FontCatalog::Inbox {
        std::mutex mutex;
        bool projectDone = false;
        std::vector<FontEntry> project;
        bool systemDone = false;
        std::vector<SystemFontFace> system;
        bool googleDone = false;
        bool googleOk = false;
        std::vector<FontEntry> google;
        std::vector<DetailResult> details;
        std::vector<DownloadResult> downloads;
        // every job started (main thread only): waitIdle waits for them
        std::vector<jobs::TaskRef> tasks;
    };

    FontCatalog::Services FontCatalog::editorServices(Project* project) {
        Services services;
        if (project) {
            services.projectFonts = [project]() {
                std::vector<ProjectFontFile> out;
                const EditorTitleAssetResolver* resolver = TitleArtRenderHost::editor().resolver(project);
                if (!resolver) return out;
                for (const jasperengine::titleart::ResolvedTitleAsset& asset : resolver->projectAssets(jasperengine::titleart::TitleAssetKind::Font))
                    out.push_back({asset.path, asset.assetPath, asset.guid});
                return out;
            };
            const fs::path assets = project->getAssetsPath();
            services.projectWebFonts = [assets]() {
                std::vector<ProjectFontFile> out;
                std::error_code ec;
                const auto options = fs::directory_options::skip_permission_denied;
                for (auto it = fs::recursive_directory_iterator(assets, options, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
                    const std::string name = FileUtils::pathToUtf8(it->path().filename());
                    if (!name.empty() && name[0] == '.') {
                        if (it->is_directory(ec)) it.disable_recursion_pending();
                        continue;
                    }
                    if (!it->is_regular_file(ec) || !isWebFontFileName(name)) continue;
                    out.push_back({FileUtils::pathToUtf8(it->path()), FileUtils::pathToGenericUtf8(it->path().lexically_relative(assets)), ""});
                }
                return out;
            };
        }
        services.cacheDir = App::getUserCacheBaseDir() / "jasperengine";
        services.now = &systemNow;
        return services;
    }

    FontCatalog::FontCatalog(Services services) : services(std::move(services)), inbox(std::make_shared<Inbox>()) {
        if (!this->services.now) this->services.now = &systemNow;
        if (!this->services.http) this->services.http = &curlGet;
    }

    FontCatalog::~FontCatalog() {
        // the jobs hold the inbox, not this object: they finish into it and it goes with them
        for (jobs::TaskRef& task : inbox->tasks) task.cancel();
    }

    void FontCatalog::open() {
        if (opened) return;
        opened = true;
        startProjectScan();
        startSystemListing();
        startGoogleMetadata();
        rebuild();
    }

    void FontCatalog::refreshProject() {
        if (!opened) return;
        projectScanning = false;
        startProjectScan();
    }

    void FontCatalog::startProjectScan() {
        if (projectScanning) return;
        projectScanning = true;
        std::vector<ProjectFontFile> files = services.projectFonts ? services.projectFonts() : std::vector<ProjectFontFile>();
        auto webFonts = services.projectWebFonts;
        std::shared_ptr<Inbox> box = inbox;
        inbox->tasks.push_back(jobs::submit(
            [box, files = std::move(files), webFonts]() {
                std::vector<FontEntry> entries;
                for (const ProjectFontFile& file : files) {
                    const std::vector<FontNames> faces = faceNamesOf(file.path);
                    const fs::path path = FileUtils::pathFromUtf8(file.path);
                    const size_t count = std::max<size_t>(1, faces.size());
                    for (size_t i = 0; i < count; i++) {
                        FontEntry entry;
                        entry.section = FontSection::Project;
                        entry.key = "project:" + file.assetPath + "#" + std::to_string(i);
                        entry.family = i < faces.size() ? faces[i].family : "";
                        entry.style = i < faces.size() ? faces[i].style : "";
                        if (entry.family.empty()) entry.family = FileUtils::pathToUtf8(path.stem());
                        entry.fileName = FileUtils::pathToUtf8(path.filename());
                        entry.path = file.path;
                        entry.faceIndex = (int)i;
                        entry.collectionFace = faces.size() > 1;
                        entry.assetPath = file.assetPath;
                        entry.guid = file.guid;
                        entries.push_back(std::move(entry));
                    }
                }
                for (const ProjectFontFile& file : webFonts ? webFonts() : std::vector<ProjectFontFile>()) {
                    const fs::path path = FileUtils::pathFromUtf8(file.path);
                    FontEntry entry;
                    entry.section = FontSection::Project;
                    entry.key = "project-web:" + file.assetPath;
                    entry.fileName = FileUtils::pathToUtf8(path.filename());
                    entry.path = file.path;
                    entry.assetPath = file.assetPath;
                    entry.webFont = true;
                    // its names from the decoded font (a web font's own tables are compressed)
                    std::string bytes;
                    if (readWholeFile(path, bytes)) {
                        const DecodedFont decoded = decodeWebFont(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
                        if (decoded.ok) {
                            const FontNames names = fontNames(decoded.bytes.data(), decoded.bytes.size(), 0);
                            entry.family = names.family;
                            entry.style = names.style;
                        }
                    }
                    if (entry.family.empty()) entry.family = FileUtils::pathToUtf8(path.stem());
                    entries.push_back(std::move(entry));
                }
                std::lock_guard<std::mutex> lock(box->mutex);
                box->project = std::move(entries);
                box->projectDone = true;
            },
            {jobs::Lane::Background, jobs::GateClass::Default, "title-font-project-scan"}));
    }

    void FontCatalog::startSystemListing() {
        if (systemListing || systemListed) return;
        systemListing = true;
        if (!services.systemFonts) {
            SystemFontList::instance().request();
            return;
        }
        std::shared_ptr<Inbox> box = inbox;
        auto lister = services.systemFonts;
        inbox->tasks.push_back(jobs::submit(
            [box, lister]() {
                std::vector<SystemFontFace> faces = lister();
                std::lock_guard<std::mutex> lock(box->mutex);
                box->system = std::move(faces);
                box->systemDone = true;
            },
            {jobs::Lane::Background, jobs::GateClass::Default, "title-font-system-list"}));
    }

    void FontCatalog::startGoogleMetadata() {
        if (google == GoogleState::Loading || google == GoogleState::Ready) return;
        std::shared_ptr<std::vector<FontEntry>> known;
        {
            GoogleSession& session = googleSession();
            std::lock_guard<std::mutex> lock(session.mutex);
            const auto it = session.families.find(services.cacheDir.string());
            if (it != session.families.end()) known = std::make_shared<std::vector<FontEntry>>(it->second);
        }
        if (known) {
            // fetched earlier this session: only which Regulars are in the cache is looked at again
            google = GoogleState::Loading;
            std::shared_ptr<Inbox> box = inbox;
            const fs::path folder = services.cacheDir / "google-fonts";
            inbox->tasks.push_back(jobs::submit(
                [box, known, folder]() {
                    for (FontEntry& entry : *known) {
                        entry.path.clear();
                        entry.fileName.clear();
                        std::error_code ec;
                        const fs::path cached = folder / (htmlui::GoogleFontsImporter::familyFileStem(entry.family) + "-400.ttf");
                        if (fs::is_regular_file(cached, ec)) {
                            entry.path = FileUtils::pathToUtf8(cached);
                            entry.fileName = FileUtils::pathToUtf8(cached.filename());
                        }
                    }
                    std::lock_guard<std::mutex> lock(box->mutex);
                    box->google = std::move(*known);
                    box->googleOk = true;
                    box->googleDone = true;
                },
                {jobs::Lane::Background, jobs::GateClass::Default, "title-google-fonts-cached"}));
            return;
        }
        google = GoogleState::Loading;
        std::shared_ptr<Inbox> box = inbox;
        const fs::path folder = services.cacheDir / "google-fonts";
        auto http = services.http;
        const int64_t now = services.now();
        inbox->tasks.push_back(jobs::system().netChannel().submit(
            [box, folder, http, now]() {
                const fs::path metadataFile = folder / "metadata.json";
                const fs::path timeFile = folder / "metadata.time";
                std::string cached, stamp;
                const bool haveCache = readWholeFile(metadataFile, cached) && !cached.empty();
                const int64_t fetched = haveCache && readWholeFile(timeFile, stamp) ? std::atoll(stamp.c_str()) : 0;
                std::vector<FontEntry> families;
                bool ok = false;
                if (haveCache && now - fetched >= 0 && now - fetched < GOOGLE_METADATA_MAX_AGE_SECONDS) ok = parseGoogleMetadata(cached, families);
                if (!ok) {
                    const HttpResult response = http(htmlui::GoogleFontsImporter::metadataUrl());
                    if (response.ok && parseGoogleMetadata(response.body, families)) {
                        ok = true;
                        writeFileAtomic(metadataFile, response.body);
                        writeFileAtomic(timeFile, std::to_string(now));
                    } else if (haveCache) {
                        // offline with an old list: it still names the families
                        ok = parseGoogleMetadata(cached, families);
                    }
                }
                for (FontEntry& entry : families) {
                    const fs::path cachedFont = folder / (htmlui::GoogleFontsImporter::familyFileStem(entry.family) + "-400.ttf");
                    std::error_code ec;
                    if (fs::is_regular_file(cachedFont, ec)) {
                        entry.path = FileUtils::pathToUtf8(cachedFont);
                        entry.fileName = FileUtils::pathToUtf8(cachedFont.filename());
                    }
                }
                std::lock_guard<std::mutex> lock(box->mutex);
                box->google = std::move(families);
                box->googleOk = ok;
                box->googleDone = true;
            },
            {jobs::Lane::Normal, jobs::GateClass::Default, "title-google-fonts-metadata"}));
    }

    fs::path FontCatalog::googleCachePath(const std::string& family) const {
        return services.cacheDir / "google-fonts" / (htmlui::GoogleFontsImporter::familyFileStem(family) + "-400.ttf");
    }

    std::string FontCatalog::googleMessage() const {
        return google == GoogleState::Offline ? std::string(GOOGLE_OFFLINE_MESSAGE) : std::string();
    }

    void FontCatalog::poll() {
        std::vector<DetailResult> details;
        std::vector<DownloadResult> finished;
        bool changed = false;
        {
            std::lock_guard<std::mutex> lock(inbox->mutex);
            if (inbox->projectDone) {
                inbox->projectDone = false;
                projectEntries = std::move(inbox->project);
                sortSection(projectEntries);
                projectScanned = true;
                projectScanning = false;
                changed = true;
            }
            if (inbox->systemDone) {
                inbox->systemDone = false;
                systemEntries.clear();
                for (const SystemFontFace& face : inbox->system) {
                    FontEntry entry;
                    entry.section = FontSection::System;
                    entry.key = "system:" + face.path + "#" + std::to_string(face.faceIndex);
                    entry.family = face.family;
                    entry.style = face.style;
                    entry.fileName = FileUtils::pathToUtf8(FileUtils::pathFromUtf8(face.path).filename());
                    entry.path = face.path;
                    entry.faceIndex = face.faceIndex;
                    systemEntries.push_back(std::move(entry));
                }
                sortSection(systemEntries);
                systemListed = true;
                changed = true;
            }
            if (inbox->googleDone) {
                inbox->googleDone = false;
                if (inbox->googleOk) {
                    googleEntries = std::move(inbox->google);
                    sortSection(googleEntries);
                    google = GoogleState::Ready;
                    GoogleSession& session = googleSession();
                    std::lock_guard<std::mutex> sessionLock(session.mutex);
                    session.families[services.cacheDir.string()] = googleEntries;
                } else {
                    google = GoogleState::Offline;
                }
                changed = true;
            }
            details.swap(inbox->details);
            finished.swap(inbox->downloads);
        }
        if (systemListing && !systemListed && !services.systemFonts && SystemFontList::instance().state() == SystemFontList::State::Ready) {
            std::lock_guard<std::mutex> lock(inbox->mutex);
            inbox->system = SystemFontList::instance().faces();
            inbox->systemDone = true;
        }
        for (DetailResult& result : details) {
            DetailSlot& slot = detailSlots[result.key];
            slot.loading = false;
            slot.failed = !result.details;
            slot.details = result.details ? result.details : std::make_shared<const FontDetails>();
        }
        for (const DownloadResult& result : finished) {
            downloadsRunning = std::max(0, downloadsRunning - 1);
            Download& download = downloads[result.key];
            download.state = result.ok ? DownloadState::Ready : DownloadState::Failed;
            download.error = result.error;
            if (result.ok) {
                for (FontEntry& entry : googleEntries) {
                    if (entry.key == result.key) {
                        entry.path = result.path;
                        entry.fileName = FileUtils::pathToUtf8(FileUtils::pathFromUtf8(result.path).filename());
                    }
                }
                changed = true;
            }
        }
        if (!finished.empty()) startDownloads();
        if (changed) rebuild();
        // finished jobs leave the wait list
        inbox->tasks.erase(std::remove_if(inbox->tasks.begin(), inbox->tasks.end(), [](const jobs::TaskRef& task) { return task.isReady(); }),
                           inbox->tasks.end());
    }

    void FontCatalog::waitIdle() {
        // a finished job can start another (a download queue): wait until none is left
        for (int round = 0; round < 64; round++) {
            std::vector<jobs::TaskRef> tasks = inbox->tasks;
            for (jobs::TaskRef& task : tasks) task.wait();
            poll();
            if (inbox->tasks.empty()) break;
        }
    }

    void FontCatalog::rebuild() {
        libraryEntries.clear();
        if (libraryProvider()) {
            for (const LibraryFont& font : libraryProvider()()) {
                FontEntry entry;
                entry.section = FontSection::Library;
                entry.key = "library:" + font.id;
                entry.family = font.family;
                entry.style = font.style;
                entry.fileName = FileUtils::pathToUtf8(FileUtils::pathFromUtf8(font.path).filename());
                entry.path = font.path;
                entry.faceIndex = font.faceIndex;
                entry.libraryId = font.id;
                entry.category = font.category;
                libraryEntries.push_back(std::move(entry));
            }
            sortSection(libraryEntries);
        }
        all.clear();
        for (const std::vector<FontEntry>* section : {&projectEntries, &libraryEntries, &googleEntries, &systemEntries})
            all.insert(all.end(), section->begin(), section->end());
    }

    const FontEntry* FontCatalog::find(const std::string& key) const {
        for (const FontEntry& entry : all) {
            if (entry.key == key) return &entry;
        }
        return nullptr;
    }

    const FontDetails* FontCatalog::details(const FontFileRef& file) {
        if (file.path.empty()) return nullptr;
        const std::string key = detailKey(file.path, file.faceIndex);
        DetailSlot& slot = detailSlots[key];
        if (slot.details) return slot.details.get();
        if (slot.loading) return nullptr;
        slot.loading = true;
        std::shared_ptr<Inbox> box = inbox;
        inbox->tasks.push_back(jobs::submit(
            [box, key, file]() {
                auto details = std::make_shared<FontDetails>();
                std::string error;
                const bool ok = loadFontDetails(file.path, file.faceIndex, *details, error);
                std::lock_guard<std::mutex> lock(box->mutex);
                box->details.push_back({key, ok ? std::shared_ptr<const FontDetails>(details) : nullptr});
            },
            {jobs::Lane::Background, jobs::GateClass::Default, "title-font-details"}));
        return nullptr;
    }

    const FontDetails* FontCatalog::details(const FontEntry& entry) {
        // a web font's tables are compressed: it is listed to be converted, not read
        if (entry.webFont) return nullptr;
        return details(entry.file());
    }

    bool FontCatalog::entryCovers(const FontEntry& entry, uint32_t codepoint, bool& known) {
        if (entry.section == FontSection::Google && entry.path.empty()) return googleSubsetsCover(entry.googleSubsets, codepoint);
        const FontDetails* d = details(entry);
        if (!d) {
            known = false;
            return false;
        }
        return d->cmap.covers(codepoint);
    }

    bool FontCatalog::missingCodepoints(const FontEntry& entry, const std::vector<uint32_t>& text, const std::vector<FontFileRef>& fallbacks,
                                        std::vector<uint32_t>& out) {
        out.clear();
        bool known = !entry.webFont;
        std::vector<const FontDetails*> fallbackDetails;
        for (const FontFileRef& fallback : fallbacks) {
            const FontDetails* d = details(fallback);
            if (!d) known = false;
            fallbackDetails.push_back(d);
        }
        // starts the entry's read too, so both arrive together
        bool entryKnown = true;
        entryCovers(entry, 'A', entryKnown);
        if (!known || !entryKnown) return false;
        std::set<uint32_t> seen;
        for (uint32_t cp : text) {
            if (isIgnorableForCoverage(cp) || !seen.insert(cp).second) continue;
            bool unused = true;
            if (entryCovers(entry, cp, unused)) continue;
            bool covered = false;
            for (const FontDetails* d : fallbackDetails) covered = covered || (d && d->cmap.covers(cp));
            if (!covered) out.push_back(cp);
        }
        return true;
    }

    bool FontCatalog::fallbackCandidates(const std::vector<uint32_t>& missing, std::vector<const FontEntry*>& out) {
        out.clear();
        std::vector<const FontEntry*> partial;
        bool ready = true;
        for (const FontEntry& entry : all) {
            if (entry.section != FontSection::Project || entry.webFont) continue;
            const FontDetails* d = details(entry);
            if (!d) {
                ready = false;
                continue;
            }
            size_t covered = 0;
            for (uint32_t cp : missing) covered += d->cmap.covers(cp) ? 1 : 0;
            if (covered == missing.size() && covered > 0) out.push_back(&entry);
            else if (covered > 0) partial.push_back(&entry);
        }
        out.insert(out.end(), partial.begin(), partial.end());
        return ready;
    }

    std::vector<const FontEntry*> FontCatalog::filter(const Query& query) {
        std::vector<std::string> words;
        {
            const std::string search = lowerAscii(query.search);
            size_t start = 0;
            while (start < search.size()) {
                size_t end = search.find(' ', start);
                if (end == std::string::npos) end = search.size();
                if (end > start) words.push_back(search.substr(start, end - start));
                start = end + 1;
            }
        }
        std::vector<const FontEntry*> kept;
        for (const FontEntry& entry : all) {
            if (!words.empty()) {
                const std::string haystack = lowerAscii(entry.family + " " + entry.style + " " + entry.fileName);
                bool matches = true;
                for (const std::string& word : words) matches = matches && haystack.find(word) != std::string::npos;
                if (!matches) continue;
            }
            if (query.category != FontCategory::Any) {
                FontCategory category = entry.category;
                bool known = entry.category != FontCategory::Any || entry.section == FontSection::Google || entry.webFont;
                if (!known) {
                    if (const FontDetails* d = details(entry)) {
                        category = d->category;
                        known = true;
                    }
                }
                if (known && category != query.category) continue;
            }
            if (query.coversText) {
                std::vector<uint32_t> missing;
                if (missingCodepoints(entry, query.text, query.fallbacks, missing) && !missing.empty()) continue;
            }
            kept.push_back(&entry);
        }
        return kept;
    }

    void FontCatalog::requestGoogleDownload(const std::string& key) {
        const FontEntry* entry = nullptr;
        for (const FontEntry& candidate : googleEntries) {
            if (candidate.key == key) entry = &candidate;
        }
        if (!entry) return;
        Download& download = downloads[key];
        if (!entry->path.empty()) {
            download.state = DownloadState::Ready;
            return;
        }
        if (download.state == DownloadState::Queued || download.state == DownloadState::Downloading || download.state == DownloadState::Ready) return;
        download.state = DownloadState::Queued;
        download.family = entry->family;
        download.error.clear();
        downloadQueue.push_back(key);
        startDownloads();
    }

    void FontCatalog::startDownloads() {
        while (downloadsRunning < MAX_GOOGLE_DOWNLOADS && !downloadQueue.empty()) {
            const std::string key = downloadQueue.front();
            downloadQueue.erase(downloadQueue.begin());
            Download& download = downloads[key];
            if (download.state != DownloadState::Queued) continue;
            download.state = DownloadState::Downloading;
            downloadsRunning++;
            std::shared_ptr<Inbox> box = inbox;
            auto http = services.http;
            const std::string family = download.family;
            const fs::path target = googleCachePath(family);
            inbox->tasks.push_back(jobs::system().netChannel().submit(
                [box, http, key, family, target]() {
                    DownloadResult result;
                    result.key = key;
                    htmlui::GoogleFontsImporter::FamilyRequest request;
                    request.family = family;
                    request.variants.push_back({400, false});
                    const std::string cssUrl = htmlui::GoogleFontsImporter::css2Url({request});
                    const HttpResult css = http(cssUrl);
                    if (!css.ok) {
                        result.error = "Google Fonts: " + css.error;
                    } else {
                        const std::vector<htmlui::CssFontFace> faces = htmlui::findFontFaces(css.body);
                        const htmlui::CssFontSource* source = nullptr;
                        for (const htmlui::CssFontFace& face : faces) {
                            for (const htmlui::CssFontSource& candidate : face.sources) {
                                if (!source && candidate.usable()) source = &candidate;
                            }
                        }
                        if (!source) {
                            result.error = "Google Fonts sent no TrueType file for '" + family + "'";
                        } else {
                            const HttpResult font = http(htmlui::resolveUrl(css.finalUrl.empty() ? cssUrl : css.finalUrl, source->url));
                            const auto* bytes = reinterpret_cast<const uint8_t*>(font.body.data());
                            if (!font.ok) result.error = "Google Fonts: " + font.error;
                            else if (sfntFaceOffsets(bytes, font.body.size()).empty()) result.error = "Google Fonts sent something that is not a font";
                            else if (!writeFileAtomic(target, font.body)) result.error = "cannot write '" + FileUtils::pathToUtf8(target) + "'";
                            else {
                                result.ok = true;
                                result.path = FileUtils::pathToUtf8(target);
                            }
                        }
                    }
                    std::lock_guard<std::mutex> lock(box->mutex);
                    box->downloads.push_back(std::move(result));
                },
                {jobs::Lane::Normal, jobs::GateClass::Default, "title-google-font-download"}));
        }
    }

    FontCatalog::DownloadState FontCatalog::googleDownloadState(const std::string& key) const {
        const auto it = downloads.find(key);
        return it == downloads.end() ? DownloadState::None : it->second.state;
    }

    std::string FontCatalog::googleDownloadError(const std::string& key) const {
        const auto it = downloads.find(key);
        return it == downloads.end() ? std::string() : it->second.error;
    }

}
