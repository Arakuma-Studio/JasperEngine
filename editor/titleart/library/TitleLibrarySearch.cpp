// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleLibrarySearch.h"

#include "search/SearchScorer.h"
#include "titleart/TitleEffectRegistry.h"
#include "titleart/designer/TitleSearchFold.h"

#include <algorithm>
#include <map>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::titlelibrary;

namespace {

    // §6.13: a derived term's English phrase and Japanese word
    struct TermWords {
        const char* term;
        const char* en;
        const char* ja;
    };
    const TermWords TERMS[] = {
        {"bevel", "bevel emboss", "ベベル 立体"},
        {"knockout", "hollow knockout", "中抜き"},
        {"erode", "erode worn dry", "かすれ"},
        {"glass", "glass", "ガラス"},
        {"reflection", "reflection reflective", "反射"},
        {"finish", "gradient map metal finish", "グラデーションマップ"},
        {"3d", "3d", "立体"},
        {"inflate", "puffy inflate balloon", "ぷっくり"},
        {"extrude", "extrude extruded", "押し出し"},
        {"generators", "generators", "ジェネレーター"},
        {"spectrum", "rainbow spectrum", "虹"},
        {"light_glow", "light glow power glow", "ライトグロー"},
        {"outer_glow", "outer glow", "外側の光彩"},
        {"inner_glow", "inner glow", "内側の光彩"},
        {"drop_shadow", "drop shadow", "ドロップシャドウ"},
        {"inner_shadow", "inner shadow", "シャドウ（内側）"},
        {"outline", "outline keyline", "縁取り"},
        {"perspective_shadow", "perspective shadow", "パースシャドウ"},
        {"extrude_2d", "long shadow", "ロングシャドウ"},
        {"extrude", "long shadow extrude", "ロングシャドウ"},
        {"drips", "drips dripping", "垂れ"},
        {"scanlines", "scanlines crt", "走査線"},
        {"pixelate", "pixel pixelate 8bit", "ドット"},
        {"halftone", "halftone screen tone", "スクリーントーン"},
        {"glitch", "glitch", "グリッチ"},
        {"chromatic_aberration", "chromatic aberration", "色収差"},
        {"grain", "grain noise", "ノイズ"},
        {"holographic", "holographic hologram foil", "ホログラム"},
        {"chalk", "chalk", "チョーク"},
        {"distress", "distressed scratched", "ダメージ"},
        {"motion_trail", "motion trail speed", "残像"},
        {"contour_lines", "contour lines light guide", "導光板"},
        {"pattern:zebra", "zebra", "ゼブラ"},
        {"pattern:giraffe", "giraffe", "キリン"},
        {"pattern:leopard", "leopard", "ヒョウ"},
        {"pattern:tiger", "tiger", "トラ"},
        {"pattern:checker", "checker checkered", "市松"},
        {"pattern:dots", "dots polka", "水玉"},
        {"pattern:stripes", "stripes striped", "ストライプ"},
        {"pattern:sakura", "sakura cherry blossom", "桜"},
        {"pattern:seigaiha", "seigaiha waves", "青海波"},
        {"pattern:hazard", "hazard caution", "警告"},
        {"texture:brushed", "brushed", "ヘアライン"},
        {"texture:grunge", "grunge", "汚し"},
        {"texture:crumple", "crumpled foil", "しわ"},
        {"texture:hammered", "hammered", "槌目"},
        {"texture:sparkle", "sparkle glitter", "ラメ"},
        {"texture:flakes", "flakes glitter", "ラメ"},
        {"texture:liquid", "liquid", "液体"},
        {"texture:noise", "noise", "ノイズ"},
        {"texture:clouds", "clouds", "雲"},
        {"generator:glints", "glints sparkles stars", "キラキラ"},
        {"generator:fire", "fire flames", "炎"},
        {"generator:smoke", "smoke", "煙"},
        {"generator:particles", "particles", "パーティクル"},
        {"generator:speed_lines", "speed lines", "集中線"},
        {"generator:sunburst", "sunburst rays", "放射"},
        {"generator:lightning", "lightning electric", "稲妻"},
        {"generator:aura", "aura", "オーラ"},
        {"generator:backlight", "backlight god rays", "後光"},
        {"generator:lens_flare", "lens flare", "レンズフレア"},
        {"generator:light_streaks", "light streaks", "光線"},
        {"generator:magic_circle", "magic circle", "魔法陣"},
        {"generator:ink_splatter", "ink splatter", "墨しぶき"},
        {"generator:brush_swirl", "brush swirl", "筆"},
        {"generator:shockwave", "shockwave", "衝撃波"},
    };

    const std::map<std::string, const TermWords*>& termTable() {
        static const std::map<std::string, const TermWords*> table = [] {
            std::map<std::string, const TermWords*> t;
            for (const TermWords& w : TERMS) t.emplace(w.term, &w);
            return t;
        }();
        return table;
    }

