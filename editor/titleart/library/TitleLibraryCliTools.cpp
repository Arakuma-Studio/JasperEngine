// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleLibraryCliTools.h"

#include "StyleLint.h"
#include "TitleLibrary.h"
#include "TitleLibraryApply.h"
#include "TitleLibraryImports.h"
#include "TitleLibraryProvider.h"
#include "TitleLibraryVerify.h"
#include "texsynth/SynthImage.h"
#include "titleart/TitleArtIO.h"
#include "titleart/TitleArtOps.h"

#include "miniz.h"
#include "stb_image.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <regex>
#include <set>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::titlelibrary;
namespace fs = std::filesystem;

namespace {

    constexpr int OK = 0, FINDINGS = 1, USAGE = 2;

    bool readBytes(const fs::path& path, std::string& out) {
        std::ifstream in(path, std::ios::binary);
        if (!in) return false;
        std::ostringstream ss;
        ss << in.rdbuf();
        out = ss.str();
        return true;
    }

    bool writeBytes(const fs::path& path, const std::string& bytes) {
        std::error_code ec;
        if (!path.parent_path().empty()) fs::create_directories(path.parent_path(), ec);
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << bytes;
        return (bool)out;
    }

    // `--name value` options and positional arguments
    struct Args {
        std::map<std::string, std::string> values;
        std::set<std::string> flags;
        std::vector<std::string> positional;
        std::string error;
    };
    Args parseArgs(const std::vector<std::string>& args, const std::set<std::string>& withValue, const std::set<std::string>& flags) {
        Args out;
        for (size_t i = 0; i < args.size(); i++) {
            const std::string& a = args[i];
            if (a.rfind("--", 0) == 0) {
                if (withValue.count(a)) {
                    if (i + 1 >= args.size()) {
                        out.error = a + " takes a value";
                        return out;
                    }
                    out.values[a] = args[++i];
                } else if (flags.count(a)) {
                    out.flags.insert(a);
                } else {
                    out.error = "unknown option " + a;
                    return out;
                }
            } else {
                out.positional.push_back(a);
            }
        }
        return out;
    }

    bool globMatch(const std::string& glob, const std::string& text) {
        std::string re;
        for (char c : glob) {
            if (c == '*') re += ".*";
            else if (c == '?') re += '.';
            else if (std::string("\\^$.|+()[]{}").find(c) != std::string::npos) re += std::string("\\") + c;
            else re += c;
        }
        return std::regex_match(text, std::regex(re));
    }

    std::string thumbName(const LibraryItemInfo& item, const std::string& sample) {
        std::string name = item.ref;
        std::replace(name.begin(), name.end(), '/', '.');
        return name + "." + sample + ".png";
    }

    // ---- PNG post-processing ----

    uint32_t crc32Of(const std::string& bytes) {
        static uint32_t table[256];
        static bool built = false;
        if (!built) {
            for (uint32_t n = 0; n < 256; n++) {
                uint32_t c = n;
                for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                table[n] = c;
            }
            built = true;
        }
        uint32_t c = 0xFFFFFFFFu;
        for (unsigned char b : bytes) c = table[(c ^ b) & 0xFF] ^ (c >> 8);
        return c ^ 0xFFFFFFFFu;
    }

    void be32(std::string& out, uint32_t v) {
        out += (char)(v >> 24);
        out += (char)(v >> 16);
        out += (char)(v >> 8);
        out += (char)v;
    }

