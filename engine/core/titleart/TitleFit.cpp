// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleFit.h"

#include "TitleArtHash.h"
#include "TitleArtIO.h"
#include "geometry/TitleGeometry.h"
#include "text/TitleTextLayout.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <list>
#include <map>
#include <mutex>

using namespace jasperengine;
using namespace jasperengine::titleart;

namespace {

    constexpr double STEP = 1.0 / 256.0;
    constexpr int MAX_MEASURES = 9;
    constexpr size_t CACHE_ENTRIES = 256;

    double num(const Json& object, const char* key, double fallback) {
        auto it = object.find(key);
        return it != object.end() && it->is_number() ? it->get<double>() : fallback;
    }

    // ---- UTF-8 ----

    std::vector<uint32_t> decodeUtf8(const std::string& s) {
        std::vector<uint32_t> out;
        for (size_t i = 0; i < s.size();) {
            const unsigned char c = (unsigned char)s[i];
            uint32_t cp = c;
            int extra = 0;
            if (c >= 0xF0) { cp = c & 0x07; extra = 3; }
            else if (c >= 0xE0) { cp = c & 0x0F; extra = 2; }
            else if (c >= 0xC0) { cp = c & 0x1F; extra = 1; }
            i++;
            for (int k = 0; k < extra && i < s.size(); k++, i++) cp = (cp << 6) | ((unsigned char)s[i] & 0x3F);
            out.push_back(cp);
        }
        return out;
    }

    std::string encodeUtf8(const std::vector<uint32_t>& cps) {
        std::string out;
        for (uint32_t cp : cps) {
            if (cp < 0x80) {
                out += (char)cp;
            } else if (cp < 0x800) {
                out += (char)(0xC0 | (cp >> 6));
                out += (char)(0x80 | (cp & 0x3F));
            } else if (cp < 0x10000) {
                out += (char)(0xE0 | (cp >> 12));
                out += (char)(0x80 | ((cp >> 6) & 0x3F));
                out += (char)(0x80 | (cp & 0x3F));
            } else {
                out += (char)(0xF0 | (cp >> 18));
                out += (char)(0x80 | ((cp >> 12) & 0x3F));
                out += (char)(0x80 | ((cp >> 6) & 0x3F));
                out += (char)(0x80 | (cp & 0x3F));
            }
        }
        return out;
    }

    bool isSpace(uint32_t c) { return c == 0x20 || c == 0x3000; }

    bool isCjk(uint32_t c) {
        return (c >= 0x3040 && c <= 0x30FF) || (c >= 0x31F0 && c <= 0x31FF) || (c >= 0x3400 && c <= 0x4DBF) ||
               (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0xF900 && c <= 0xFAFF) || (c >= 0xFF66 && c <= 0xFF9F) ||
               (c >= 0x20000 && c <= 0x2FFFF) || (c >= 0x3001 && c <= 0x303F) || (c >= 0xFF01 && c <= 0xFF65) ||
               c == 0x201D || c == 0x2019 || c == 0x201C || c == 0x2018;
    }

    // R10's kinsoku table: never at the start of a line
    bool noBreakBefore(uint32_t c) {
        static const std::u32string closing = U"、。，．・：；？！ー）」』】〕〉》”’ぁぃぅぇぉっゃゅょゎァィゥェォッャュョヮヵヶ";
        return closing.find((char32_t)c) != std::u32string::npos;
    }

    // never at the end of a line
    bool noBreakAfter(uint32_t c) {
        static const std::u32string opening = U"（「『【〔〈《“‘";
        return opening.find((char32_t)c) != std::u32string::npos;
    }

    // ---- measuring ----

    struct Measurer {
        const TitleArtDoc& doc;
        TitleGeometry geometry;
        int measures = 0;

        Measurer(const TitleArtDoc& d, const AssetResolver* assets) : doc(d), geometry(assets) {}

        TitleVec2 extent(const Json& layer) {
            measures++;
            return measureWith(geometry, layer, doc);
        }