    std::vector<std::string> wordsOf(const std::string& folded) {
        std::vector<std::string> out;
        std::istringstream in(folded);
        std::string w;
        while (in >> w) out.push_back(w);
        return out;
    }

    bool asciiWord(const std::string& w) {
        for (unsigned char c : w) if (c >= 0x80) return false;
        return true;
    }

    struct Field {
        std::string folded;
        std::vector<std::string> words;
        int penalty = 0;
    };

    bool wordMatches(const std::string& q, const Field& f) {
        if (f.folded.find(q) != std::string::npos) return true;
        for (const std::string& w : f.words) {
            if (w.rfind(q, 0) == 0) return true;
            if (asciiWord(q) && q.size() >= 5 && search::damerau(q, w, 1) <= 1) return true;
        }
        return false;
    }

    std::vector<Field> fieldsOf(const LibraryItemInfo& item) {
        std::vector<Field> fields;
        auto add = [&](const std::string& text, int penalty) {
            if (text.empty()) return;
            Field f;
            f.folded = titledesigner::foldForSearch(text);
            f.words = wordsOf(f.folded);
            f.penalty = penalty;
            fields.push_back(std::move(f));
        };
        add(item.name.en, 0);
        add(item.name.ja, 0);
        add(item.ref, 40);
        add(item.synonymsText.en, 80);
        add(item.synonymsText.ja, 80);
        for (const std::string& t : item.tags) add(t, 80);
        std::vector<std::string> families = item.familyHint;
        if (!item.family.empty()) families.push_back(item.family);
        for (const std::string& f : families) {
            add(f, 80);
            if (const LibraryFamily* fam = TitleLibrary::get().family(f)) {
                add(fam->name.en, 80);
                add(fam->name.ja, 80);
            }
        }
        add(item.dimension, 80);
        for (const std::string& w : derivedTermWords(item)) add(w, 80);
        return fields;
    }

}

std::vector<std::string> titlelibrary::derivedTermWords(const LibraryItemInfo& item) {
    std::vector<std::string> out;
    std::vector<std::string> terms = item.features;
    for (const std::string& u : item.uses) if (u.rfind("generator:", 0) == 0) terms.push_back(u);
    for (const std::string& term : terms) {
        auto it = termTable().find(term);
        if (it != termTable().end()) {
            out.push_back(it->second->en);
            out.push_back(it->second->ja);
            continue;
        }
        // an effect without a phrase of its own: its registry label ("Outer Glow")
        if (const titleart::EffectDef* def = titleart::findEffect(term)) out.push_back(def->label);
        else {
            std::string words = term;
            for (char& c : words) if (c == '_' || c == ':') c = ' ';
            out.push_back(words);
        }
    }
    return out;
}

std::vector<std::string> titlelibrary::searchFields(const LibraryItemInfo& item) {
    std::vector<std::string> out;
    for (const Field& f : fieldsOf(item)) out.push_back(f.folded);
    return out;
}

std::vector<LibraryHit> titlelibrary::searchLibrary(const LibraryQuery& query) {
    std::vector<LibraryHit> hits;
    const std::vector<std::string> words = wordsOf(titledesigner::foldForSearch(query.text));
    for (LibraryKind kind : LIBRARY_KINDS) {
        if (query.kind && *query.kind != kind) continue;
        for (const LibraryItemInfo& item : TitleLibrary::get().items(kind)) {
            if (!query.family.empty() && item.family != query.family &&
                std::find(item.familyHint.begin(), item.familyHint.end(), query.family) == item.familyHint.end()) continue;
            if (!query.dimension.empty() && item.dimension != query.dimension) continue;
            if (query.scope && item.scope != *query.scope) continue;
            if (query.availableOnly && !item.available) continue;
            if (query.favoritesOnly && std::find(query.favorites.begin(), query.favorites.end(), item.ref) == query.favorites.end()) continue;
            bool tagsOk = true;
            for (const std::string& t : query.tags) tagsOk = tagsOk && std::find(item.tags.begin(), item.tags.end(), t) != item.tags.end();
            if (!tagsOk) continue;
            int score = 0;
            bool all = true;
            if (!words.empty()) {
                const std::vector<Field> fields = fieldsOf(item);
                for (const std::string& w : words) {
                    int best = -1;
                    for (const Field& f : fields) {
                        if (!wordMatches(w, f)) continue;
                        best = std::max(best, std::max(0, search::fuzzyScore(w, f.folded)) + 200 - f.penalty);
                    }
                    if (best < 0) {
                        all = false;
                        break;
                    }
                    score += best;
                }
            }
            if (!all) continue;
            hits.push_back({&item, score});
        }
    }
    std::stable_sort(hits.begin(), hits.end(), [](const LibraryHit& a, const LibraryHit& b) {
        if (a.score != b.score) return a.score > b.score;
        return a.item->name.any() < b.item->name.any();
    });
    return hits;
}