    // a full render cropped to its alpha bounds plus a margin and fitted into the thumbnail size
    texsynth::Image frameThumbnail(const unsigned char* rgba, int w, int h) {
        int x0 = w, y0 = h, x1 = -1, y1 = -1;
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                if (rgba[((size_t)y * w + x) * 4 + 3] == 0) continue;
                x0 = std::min(x0, x);
                y0 = std::min(y0, y);
                x1 = std::max(x1, x);
                y1 = std::max(y1, y);
            }
        }
        texsynth::Image out = texsynth::Image::make(THUMBNAIL_WIDTH, THUMBNAIL_HEIGHT, 4, 8);
        if (x1 < 0) return out;
        const int pad = std::max(2, (x1 - x0 + y1 - y0) / 40);
        x0 = std::max(0, x0 - pad);
        y0 = std::max(0, y0 - pad);
        x1 = std::min(w - 1, x1 + pad);
        y1 = std::min(h - 1, y1 + pad);
        const int cw = x1 - x0 + 1, ch = y1 - y0 + 1;
        const double s = std::min({(double)THUMBNAIL_WIDTH / cw, (double)THUMBNAIL_HEIGHT / ch, 1.0});
        const int dw = std::max(1, (int)std::lround(cw * s)), dh = std::max(1, (int)std::lround(ch * s));
        const int ox = (THUMBNAIL_WIDTH - dw) / 2, oy = (THUMBNAIL_HEIGHT - dh) / 2;
        // box filter over premultiplied colour
        for (int y = 0; y < dh; y++) {
            for (int x = 0; x < dw; x++) {
                const int sx0 = x0 + (int)(x / s), sy0 = y0 + (int)(y / s);
                const int sx1 = std::max(sx0 + 1, std::min(x1 + 1, x0 + (int)((x + 1) / s))), sy1 = std::max(sy0 + 1, std::min(y1 + 1, y0 + (int)((y + 1) / s)));
                double r = 0, g = 0, b = 0, a = 0;
                int n = 0;
                for (int sy = sy0; sy < sy1; sy++) {
                    for (int sx = sx0; sx < sx1; sx++) {
                        const unsigned char* p = &rgba[((size_t)sy * w + sx) * 4];
                        const double pa = p[3] / 255.0;
                        r += p[0] * pa;
                        g += p[1] * pa;
                        b += p[2] * pa;
                        a += pa;
                        n++;
                    }
                }
                uint8_t* o = &out.data[((size_t)(y + oy) * THUMBNAIL_WIDTH + x + ox) * 4];
                if (a > 0) {
                    o[0] = (uint8_t)std::lround(std::min(255.0, r / a));
                    o[1] = (uint8_t)std::lround(std::min(255.0, g / a));
                    o[2] = (uint8_t)std::lround(std::min(255.0, b / a));
                }
                o[3] = (uint8_t)std::lround(std::min(255.0, 255.0 * a / std::max(1, n)));
            }
        }
        return out;
    }

    // R41: the thumbnails live in 16 MiB: posterized a little (invisible at tile size), filtered per
    // row and deflated at miniz's best level, with the recipe chunk
    std::string encodeThumbnail(const texsynth::Image& image, const std::string& recipe) {
        std::vector<uint8_t> data = image.data;
        for (size_t i = 0; i + 3 < data.size(); i += 4) {
            if (data[i + 3] == 0) {
                data[i] = data[i + 1] = data[i + 2] = 0;
                continue;
            }
            for (int c = 0; c < 3; c++) data[i + c] = (uint8_t)((data[i + c] & 0xF8) | (data[i + c] >> 5));
            if (data[i + 3] != 255) data[i + 3] = (uint8_t)((data[i + 3] & 0xFC) | (data[i + 3] >> 6));
        }
        const int stride = image.width * 4;
        std::vector<uint8_t> filtered;
        filtered.reserve((size_t)(stride + 1) * image.height);
        std::vector<uint8_t> prev((size_t)stride, 0), row((size_t)stride), attempt((size_t)stride);
        for (int y = 0; y < image.height; y++) {
            std::copy(data.begin() + (long)y * stride, data.begin() + (long)(y + 1) * stride, row.begin());
            int bestType = 0;
            long bestScore = -1;
            std::vector<uint8_t> best;
            for (int type : {0, 1, 2, 4}) {
                long score = 0;
                for (int x = 0; x < stride; x++) {
                    const int a = x >= 4 ? row[x - 4] : 0, b = prev[x], c = x >= 4 ? prev[x - 4] : 0;
                    int predicted = 0;
                    if (type == 1) predicted = a;
                    else if (type == 2) predicted = b;
                    else if (type == 4) {
                        const int pa = std::abs(b - c), pb = std::abs(a - c), pc = std::abs(a + b - 2 * c);
                        predicted = pa <= pb && pa <= pc ? a : (pb <= pc ? b : c);
                    }
                    const uint8_t value = (uint8_t)(row[x] - predicted);
                    attempt[x] = value;
                    score += std::min<int>(value, 256 - value);
                }
                if (bestScore < 0 || score < bestScore) {
                    bestScore = score;
                    bestType = type;
                    best = attempt;
                }
            }
            filtered.push_back((uint8_t)bestType);
            filtered.insert(filtered.end(), best.begin(), best.end());
            prev = row;
        }
        mz_ulong compressedSize = mz_compressBound((mz_ulong)filtered.size());
        std::string compressed(compressedSize, '\0');
        if (mz_compress2((unsigned char*)compressed.data(), &compressedSize, filtered.data(), (mz_ulong)filtered.size(), MZ_BEST_COMPRESSION) != MZ_OK) return {};
        compressed.resize(compressedSize);
        auto chunk = [](std::string& out, const std::string& type, const std::string& body) {
            be32(out, (uint32_t)body.size());
            out += type + body;
            be32(out, crc32Of(type + body));
        };
        std::string png("\x89PNG\r\n\x1a\n", 8);
        std::string header;
        be32(header, (uint32_t)image.width);
        be32(header, (uint32_t)image.height);
        header += std::string("\x08\x06\x00\x00\x00", 5);
        chunk(png, "IHDR", header);
        chunk(png, "tEXt", std::string(THUMBNAIL_TEXT_KEY) + std::string(1, '\0') + recipe);
        chunk(png, "IDAT", compressed);
        chunk(png, "IEND", {});
        return png;
    }

    // ---- the one-shot headless editor on a scratch project ----

    struct Scratch {
        fs::path dir;
        std::vector<std::string> calls;     // ndjson lines
    };

    bool makeScratchProject(const fs::path& dir, std::string& error) {
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir / "assets" / "items", ec);
        fs::create_directories(dir / "scripts", ec);
        if (!writeBytes(dir / "project.yaml", "name: Title Library Scratch\nassetsDir: assets\nluaDir: scripts\nscenes: []\n")) {
            error = "cannot write " + (dir / "project.yaml").string();
            return false;
        }
        // the pack's fonts and HDRIs where a library apply names them
        const fs::path pack = TitleLibrary::packDir();
        for (const PackFont& f : packFonts()) {
            for (const std::string& file : f.files) {
                fs::create_directories(dir / "assets" / TITLE_FONTS_ASSET_DIR / f.id, ec);
                fs::copy_file(pack / "fonts" / f.id / file, dir / "assets" / TITLE_FONTS_ASSET_DIR / f.id / file, fs::copy_options::overwrite_existing, ec);
            }
        }
        std::string manifest;
        if (readBytes(pack / "hdri" / "manifest.json", manifest)) {
            const Json m = Json::parse(manifest, nullptr, false);
            fs::create_directories(dir / "assets" / TITLE_ENVIRONMENTS_ASSET_DIR, ec);
            if (m.is_object() && m.contains("environments")) {
                for (auto it = m["environments"].begin(); it != m["environments"].end(); ++it) {
                    const std::string file = it.value().value("file", std::string());
                    fs::copy_file(pack / "hdri" / file, dir / "assets" / TITLE_ENVIRONMENTS_ASSET_DIR / file, fs::copy_options::overwrite_existing, ec);
                }
            }
        }
        return true;
    }

    // runs the calls in a one-shot headless editor on the scratch project; the result envelopes
    bool runBatch(const char* executable, const Scratch& s, std::vector<Json>& results, std::string& error) {
        const fs::path calls = s.dir / "calls.ndjson", out = s.dir / "results.ndjson", err = s.dir / "editor-stderr.txt";
        std::string text;
        for (const std::string& c : s.calls) text += c + "\n";
        if (!writeBytes(calls, text)) {
            error = "cannot write " + calls.string();
            return false;
        }
        const std::string command = "\"" + std::string(executable) + "\" batch --project \"" + s.dir.string() + "\" --continue --quiet < \"" + calls.string() +
                                    "\" > \"" + out.string() + "\" 2> \"" + err.string() + "\"";
        const int status = std::system(command.c_str());
        std::string lines;
        readBytes(out, lines);
        std::istringstream in(lines);
        std::string line;
        while (std::getline(in, line)) {
            const Json j = Json::parse(line, nullptr, false);
            if (j.is_object()) results.push_back(j);
        }
        if (results.empty()) {
            std::string tail;
            readBytes(err, tail);
            error = "the headless editor answered nothing (exit " + std::to_string(status) + "): " + tail.substr(tail.size() > 2000 ? tail.size() - 2000 : 0);
            return false;
        }
        return true;
    }

    std::string callLine(const char* tool, const Json& arguments) {
        return Json{{"tool", tool}, {"arguments", arguments}}.dump();
    }

    // §6.14.3's numeric metrics: where check_title's measured value sits, which way is better and
    // the threshold member
    struct CalibrationMetric {
        const char* path;
        bool higherIsBetter;
        const char* thresholdKey;
        // the worst of the object's numbers (delta_l per background) or the member itself
        double value(const Json& measured) const {
            if (!measured.is_object() || !measured.contains(path)) return std::nan("");
            const Json& v = measured[path];
            if (v.is_number()) return v.get<double>();
            if (v.is_object()) {
                double out = higherIsBetter ? 1e18 : -1e18;
                for (auto it = v.begin(); it != v.end(); ++it) {
                    if (!it->is_number()) continue;
                    out = higherIsBetter ? std::min(out, it->get<double>()) : std::max(out, it->get<double>());
                }
                return out;
            }
            return std::nan("");
        }
        double threshold(const Json& t) const {
            return t.is_object() && t.contains(thresholdKey) && t[thresholdKey].is_number() ? t[thresholdKey].get<double>() : std::nan("");
        }
    };

    const CalibrationMetric* calibrationMetric(const std::string& id) {
        static const std::map<std::string, CalibrationMetric> METRICS = {
            {"legibility_contrast", {"delta_l", true, "delta_l"}},
            {"counter_survival", {"delta_e", true, "delta_e"}},
            {"metal_range", {"l_range", true, "l_range_min"}},
            {"not_clipped", {"min_margin_px", true, "margin_px"}},
            {"clipped_highlights", {"clipped_pct", false, "clipped_pct_max"}},
            {"halo", {"halo_share", false, "halo_share_max"}},
            {"fallback_parity", {"fraction_over", false, "fraction_over_8"}},
            {"rim_spikes", {"spike_to_band", false, "spike_to_band"}},
            {"style_lint.contrast", {"value", true, "value"}},
            {"style_lint.rims_thin", {"value", true, "value"}},
            {"style_lint.rims_thick", {"value", false, "value"}},
            {"style_lint.spectrum_span", {"value", false, "value"}},
            {"style_lint.hue_count", {"value", false, "value"}},
            {"style_lint.hard_shadow", {"value", true, "value"}},
        };
        auto it = METRICS.find(id);
        return it == METRICS.end() ? nullptr : &it->second;
    }

    std::string replaceDotted(std::string id) {
        std::replace(id.begin(), id.end(), '.', '-');
        return id;
    }

    struct Planned {
        const LibraryItemInfo* item;
        ThumbnailSample sample;
        std::string doc;        // assets-relative document path
        std::string png;        // project-relative render
    };

    // writes the document of every (item, sample) into the scratch project; the plan
    bool planDocuments(const std::vector<std::pair<const LibraryItemInfo*, ThumbnailSample>>& work, Scratch& s, std::vector<Planned>& plan,
                       std::vector<std::string>& failures) {
        int n = 0;
        for (const auto& [item, sample] : work) {
            titleart::TitleArtDoc doc;
            std::map<std::string, std::string> transient;
            std::string error;
            if (!thumbnailDocument(*item, sample, doc, transient, error)) {
                failures.push_back(item->ref + " (" + sample.key + "): " + error);
                continue;
            }
            const std::string name = "items/" + std::to_string(n++) + ".titleart";
            if (!writeBytes(s.dir / "assets" / name, titleart::writeTitleArt(doc))) return false;
            plan.push_back({item, sample, name, "renders/" + std::to_string(n - 1) + ".png"});
        }
        return true;
    }

    std::vector<std::pair<const LibraryItemInfo*, ThumbnailSample>> thumbnailWork(const std::string& only) {
        std::vector<std::pair<const LibraryItemInfo*, ThumbnailSample>> work;
        for (LibraryKind kind : LIBRARY_KINDS) {
            for (const LibraryItemInfo& item : TitleLibrary::get().items(kind)) {
                if (item.scope != LibraryScope::BuiltIn) continue;
                bool wanted = only.empty();
                for (size_t from = 0; !wanted && from < only.size();) {     // a comma-separated list of globs
                    const size_t comma = only.find(',', from);
                    wanted = globMatch(only.substr(from, comma == std::string::npos ? std::string::npos : comma - from), item.ref);
                    from = comma == std::string::npos ? only.size() : comma + 1;
                }
                if (!wanted) continue;
                for (const ThumbnailSample& s : thumbnailSamples(item)) work.push_back({&item, s});
            }
        }
        return work;
    }

}