        static TitleVec2 measureWith(TitleGeometry& geometry, const Json& layer, const TitleArtDoc& doc) {
            double x0 = std::numeric_limits<double>::infinity(), y0 = x0, x1 = -x0, y1 = -x0;
            for (const GlyphOutline& o : geometry.outlines(layer, doc)) {
                const TitleRect b = o.path.bounds();
                if (b.width <= 0 && b.height <= 0) continue;
                x0 = std::min(x0, b.x);
                y0 = std::min(y0, b.y);
                x1 = std::max(x1, b.x + b.width);
                y1 = std::max(y1, b.y + b.height);
            }
            if (!(x1 >= x0)) return {0, 0};
            const std::vector<double> bands = geometry.rimBands(layer);
            const double grow = bands.empty() ? 0.0 : bands.back();
            // each rim's offset moves its band: the farthest offset grows that side
            double ox = 0, oy = 0;
            const double unit = layerSourceSize(layer) / 100.0;
            if (auto style = layer.find("style"); style != layer.end() && style->contains("rims") && (*style)["rims"].is_array()) {
                for (const Json& rim : (*style)["rims"]) {
                    if (!rim.value("enabled", true) || !(num(rim, "width", 0) > 0)) continue;
                    const TitleVec2 offset = decodeVec2(rim.value("offset", Json::array({0, 0})));
                    ox = std::max(ox, std::abs(offset.x) * unit);
                    oy = std::max(oy, std::abs(offset.y) * unit);
                }
            }
            const double gx = grow + ox, gy = grow + oy;
            return {2.0 * std::max(std::abs(x0 - gx), std::abs(x1 + gx)), 2.0 * std::max(std::abs(y0 - gy), std::abs(y1 + gy))};
        }
    };

    struct Box {
        double w = 0, h = 0;
        bool vertical = false;
        // an axis of 0 is not limited
        bool fits(const TitleVec2& e) const {
            return (w <= 0 || e.x <= w + 1e-9) && (h <= 0 || e.y <= h + 1e-9);
        }
        double overflow(const TitleVec2& e) const {
            return std::max(w > 0 ? e.x - w : 0.0, h > 0 ? e.y - h : 0.0);
        }
        // the scale the extent predicts (the extent scales with the size)
        double predict(const TitleVec2& e) const {
            double s = 1.0;
            if (w > 0 && e.x > 0) s = std::min(s, w / e.x);
            if (h > 0 && e.y > 0) s = std::min(s, h / e.y);
            return s;
        }
    };

    Json scaled(const Json& layer, double scale, double hScale) {
        Json copy = layer;
        copy["size"] = num(layer, "size", 200) * scale;
        copy["h_scale"] = num(layer, "h_scale", 1) * hScale;
        return copy;
    }

    // The largest value v in [lo, 1] (to 1/256) with fitsAt(v); `predicted` is probed first. False
    // in `fits` when not even `lo` fits (v = lo then)
    template<class Fn>
    double search(double lo, double predicted, int& budget, bool& fits, Fn&& fitsAt) {
        const auto quant = [](double v) { return std::floor(v / STEP + 1e-9) * STEP; };
        double good = -1, bad = 1.0;     // 1 is known not to fit (measured before)
        double probe = std::clamp(quant(predicted), lo, 1.0 - STEP);
        while (budget > 0) {
            budget--;
            if (fitsAt(probe)) {
                good = probe;
                if (bad - good <= STEP + 1e-9) break;
                probe = quant((good + bad) / 2);
                if (probe <= good) break;
            } else {
                bad = probe;
                if (probe <= lo + 1e-9) break;
                probe = good >= 0 ? quant((good + bad) / 2) : std::max(lo, quant((lo + bad) / 2));
                if (good >= 0 && probe <= good) break;
                if (probe >= bad) probe = std::max(lo, bad - STEP);
            }
        }
        fits = good >= 0;
        return fits ? good : lo;
    }