// ---- list, verify, lint, gen-docs ----

int titlelibrary::runListCommand(const std::vector<std::string>& args) {
    const Args a = parseArgs(args, {"--kind", "--family"}, {"--json"});
    if (!a.error.empty()) {
        std::cerr << "title-library list: " << a.error << "\n";
        return USAGE;
    }
    Json rows = Json::array();
    for (LibraryKind kind : LIBRARY_KINDS) {
        if (a.values.count("--kind") && a.values.at("--kind") != libraryKindName(kind)) continue;
        for (const LibraryItemInfo& item : TitleLibrary::get().items(kind)) {
            if (a.values.count("--family") && item.family != a.values.at("--family")) continue;
            rows.push_back(Json{{"kind", libraryKindName(kind)}, {"id", item.ref}, {"name", item.name.any()}, {"family", item.family}, {"dimension", item.dimension},
                                {"available", item.available}, {"reason", item.unavailableReason}, {"cost", item.cost}});
        }
    }
    if (a.flags.count("--json")) {
        std::cout << rows.dump(2) << "\n";
    } else {
        for (const Json& r : rows) {
            std::cout << r["kind"].get<std::string>() << "\t" << r["id"].get<std::string>() << "\t" << r["name"].get<std::string>()
                      << (r["available"].get<bool>() ? "" : "\t(unavailable: " + r["reason"].get<std::string>() + ")") << "\n";
        }
    }
    return OK;
}

int titlelibrary::runVerifyCommand(const std::vector<std::string>& args) {
    const Args a = parseArgs(args, {"--review"}, {"--no-thumbnails", "--json"});
    if (!a.error.empty()) {
        std::cerr << "title-library verify: " << a.error << "\n";
        return USAGE;
    }
    VerifyOptions o;
    o.thumbnails = !a.flags.count("--no-thumbnails");
    std::error_code ec;
    o.reviewFile = a.values.count("--review") ? fs::path(a.values.at("--review")) : fs::path("tools/titleart/pachislop-port/review.json");
    if (!fs::exists(o.reviewFile, ec)) o.reviewFile.clear();
    const std::vector<VerifyFinding> findings = verifyLibrary(o);
    int errors = 0, warnings = 0;
    for (const VerifyFinding& f : findings) {
        (f.warning ? warnings : errors)++;
        std::cout << (f.warning ? "warning " : "error ") << f.code << " " << (f.item.empty() ? "library" : f.item) << ": " << f.message << "\n";
    }
    size_t items = 0;
    for (LibraryKind kind : LIBRARY_KINDS) items += TitleLibrary::get().items(kind).size();
    std::cout << "title-library verify: " << items << " items, " << errors << " errors, " << warnings << " warnings\n";
    return errors ? FINDINGS : OK;
}

int titlelibrary::runLintCommand(const std::vector<std::string>& args) {
    const Args a = parseArgs(args, {}, {"--json"});
    if (!a.error.empty()) {
        std::cerr << "title-library lint: " << a.error << "\n";
        return USAGE;
    }
    int failing = 0, checked = 0;
    for (const LibraryItemInfo& item : TitleLibrary::get().items(LibraryKind::Look)) {
        if (!a.positional.empty() && std::find(a.positional.begin(), a.positional.end(), item.ref) == a.positional.end()) continue;
        if (!item.available) continue;
        const Json raw = TitleLibrary::get().itemJson(item);
        std::set<std::string> allowed;
        if (raw.contains("lint_allow")) for (const Json& e : raw["lint_allow"]) allowed.insert(e.value("rule", std::string()));
        for (const ThumbnailSample& s : thumbnailSamples(item)) {
            titleart::TitleArtDoc doc;
            std::map<std::string, std::string> transient;
            std::string error;
            if (!thumbnailDocument(item, s, doc, transient, error)) {
                std::cout << "error " << item.ref << " (" << s.key << "): " << error << "\n";
                failing++;
                continue;
            }
            checked++;
            for (const StyleLintFinding& f : lintDocument(doc)) {
                if (allowed.count(f.rule)) continue;
                std::cout << item.ref << " (" << s.key << ") style_lint." << f.rule << " " << f.element << ": measured " << f.measured << ", threshold "
                          << f.threshold << ": " << f.message << "\n";
                failing++;
            }
        }
    }
    std::cout << "title-library lint: " << checked << " renders of looks linted, " << failing << " findings\n";
    return failing ? FINDINGS : OK;
}

int titlelibrary::runGenDocsCommand(const std::vector<std::string>& args) {
    const Args a = parseArgs(args, {"--out"}, {"--check"});
    if (!a.error.empty()) {
        std::cerr << "title-library gen-docs: " << a.error << "\n";
        return USAGE;
    }
    const fs::path out = a.values.count("--out") ? fs::path(a.values.at("--out")) : fs::path("docs/mcp/guide/title-library.md");
    const std::string text = libraryDocsMarkdown();
    if (a.flags.count("--check")) {
        std::string current;
        if (!readBytes(out, current) || current != text) {
            std::cout << out.string() << " is stale: run title-library gen-docs\n";
            return FINDINGS;
        }
        std::cout << out.string() << " is up to date\n";
        return OK;
    }
    if (!writeBytes(out, text)) {
        std::cerr << "title-library gen-docs: cannot write " << out.string() << "\n";
        return USAGE;
    }
    std::cout << "wrote " << out.string() << "\n";
    return OK;
}