    // the line-break plan of `text`: break opportunities (code point index of the break, whether
    // it replaces a space) and their positions along the line
    struct BreakPoint {
        size_t index = 0;           // the code point the new line starts at
        bool atSpace = false;       // the code point at index - 1 is a space that becomes '\n'
    };

    std::vector<BreakPoint> breakPoints(const std::vector<uint32_t>& cps) {
        std::vector<BreakPoint> out;
        for (size_t i = 1; i < cps.size(); i++) {
            if (isSpace(cps[i - 1]) && i < cps.size() && !isSpace(cps[i]) && cps[i] != '\n') {
                out.push_back({i, true});
            } else if (cps[i - 1] != '\n' && cps[i] != '\n' && titleFitCanBreakBetween(cps[i - 1], cps[i])) {
                out.push_back({i, false});
            }
        }
        return out;
    }

    // the text with breaks at `chosen`; CharAdjust keys after an inserted newline move by one
    std::string wrapText(const std::vector<uint32_t>& cps, const std::vector<BreakPoint>& chosen, Json* chars) {
        std::vector<uint32_t> out;
        std::vector<size_t> inserted;
        size_t next = 0;
        for (size_t i = 0; i < cps.size(); i++) {
            if (next < chosen.size() && chosen[next].index == i) {
                if (chosen[next].atSpace && !out.empty()) {
                    out.back() = '\n';
                } else {
                    inserted.push_back(i);
                    out.push_back('\n');
                }
                next++;
            }
            out.push_back(cps[i]);
        }
        if (chars && chars->is_object() && !inserted.empty()) {
            Json moved = Json::object();
            for (auto it = chars->begin(); it != chars->end(); ++it) {
                const size_t key = (size_t)std::strtoull(it.key().c_str(), nullptr, 10);
                size_t shift = 0;
                for (size_t at : inserted) shift += key >= at ? 1 : 0;
                moved[std::to_string(key + shift)] = it.value();
            }
            *chars = std::move(moved);
        }
        return encodeUtf8(out);
    }

    // per code point: its position along the line (start and end of its cluster), from one layout
    struct LinePositions {
        std::vector<double> start, end;     // by code point index; NaN for code points without a cluster
        std::vector<uint32_t> line;
    };

    LinePositions positions(const LaidOutText& laid, size_t count, bool vertical) {
        LinePositions p;
        const double nan = std::numeric_limits<double>::quiet_NaN();
        p.start.assign(count, nan);
        p.end.assign(count, nan);
        p.line.assign(count, 0);
        for (const LaidOutCluster& c : laid.clusters) {
            const double s = vertical ? -c.pen.y : c.pen.x;
            for (size_t k = 0; k < std::max<size_t>(1, c.codepoints.size()); k++) {
                const size_t i = c.index + k;
                if (i >= count) break;
                p.start[i] = s;
                p.end[i] = s + c.advance;
                p.line[i] = c.line;
            }
        }
        return p;
    }

    // the longest line (layout units) when `cps` breaks at `chosen` (indexes into `points`)
    double longestLine(const std::vector<uint32_t>& cps, const LinePositions& pos, const std::vector<size_t>& cuts) {
        double longest = 0;
        size_t begin = 0;
        auto lineLength = [&](size_t a, size_t b) {
            double lo = std::numeric_limits<double>::infinity(), hi = -lo;
            for (size_t i = a; i < b; i++) {
                if (std::isnan(pos.start[i]) || isSpace(cps[i]) || cps[i] == '\n') continue;
                lo = std::min(lo, pos.start[i]);
                hi = std::max(hi, pos.end[i]);
            }
            return hi > lo ? hi - lo : 0.0;
        };
        std::vector<size_t> ends = cuts;
        // hard newlines end lines too
        for (size_t i = 0; i < cps.size(); i++) if (cps[i] == '\n') ends.push_back(i + 1);
        std::sort(ends.begin(), ends.end());
        ends.push_back(cps.size());
        for (size_t e : ends) {
            if (e <= begin) continue;
            longest = std::max(longest, lineLength(begin, e));
            begin = e;
        }
        return longest;
    }