bool titlelibrary::buildTemplateFromRecipe(const Json& r, TitleTemplate& t, std::string& error) {
    const std::string id = r.value("id", std::string());
    const Json texts = r.contains("texts") ? r["texts"] : Json::object();
    const std::string language = r.value("language", std::string("en"));
    titleart::NewTitleArtOptions o;
    if (r.contains("canvas") && r["canvas"].is_array() && r["canvas"].size() == 2) {
        o.width = r["canvas"][0].get<int>();
        o.height = r["canvas"][1].get<int>();
    }
    o.text = texts.contains("text") ? texts["text"].value(language, std::string("TITLE")) : std::string("TITLE");
    o.idSeed = 0x5384;                  // a fixed sequence: re-running gives the same ids
    const titleart::TitleArtDoc start = titleart::makeNewTitleArt(o);
    TitleOpsOptions options;
    options.idSeed = 0x5384;
    TitleOpsResult result = TitleArtOps::apply(start, r.contains("ops") ? r["ops"] : Json::array(), options);
    if (!result.ok) {
        error = result.error();
        return false;
    }
    t.doc = result.doc;
    // a stable guid per template: the FNV-1a of its id, as 32 hex
    uint64_t h1 = 1469598103934665603ull, h2 = 1099511628211ull;
    for (unsigned char c : id) {
        h1 = (h1 ^ c) * 1099511628211ull;
        h2 = (h2 ^ c) * 1469598103934665603ull + 0x9e3779b97f4a7c15ull;
    }
    char guid[33];
    std::snprintf(guid, sizeof(guid), "%016llx%016llx", (unsigned long long)h1, (unsigned long long)h2);
    t.doc.root["guid"] = std::string(guid);
    Json placeholders = Json::array();
    for (const Json& e : t.doc.root["exposed"]) {
        if (e.value("type", std::string()) != "text") continue;
        const std::string name = e.value("name", std::string());
        Json sample = Json{{"en", ""}, {"ja", ""}};
        if (texts.contains(name)) sample = Json{{"en", texts[name].value("en", std::string())}, {"ja", texts[name].value("ja", std::string())}};
        const char* role = name == "text" ? "title" : name == "subtitle" ? "subtitle" : name == "counter" ? "counter" : name == "kicker" ? "kicker" : "label";
        const size_t longest = std::max(sample["en"].get<std::string>().size(), sample["ja"].get<std::string>().size() / 3);
        placeholders.push_back(Json{{"name", name}, {"role", role}, {"sample", sample}, {"max_chars", std::max<size_t>(8, longest + 4)}});
    }
    t.meta = Json{{"version", TITLE_TEMPLATE_MEMBER_VERSION}, {"id", id}, {"revision", r.value("revision", 1)}, {"name", r.value("name", Json::object())},
                  {"description", r.value("description", Json::object())}, {"family", r.value("family", std::string("custom"))},
                  {"tags", r.value("tags", Json::array())}, {"synonyms", r.value("synonyms", Json::object())}, {"placeholders", placeholders},
                  {"canvas_note", r.value("canvas_note", std::string())}, {"provenance", r.value("provenance", Json{{"source", "jasperengine"}})}};
    for (const char* key : {"background", "check_allow", "lint_allow"}) if (r.contains(key)) t.meta[key] = r[key];
    return true;
}

int titlelibrary::runBuildTemplatesCommand(const std::vector<std::string>& args) {
    const Args a = parseArgs(args, {"--recipes", "--out"}, {"--check"});
    if (!a.error.empty()) {
        std::cerr << "title-library build-templates: " << a.error << "\n";
        return USAGE;
    }
    const fs::path recipes = a.values.count("--recipes") ? fs::path(a.values.at("--recipes")) : fs::path("tools/titleart/templates");
    const fs::path out = a.values.count("--out") ? fs::path(a.values.at("--out")) : fs::path("editor/resources/titleart/templates");
    std::vector<fs::path> files;
    std::error_code ec;
    if (fs::is_directory(recipes, ec)) {
        for (const auto& e : fs::directory_iterator(recipes, ec)) if (e.path().extension() == ".json") files.push_back(e.path());
    } else {
        files.push_back(recipes);
    }
    std::sort(files.begin(), files.end());
    int built = 0, problems = 0;
    for (const fs::path& file : files) {
        std::string text;
        if (!readBytes(file, text)) {
            std::cerr << "title-library build-templates: cannot read " << file.string() << "\n";
            return USAGE;
        }
        const Json parsed = Json::parse(text, nullptr, false);
        const Json list = parsed.is_array() ? parsed : (parsed.contains("templates") ? parsed["templates"] : Json::array({parsed}));
        for (const Json& r : list) {
            const std::string id = r.value("id", std::string());
            TitleTemplate t;
            std::string buildError;
            if (!buildTemplateFromRecipe(r, t, buildError)) {
                std::cout << "error " << id << ": " << buildError << "\n";
                problems++;
                continue;
            }
            const std::string written = writeTemplate(t);
            const fs::path target = out / (id + TITLE_TEMPLATE_EXTENSION);
            if (a.flags.count("--check")) {
                std::string current;
                if (!readBytes(target, current) || current != written) {
                    std::cout << "stale " << id << ": " << target.string() << "\n";
                    problems++;
                }
            } else if (!writeBytes(target, written)) {
                std::cerr << "title-library build-templates: cannot write " << target.string() << "\n";
                return USAGE;
            }
            built++;
        }
    }
    std::cout << "title-library build-templates: " << built << " templates, " << problems << " problems\n";
    return problems ? FINDINGS : OK;
}

// ---- GPU: thumbnails, review sheets, calibration ----

int titlelibrary::runBakeThumbnailsCommand(const std::vector<std::string>& args, const char* executable) {
    const Args a = parseArgs(args, {"--only", "--out", "--scratch"}, {"--check"});
    if (!a.error.empty()) {
        std::cerr << "title-library bake-thumbnails: " << a.error << "\n";
        return USAGE;
    }
    const fs::path out = a.values.count("--out") ? fs::path(a.values.at("--out")) : fs::path("editor/resources/titleart/thumbnails");
    const std::string only = a.values.count("--only") ? a.values.at("--only") : std::string();
    const auto work = thumbnailWork(only);
    if (a.flags.count("--check")) {
        int stale = 0;
        for (const auto& [item, sample] : work) {
            const fs::path png = out / libraryKindName(item->kind) / thumbName(*item, sample.key);
            const std::string have = readThumbnailRecipe(png);
            if (have.empty()) {
                std::cout << "missing " << item->ref << " (" << sample.key << "): " << png.string() << "\n";
                stale++;
            } else if (have != thumbnailRecipe(*item, sample)) {
                std::cout << "stale " << item->ref << " (" << sample.key << "): " << png.string() << "\n";
                stale++;
            }
        }
        std::cout << "title-library bake-thumbnails --check: " << work.size() << " thumbnails, " << stale << " stale or missing\n";
        return stale ? FINDINGS : OK;
    }
    Scratch s;
    s.dir = a.values.count("--scratch") ? fs::path(a.values.at("--scratch")) : fs::temp_directory_path() / "titleart-thumbnails";
    std::string error;
    if (!makeScratchProject(s.dir, error)) {
        std::cerr << "title-library bake-thumbnails: " << error << "\n";
        return USAGE;
    }
    std::vector<Planned> plan;
    std::vector<std::string> failures;
    if (!planDocuments(work, s, plan, failures)) {
        std::cerr << "title-library bake-thumbnails: cannot write the scratch documents\n";
        return USAGE;
    }
    for (const Planned& p : plan) {
        // a template fills its canvas (640 wide is plenty for a 240 px thumbnail, and the 3D ones
        // stay inside the renderer's target memory); a look is a word cropped out of the canvas
        const int width = p.item->kind == LibraryKind::Template ? 640 : 1280;
        s.calls.push_back(callLine("render_title", Json{{"path", "assets/" + p.doc}, {"save_path", p.png}, {"backgrounds", Json::array({"transparent"})},
                                                       {"image", "none"}, {"max_width", width}}));
    }
    const auto start = std::chrono::steady_clock::now();
    std::vector<Json> results;
    if (!plan.empty() && !runBatch(executable, s, results, error)) {
        std::cerr << "title-library bake-thumbnails: " << error << "\n";
        return FINDINGS;
    }
    size_t bytes = 0;
    int written = 0;
    for (const Planned& p : plan) {
        std::string png;
        if (!readBytes(s.dir / p.png, png)) {
            failures.push_back(p.item->ref + " (" + p.sample.key + "): no render");
            continue;
        }
        int w = 0, h = 0, n = 0;
        unsigned char* rgba = stbi_load_from_memory((const unsigned char*)png.data(), (int)png.size(), &w, &h, &n, 4);
        if (!rgba) {
            failures.push_back(p.item->ref + " (" + p.sample.key + "): unreadable render");
            continue;
        }
        const texsynth::Image thumb = frameThumbnail(rgba, w, h);
        stbi_image_free(rgba);
        const std::string file = encodeThumbnail(thumb, thumbnailRecipe(*p.item, p.sample));
        bytes += file.size();
        if (!writeBytes(out / libraryKindName(p.item->kind) / thumbName(*p.item, p.sample.key), file)) {
            failures.push_back(p.item->ref + ": cannot write");
            continue;
        }
        written++;
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    // R41: the staged thumbnails stay within their budget
    size_t budget = 16u * 1024u * 1024u;
    if (const char* env = std::getenv("JASPER_TITLEART_THUMB_BUDGET")) budget = (size_t)std::strtoull(env, nullptr, 10);
    size_t total = 0;
    std::error_code ec;
    if (fs::is_directory(out, ec)) {
        for (const auto& e : fs::recursive_directory_iterator(out, ec)) if (e.is_regular_file(ec)) total += e.file_size(ec);
    }
    for (const std::string& f : failures) std::cout << "failed " << f << "\n";
    std::cout << "title-library bake-thumbnails: " << written << " written (" << bytes << " bytes, " << seconds << " s), " << failures.size()
              << " failed; the folder holds " << total << " bytes of " << budget << "\n";
    if (total > budget) {
        std::cout << "over budget: the thumbnails take " << total << " bytes, JASPER_TITLEART_THUMB_BUDGET allows " << budget << "\n";
        return FINDINGS;
    }
    return failures.empty() ? OK : FINDINGS;
}

int titlelibrary::runReviewSheetCommand(const std::vector<std::string>& args, const char* executable) {
    const Args a = parseArgs(args, {"--pachislop", "--out", "--only"}, {});
    if (!a.error.empty() || !a.values.count("--pachislop") || !a.values.count("--out")) {
        std::cerr << "title-library review-sheet --pachislop <clone> --out <dir> [--only <glob>]" << (a.error.empty() ? "" : ": " + a.error) << "\n";
        return USAGE;
    }
    const fs::path clone = a.values.at("--pachislop"), out = a.values.at("--out");
    const std::string only = a.values.count("--only") ? a.values.at("--only") : std::string();
    std::vector<std::pair<const LibraryItemInfo*, ThumbnailSample>> work;
    std::vector<fs::path> refs;
    for (LibraryKind kind : {LibraryKind::Look, LibraryKind::Template}) {
        for (const LibraryItemInfo& item : TitleLibrary::get().items(kind)) {
            if (item.scope != LibraryScope::BuiltIn || (!only.empty() && !globMatch(only, item.ref))) continue;
            const Json raw = TitleLibrary::get().itemJson(item);
            const Json& identity = kind == LibraryKind::Template && raw.contains("template") ? raw["template"] : raw;
            if (!identity.contains("provenance") || identity["provenance"].value("source", std::string()) != "pachislop") continue;
            const Json source = identity["provenance"].value("item", Json());
            if (!source.is_string()) continue;          // a withheld id has no reference picture name
            const fs::path ref = clone / "tests" / "golden" / "refs" / (kind == LibraryKind::Look ? "look" : "template") / (source.get<std::string>() + ".png");
            // the reference's own text: 確定 for 2D looks, BIG for 3D, the template's sample
            ThumbnailSample sample = kind == LibraryKind::Template ? ThumbnailSample{"ja", ""} : ThumbnailSample{"review", item.dimension == "3d" ? "BIG" : "確定"};
            work.push_back({&item, sample});
            refs.push_back(ref);
        }
    }
    Scratch s;
    s.dir = out / "scratch";
    std::string error;
    if (!makeScratchProject(s.dir, error)) {
        std::cerr << "title-library review-sheet: " << error << "\n";
        return USAGE;
    }
    std::vector<Planned> plan;
    std::vector<std::string> failures;
    planDocuments(work, s, plan, failures);
    for (const Planned& p : plan) {
        s.calls.push_back(callLine("render_title", Json{{"path", "assets/" + p.doc}, {"save_path", p.png}, {"backgrounds", Json::array({"transparent"})},
                                                       {"image", "none"}, {"max_width", 960}}));
    }
    std::vector<Json> results;
    if (!plan.empty() && !runBatch(executable, s, results, error)) {
        std::cerr << "title-library review-sheet: " << error << "\n";
        return FINDINGS;
    }
    // ours | reference, side by side on a grey ground
    int sheets = 0;
    for (size_t i = 0; i < plan.size(); i++) {
        const Planned& p = plan[i];
        size_t index = 0;
        for (size_t k = 0; k < work.size(); k++) if (work[k].first == p.item) index = k;
        std::string ours, theirs;
        readBytes(s.dir / p.png, ours);
        readBytes(refs[index], theirs);
        int w1 = 0, h1 = 0, w2 = 0, h2 = 0, n = 0;
        unsigned char* a1 = ours.empty() ? nullptr : stbi_load_from_memory((const unsigned char*)ours.data(), (int)ours.size(), &w1, &h1, &n, 4);
        unsigned char* a2 = theirs.empty() ? nullptr : stbi_load_from_memory((const unsigned char*)theirs.data(), (int)theirs.size(), &w2, &h2, &n, 4);
        const int cell = 480, height = 270;
        texsynth::Image sheet = texsynth::Image::make(cell * 2 + 8, height, 4, 8);
        for (size_t px = 0; px < sheet.data.size(); px += 4) {
            sheet.data[px] = sheet.data[px + 1] = sheet.data[px + 2] = 64;
            sheet.data[px + 3] = 255;
        }
        auto place = [&](unsigned char* rgba, int w, int h, int ox) {
            if (!rgba) return;
            const texsynth::Image t = frameThumbnail(rgba, w, h);
            for (int y = 0; y < height; y++) {
                for (int x = 0; x < cell; x++) {
                    const int sx = x * THUMBNAIL_WIDTH / cell, sy = y * THUMBNAIL_HEIGHT / height;
                    const uint8_t* src = &t.data[((size_t)sy * THUMBNAIL_WIDTH + sx) * 4];
                    uint8_t* dst = &sheet.data[((size_t)y * sheet.width + x + ox) * 4];
                    const double al = src[3] / 255.0;
                    for (int c = 0; c < 3; c++) dst[c] = (uint8_t)std::lround(src[c] * al + dst[c] * (1 - al));
                }
            }
        };
        place(a1, w1, h1, 0);
        place(a2, w2, h2, cell + 8);
        if (a1) stbi_image_free(a1);
        if (a2) stbi_image_free(a2);
        if (!a2) failures.push_back(p.item->ref + ": no reference " + refs[index].string());
        std::vector<uint8_t> encoded;
        texsynth::encodePng(sheet, encoded);
        std::string name = p.item->ref;
        std::replace(name.begin(), name.end(), '/', '.');
        writeBytes(out / (name + ".png"), std::string(encoded.begin(), encoded.end()));
        sheets++;
    }
    for (const std::string& f : failures) std::cout << "note " << f << "\n";
    std::cout << "title-library review-sheet: " << sheets << " sheets (ours | Pachislop's reference) in " << out.string() << "\n";
    return OK;
}

int titlelibrary::runCalibrateCommand(const std::vector<std::string>& args, const char* executable) {
    const Args a = parseArgs(args, {"--out", "--red", "--scratch", "--samples", "--only", "--checks"}, {});
    if (!a.error.empty()) {
        std::cerr << "title-library calibrate: " << a.error << "\n";
        return USAGE;
    }
    const fs::path out = a.values.count("--out") ? fs::path(a.values.at("--out")) : fs::path("tools/titleart/calibration.json");
    const fs::path red = a.values.count("--red") ? fs::path(a.values.at("--red")) : fs::path("tools/mcp/tests/fixtures/title-library-red");
    Scratch s;
    s.dir = a.values.count("--scratch") ? fs::path(a.values.at("--scratch")) : fs::temp_directory_path() / "titleart-calibrate";
    std::string error;
    if (!makeScratchProject(s.dir, error)) {
        std::cerr << "title-library calibrate: " << error << "\n";
        return USAGE;
    }
    std::vector<Planned> plan;
    std::vector<std::string> failures;
    std::vector<std::pair<const LibraryItemInfo*, ThumbnailSample>> work;
    // --samples latin: one render per item (HERO, or a template's English sample), for a machine without a GPU
    const std::string samples = a.values.count("--samples") ? a.values.at("--samples") : std::string("all");
    for (const auto& w : thumbnailWork(a.values.count("--only") ? a.values.at("--only") : std::string())) {
        if (!w.first->available) continue;
        if (samples == "latin" && w.second.key != "latin" && w.second.key != "en") continue;
        work.push_back(w);
    }
    planDocuments(work, s, plan, failures);
    // the red controls: every fixture document (bad twins and their fixes)
    std::vector<std::string> redDocs;
    std::error_code ec;
    if (fs::is_directory(red, ec)) {
        for (const auto& e : fs::directory_iterator(red, ec)) {
            if (e.path().extension() != ".titleart") continue;
            const std::string name = "red/" + e.path().filename().string();
            fs::create_directories(s.dir / "assets" / "red", ec);
            fs::copy_file(e.path(), s.dir / "assets" / name, fs::copy_options::overwrite_existing, ec);
            redDocs.push_back(name);
        }
    }
    std::sort(redDocs.begin(), redDocs.end());
    // the gate's render size (§6.14.2)
    for (const Planned& p : plan) {
        const Json raw = TitleLibrary::get().itemJson(*p.item);
        const Json& identity = p.item->kind == LibraryKind::Template && raw.contains("template") ? raw["template"] : raw;
        // legibility is judged on the backgrounds the look is made for (an empty `background` is any)
        const std::string background = identity.value("background", std::string());
        const Json backgrounds = background == "dark" ? Json::array({"black", "checker"}) : background == "light" ? Json::array({"white", "checker"}) : Json::array({"black", "white", "checker"});
        Json arguments = Json{{"path", "assets/" + p.doc}, {"backgrounds", backgrounds}, {"render_scale", 0.25}};
        if (a.values.count("--checks")) {      // only these checks (ids, comma separated): a tuning run's
            Json ids = Json::array();
            std::istringstream list(a.values.at("--checks"));
            for (std::string id; std::getline(list, id, ',');) ids.push_back(id);
            arguments["checks"] = ids;
        }
        s.calls.push_back(callLine("check_title", arguments));
    }
    for (const std::string& d : redDocs) s.calls.push_back(callLine("check_title", Json{{"path", "assets/" + d}, {"backgrounds", Json::array({"black", "white", "checker"})}, {"render_scale", 0.25}}));
    // which render each call is, for a person triaging the raw results beside it
    Json planFile = Json::array();
    for (const Planned& p : plan) planFile.push_back(Json{{"path", "assets/" + p.doc}, {"ref", p.item->ref}, {"kind", libraryKindName(p.item->kind)}, {"sample", p.sample.key}});
    writeBytes(s.dir / "plan.json", planFile.dump() + "\n");
    std::vector<Json> results;
    if (!runBatch(executable, s, results, error)) {
        std::cerr << "title-library calibrate: " << error << "\n";
        return FINDINGS;
    }
    // per check id: the library's values per kind (allowlisted items left out, §6.14.3) and the value
    // of the check's own red control (bad twin)
    std::map<std::string, std::map<std::string, std::vector<double>>> library;
    std::map<std::string, std::pair<double, std::string>> worst;
    std::map<std::string, std::vector<double>> controls;
    std::map<std::string, Json> thresholds;
    std::map<std::string, std::string> statuses;
    for (size_t i = 0; i < results.size(); i++) {
        const Json& r = results[i];
        const Json structured = r.contains("structured") ? r["structured"] : r.value("result", Json::object());
        if (!structured.contains("checks")) continue;
        const bool isRed = i >= plan.size();
        const std::string redName = isRed && i - plan.size() < redDocs.size() ? redDocs[i - plan.size()].substr(4) : std::string();      // without "red/"
        if (isRed && redName.find("-bad") == std::string::npos) continue;
        std::set<std::string> allowed;
        if (!isRed) {
            const Json raw = TitleLibrary::get().itemJson(*plan[i].item);
            const Json& identity = plan[i].item->kind == LibraryKind::Template && raw.contains("template") ? raw["template"] : raw;
            if (identity.contains("check_allow")) for (const Json& a : identity["check_allow"]) allowed.insert(a.value("check", std::string()));
            if (identity.contains("lint_allow")) for (const Json& a : identity["lint_allow"]) allowed.insert("style_lint." + a.value("rule", std::string()));
        }
        for (const Json& c : structured["checks"]) {
            const std::string id = c.value("id", std::string());
            const CalibrationMetric* metric = calibrationMetric(id);
            if (!metric || c.value("status", std::string()) == "SKIP" || !c.contains("measured")) continue;
            const double v = metric->value(c["measured"]);
            if (std::isnan(v)) continue;
            thresholds[id] = c.value("threshold", Json());
            if (isRed) {
                if (redName.rfind(id + "-bad", 0) == 0 || redName.rfind(replaceDotted(id) + "-bad", 0) == 0) controls[id].push_back(v);
            } else if (!allowed.count(id) && !allowed.count(id.substr(0, id.find('.')))) {
                library[id][libraryKindName(plan[i].item->kind)].push_back(v);
                auto it = worst.find(id);
                if (it == worst.end() || (metric->higherIsBetter ? v < it->second.first : v > it->second.first)) worst[id] = {v, plan[i].item->ref};
            }
        }
    }
    auto stats = [](std::vector<double> v) {
        std::sort(v.begin(), v.end());
        auto at = [&](double q) { return v.empty() ? 0.0 : v[(size_t)std::min<double>(v.size() - 1, std::floor(q * (v.size() - 1) + 0.5))]; };
        return Json{{"n", v.size()}, {"min", v.empty() ? 0 : v.front()}, {"p5", at(0.05)}, {"median", at(0.5)}, {"p95", at(0.95)}, {"max", v.empty() ? 0 : v.back()}};
    };
    Json checks = Json::object();
    for (const auto& [id, kinds] : library) {
        const CalibrationMetric* metric = calibrationMetric(id);
        Json k = Json::object();
        std::vector<double> all;
        for (const auto& [kind, values] : kinds) {
            k[kind] = stats(values);
            all.insert(all.end(), values.begin(), values.end());
        }
        Json entry = Json{{"library", k}, {"threshold", thresholds[id]}, {"metric", metric->path}, {"higher_is_better", metric->higherIsBetter}};
        entry["L"] = worst[id].first;
        entry["L_item"] = worst[id].second;
        if (controls.count(id)) {
            std::vector<double>& red = controls[id];
            std::sort(red.begin(), red.end());
            const double R = metric->higherIsBetter ? red.back() : red.front();
            const double L = worst[id].first;
            const double T = metric->threshold(thresholds[id]);
            const bool separates = metric->higherIsBetter ? R < L : R > L;
            const double lo = std::min(L, R), hi = std::max(L, R), gap = hi - lo;
            entry["R"] = R;
            entry["T"] = T;
            entry["separates"] = separates;
            entry["middle_half"] = separates && lo + 0.25 * gap <= T && T <= hi - 0.25 * gap;
            entry["suggested_T"] = (lo + hi) / 2.0;
        }
        checks[id] = entry;
    }
    const Json file = Json{{"format", "jasperengine.titlelibrary.calibration"}, {"version", 1}, {"renders", plan.size()}, {"red_controls", redDocs.size()},
                           {"checks", checks}};
    if (!writeBytes(out, titleart::writeTitleArtJson(file) + "\n")) {
        std::cerr << "title-library calibrate: cannot write " << out.string() << "\n";
        return USAGE;
    }
    for (const std::string& f : failures) std::cout << "failed " << f << "\n";
    std::cout << "title-library calibrate: " << plan.size() << " library renders, " << redDocs.size() << " red controls, " << checks.size() << " checks -> "
              << out.string() << "\n";
    return failures.empty() ? OK : FINDINGS;
}