    // the cuts (indexes of `points`) of `extra` soft breaks minimising the longest line: an exact
    // search over the small sets titles have, greedy refinement beyond
    std::vector<size_t> balancedBreaks(const std::vector<uint32_t>& cps, const LinePositions& pos, const std::vector<BreakPoint>& points,
                                       int extra) {
        std::vector<size_t> best;
        double bestLength = std::numeric_limits<double>::infinity();
        std::vector<size_t> pick;
        const size_t n = points.size();
        // exact: combinations of `extra` of n (titles have few break points)
        std::function<void(size_t)> visit = [&](size_t from) {
            if ((int)pick.size() == extra) {
                std::vector<size_t> cuts;
                for (size_t p : pick) cuts.push_back(points[p].index);
                const double l = longestLine(cps, pos, cuts);
                if (l < bestLength - 1e-9) {
                    bestLength = l;
                    best = pick;
                }
                return;
            }
            for (size_t i = from; i < n; i++) {
                pick.push_back(i);
                visit(i + 1);
                pick.pop_back();
            }
        };
        double combos = 1;
        for (int k = 0; k < extra; k++) combos *= (double)(n - k) / (k + 1);
        if (combos <= 20000) {
            visit(0);
            return best;
        }
        // greedy: equal shares of the total length
        const double total = longestLine(cps, pos, {});
        for (int k = 1; k <= extra; k++) {
            const double target = total * k / (extra + 1);
            size_t chosen = 0;
            double nearest = std::numeric_limits<double>::infinity();
            for (size_t i = 0; i < n; i++) {
                const double at = std::isnan(pos.start[points[i].index]) ? 0 : pos.start[points[i].index];
                if (std::abs(at - target) < nearest && (best.empty() || i > best.back())) {
                    nearest = std::abs(at - target);
                    chosen = i;
                }
            }
            best.push_back(chosen);
        }
        return best;
    }

    // ---- the cache ----

    struct FitCache {
        std::mutex lock;
        std::list<std::pair<uint64_t, TitleFitResult>> entries;
        std::map<uint64_t, std::list<std::pair<uint64_t, TitleFitResult>>::iterator> index;
    };

    FitCache& cache() {
        static FitCache* c = new FitCache();
        return *c;
    }

    uint64_t keyOf(const Json& layer, const AssetResolver* assets) {
        uint64_t h = fnv1a64(writeTitleArtJson(layer));
        return hashCombine(h, (uint64_t)(uintptr_t)assets);
    }

}

bool titleart::titleFitCanBreakBetween(uint32_t before, uint32_t after) {
    if (!(isCjk(before) && isCjk(after))) return false;
    if (noBreakBefore(after)) return false;
    if (noBreakAfter(before)) return false;
    return true;
}

TitleVec2 titleart::measureTitleTextExtent(const Json& layer, const TitleArtDoc& doc, const AssetResolver* assets) {
    TitleGeometry geometry(assets);
    return Measurer::measureWith(geometry, layer, doc);
}

TitleFitResult titleart::computeTitleFit(const Json& layer, const TitleArtDoc& doc, const AssetResolver* assets) {
    TitleFitResult result;
    const Json fit = layer.value("fit", Json::object());
    const std::string mode = fit.value("mode", std::string("none"));
    const TitleVec2 boxSize = decodeVec2(fit.value("box", Json::array({0, 0})));
    result.text = layer.value("text", std::string());
    result.box = boxSize;
    if (mode == "none" || (boxSize.x <= 0 && boxSize.y <= 0) || layerTypeOf(layer) != LayerType::Text) return result;
    result.active = true;
    const uint64_t key = keyOf(layer, assets);
    {
        FitCache& c = cache();
        std::lock_guard<std::mutex> guard(c.lock);
        if (auto it = c.index.find(key); it != c.index.end()) {
            c.entries.splice(c.entries.begin(), c.entries, it->second);
            return it->second->second;
        }
    }
    Box box;
    box.vertical = layer.value("vertical", false);
    box.w = boxSize.x;
    box.h = boxSize.y;
    // vertical text measures the column length against box[1]: the width limit is the box's too
    const double minScale = std::clamp(num(fit, "min_scale", 0.6), 0.05, 1.0);
    const double minH = std::clamp(num(fit, "min_h_scale", 0.8), 0.1, 1.0);
    const int maxLines = std::clamp((int)num(fit, "max_lines", 2), 1, 16);
    Measurer m(doc, assets);

    Json working = layer;
    TitleVec2 e = m.extent(working);
    if (mode == "wrap") {
        const std::vector<uint32_t> cps = decodeUtf8(result.text);
        const std::vector<BreakPoint> points = breakPoints(cps);
        int hardLines = 1;
        for (uint32_t c : cps) hardLines += c == '\n' ? 1 : 0;
        if (!box.fits(e) && !points.empty() && hardLines < maxLines) {
            TitleTextLayout layout(assets);
            const LaidOutText& laid = layout.layout(layer);
            const LinePositions pos = positions(laid, cps.size(), box.vertical);
            double bestScale = -1;
            for (int extra = 1; extra <= std::min<int>(maxLines - hardLines, (int)points.size()); extra++) {
                const std::vector<size_t> cuts = balancedBreaks(cps, pos, points, extra);
                std::vector<BreakPoint> chosen;
                for (size_t c : cuts) chosen.push_back(points[c]);
                Json candidate = layer;
                Json chars = layer.value("chars", Json::object());
                candidate["text"] = wrapText(cps, chosen, &chars);
                candidate["chars"] = chars;
                const TitleVec2 ce = m.extent(candidate);
                const double s = box.predict(ce);
                if (box.fits(ce)) {
                    working = candidate;
                    e = ce;
                    result.lines = hardLines + extra;
                    bestScale = 1;
                    break;
                }
                if (s > bestScale + 1e-9) {
                    bestScale = s;
                    working = candidate;
                    e = ce;
                    result.lines = hardLines + extra;
                }
            }
        } else {
            result.lines = hardLines;
        }
    }
    double hScale = 1.0;
    if (mode == "condense" && !box.fits(e)) {
        // ink scales with h_scale, the rims do not: predict from the measured ink width
        const double unit = layerSourceSize(layer) / 100.0;
        TitleGeometry& geo = m.geometry;
        const std::vector<double> bands = geo.rimBands(layer);
        const double grow = 2.0 * (bands.empty() ? 0.0 : bands.back()) + 0.0 * unit;
        const double predicted = box.w > 0 && e.x > grow ? (box.w - grow) / (e.x - grow) : 1.0;
        int budget = MAX_MEASURES - 1;
        bool fitsX = false;
        Box widthOnly = box;
        widthOnly.h = 0;
        hScale = search(minH, predicted, budget, fitsX, [&](double c) {
            const TitleVec2 ce = m.extent(scaled(working, 1.0, c));
            if (widthOnly.fits(ce)) e = ce;
            return widthOnly.fits(ce);
        });
        if (!fitsX) e = m.extent(scaled(working, 1.0, hScale));
    }
    double scale = 1.0;
    bool fits = box.fits(e);
    if (!fits) {
        int budget = MAX_MEASURES - 1;
        const TitleVec2 base = e;
        scale = search(minScale, box.predict(base), budget, fits, [&](double s) {
            const TitleVec2 se = m.extent(scaled(working, s, hScale));
            if (box.fits(se)) e = se;
            return box.fits(se);
        });
        if (!fits) e = m.extent(scaled(working, scale, hScale));
    }
    result.scale = scale;
    result.hScale = hScale;
    result.fits = fits;
    result.extent = e;
    result.overflowPx = std::max(0.0, box.overflow(e));
    result.measures = m.measures;
    result.text = working.value("text", result.text);
    if (working.contains("chars")) {
        // the wrapped text's CharAdjust keys travel in the result through the text itself: keep them
        result.text = working["text"].get<std::string>();
    }
    {
        FitCache& c = cache();
        std::lock_guard<std::mutex> guard(c.lock);
        c.entries.emplace_front(key, result);
        c.index[key] = c.entries.begin();
        while (c.entries.size() > CACHE_ENTRIES) {
            c.index.erase(c.entries.back().first);
            c.entries.pop_back();
        }
    }
    return result;
}

void titleart::applyTitleFit(Json& layer, const TitleFitResult& result) {
    if (!result.active) return;
    const std::string before = layer.value("text", std::string());
    if (result.text != before) {
        // re-run the wrap's CharAdjust move: newlines inserted (not replacing spaces) shift keys
        const std::vector<uint32_t> a = decodeUtf8(before), b = decodeUtf8(result.text);
        if (b.size() > a.size() && layer.contains("chars") && layer["chars"].is_object()) {
            std::vector<size_t> inserted;
            for (size_t i = 0, j = 0; j < b.size(); j++) {
                if (i < a.size() && (a[i] == b[j] || (b[j] == '\n' && isSpace(a[i])))) {
                    i++;
                } else {
                    inserted.push_back(i);
                }
            }
            Json moved = Json::object();
            for (auto it = layer["chars"].begin(); it != layer["chars"].end(); ++it) {
                const size_t key = (size_t)std::strtoull(it.key().c_str(), nullptr, 10);
                size_t shift = 0;
                for (size_t at : inserted) shift += key >= at ? 1 : 0;
                moved[std::to_string(key + shift)] = it.value();
            }
            layer["chars"] = std::move(moved);
        }
        layer["text"] = result.text;
    }
    layer["size"] = num(layer, "size", 200) * result.scale;
    layer["h_scale"] = num(layer, "h_scale", 1) * result.hScale;
}

namespace {

    bool layerHasFit(const Json& layer) {
        if (layerTypeOf(layer) != LayerType::Text) return false;
        auto fit = layer.find("fit");
        if (fit == layer.end() || !fit->is_object() || fit->value("mode", std::string("none")) == "none") return false;
        const TitleVec2 box = decodeVec2(fit->value("box", Json::array({0, 0})));
        return box.x > 0 || box.y > 0;
    }

    void fitList(Json& layers, TitleArtDoc& doc, const AssetResolver* assets, std::vector<std::pair<std::string, TitleFitResult>>& out) {
        if (!layers.is_array()) return;
        for (Json& layer : layers) {
            if (layerHasFit(layer) && layer.value("visible", true)) {
                const TitleFitResult r = computeTitleFit(layer, doc, assets);
                applyTitleFit(layer, r);
                out.emplace_back(layer.value("id", std::string()), r);
            }
            if (auto children = layer.find("children"); children != layer.end()) fitList(*children, doc, assets, out);
        }
    }

    bool anyFit(const Json& layers) {
        if (!layers.is_array()) return false;
        for (const Json& layer : layers) {
            if (layerHasFit(layer)) return true;
            if (auto children = layer.find("children"); children != layer.end() && anyFit(*children)) return true;
        }
        return false;
    }

}

std::vector<std::pair<std::string, TitleFitResult>> titleart::applyTitleFits(TitleArtDoc& doc, const AssetResolver* assets) {
    std::vector<std::pair<std::string, TitleFitResult>> out;
    if (!doc.root.contains("layers")) return out;
    // the layers are fitted in a copy's tree; `doc` stays what the geometry reads meanwhile
    TitleArtDoc reading = doc;
    fitList(doc.root["layers"], reading, assets, out);
    return out;
}

bool titleart::titleHasFit(const Json& root) {
    auto layers = root.find("layers");
    return layers != root.end() && anyFit(*layers);
}

void titleart::clearTitleFitCache() {
    FitCache& c = cache();
    std::lock_guard<std::mutex> guard(c.lock);
    c.entries.clear();
    c.index.clear();
}
