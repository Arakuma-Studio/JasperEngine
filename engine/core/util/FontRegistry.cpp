// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "FontRegistry.h"

#include "DefaultFont.h"
#include "DefaultFontArabic.h"
#include "Log.h"
#include "util/TitleArtLanguage.h"

#include <algorithm>
#include <cstdlib>
#include <map>
#include <mutex>

using namespace jasperengine;

namespace {

    const char* builtInLatinFamily = "Roboto";

    struct Entry {
        std::string family;   // normalized
        int weight;
        bool italic;
        std::shared_ptr<FontFace> face;
    };

    struct Registry {
        std::recursive_mutex mutex;
        bool builtInsRegistered = false;
        std::shared_ptr<FontFace> builtInLatin;
        std::shared_ptr<FontFace> builtInArabic;
        std::vector<Entry> entries;
        std::vector<std::shared_ptr<FontFace>> fallbacks;
        std::map<std::string, std::weak_ptr<FontFace>> files;
        std::vector<std::pair<std::string, std::vector<std::shared_ptr<FontFace>>>> languageFaces;   // registration order
    };

    Registry& registry(){
        //never destroyed: faces may still be held by statics destroyed after it
        static Registry* instance = new Registry();
        return *instance;
    }

    void addEntry(Registry& reg, const std::shared_ptr<FontFace>& face, const std::string& family, int weight, bool italic){
        for (Entry& entry : reg.entries){
            if (entry.family == family && entry.weight == weight && entry.italic == italic){
                entry.face = face;
                return;
            }
        }
        reg.entries.push_back({family, weight, italic, face});
    }

    void ensureBuiltIns(Registry& reg){
        if (reg.builtInsRegistered)
            return;
        reg.builtInsRegistered = true;

        std::string error;
        reg.builtInLatin = FontFace::loadMemory(roboto_regular_ttf, roboto_regular_ttf_len, 0, false, "built-in Roboto", &error);
        if (!reg.builtInLatin)
            Log::error("%s", error.c_str());
        reg.builtInArabic = FontFace::loadMemory(noto_sans_arabic_ttf, noto_sans_arabic_ttf_len, 0, false, "built-in Noto Sans Arabic", &error);
        if (!reg.builtInArabic)
            Log::error("%s", error.c_str());

        if (reg.builtInLatin)
            addEntry(reg, reg.builtInLatin, FontRegistry::normalizeFamily(builtInLatinFamily), 400, false);
        if (reg.builtInArabic)
            addEntry(reg, reg.builtInArabic, FontRegistry::normalizeFamily(reg.builtInArabic->getFamilyName()),
                     reg.builtInArabic->getWeight(), reg.builtInArabic->isItalic());
    }

    // CSS Fonts 4 §5.2 step 4, font-weight
    const Entry* matchWeight(const std::vector<const Entry*>& candidates, int desired){
        const Entry* best = nullptr;

        // descending: the heaviest of the range first; else the lightest first
        auto pick = [&](auto inRange, bool descending) -> const Entry* {
            const Entry* found = nullptr;
            for (const Entry* entry : candidates){
                if (!inRange(entry->weight))
                    continue;
                if (!found || (descending ? entry->weight > found->weight : entry->weight < found->weight))
                    found = entry;
            }
            return found;
        };

        if (desired >= 400 && desired <= 500){
            // desired..500 ascending, then below desired descending, then above 500 ascending
            best = pick([&](int w){ return w >= desired && w <= 500; }, false);
            if (!best) best = pick([&](int w){ return w < desired; }, true);
            if (!best) best = pick([&](int w){ return w > 500; }, false);
        }else if (desired < 400){
            // desired and lighter descending, then heavier ascending
            best = pick([&](int w){ return w <= desired; }, true);
            if (!best) best = pick([&](int w){ return w > desired; }, false);
        }else{
            // desired and heavier ascending, then lighter descending
            best = pick([&](int w){ return w >= desired; }, false);
            if (!best) best = pick([&](int w){ return w < desired; }, true);
        }

        return best;
    }

}

std::string FontRegistry::normalizeFamily(const std::string& family){
    size_t start = 0;
    size_t end = family.size();

    auto isTrimmed = [](char c){
        return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '"' || c == '\'';
    };
    while (start < end && isTrimmed(family[start])) start++;
    while (end > start && isTrimmed(family[end - 1])) end--;

    std::string out = family.substr(start, end - start);
    for (char& c : out){
        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
    }
    return out;
}

void FontRegistry::registerFace(std::shared_ptr<FontFace> face, const std::string& family, int weight, int italic, bool fallback){
    if (!face){
        Log::error("FontRegistry::registerFace: no face");
        return;
    }

    Registry& reg = registry();
    std::lock_guard<std::recursive_mutex> lock(reg.mutex);
    ensureBuiltIns(reg);

    std::string name = normalizeFamily(family.empty() ? face->getFamilyName() : family);
    int faceWeight = (weight > 0) ? std::min(weight, 1000) : face->getWeight();
    bool faceItalic = (italic < 0) ? face->isItalic() : (italic != 0);

    addEntry(reg, face, name, faceWeight, faceItalic);

    if (fallback && std::find(reg.fallbacks.begin(), reg.fallbacks.end(), face) == reg.fallbacks.end()){
        reg.fallbacks.push_back(face);
    }
}

std::shared_ptr<FontFace> FontRegistry::find(const std::string& family, int weight, bool italic){
    Registry& reg = registry();
    std::lock_guard<std::recursive_mutex> lock(reg.mutex);
    ensureBuiltIns(reg);

    std::string name = isGenericFamily(family) ? normalizeFamily(builtInLatinFamily) : normalizeFamily(family);
    if (weight <= 0)
        weight = 400;

    std::vector<const Entry*> sameStyle;
    std::vector<const Entry*> otherStyle;
    for (const Entry& entry : reg.entries){
        if (entry.family != name)
            continue;
        (entry.italic == italic ? sameStyle : otherStyle).push_back(&entry);
    }

    const Entry* best = matchWeight(sameStyle.empty() ? otherStyle : sameStyle, weight);
    return best ? best->face : nullptr;
}

bool FontRegistry::hasFamily(const std::string& family){
    if (isGenericFamily(family))
        return true;

    Registry& reg = registry();
    std::lock_guard<std::recursive_mutex> lock(reg.mutex);
    ensureBuiltIns(reg);

    std::string name = normalizeFamily(family);
    for (const Entry& entry : reg.entries){
        if (entry.family == name)
            return true;
    }
    return false;
}

bool FontRegistry::isGenericFamily(const std::string& family){
    // (emoji, math and ui-rounded: CSS Fonts 4, phase 3; they map to the built-in faces too)
    static const char* generics[] = {"sans-serif", "serif", "monospace", "system-ui", "ui-sans-serif",
                                     "ui-serif", "ui-monospace", "ui-rounded", "cursive", "fantasy", "emoji", "math"};

    std::string name = normalizeFamily(family);
    for (const char* generic : generics){
        if (name == generic)
            return true;
    }
    return false;
}

const std::vector<std::shared_ptr<FontFace>>& FontRegistry::getFallbackFaces(){
    Registry& reg = registry();
    std::lock_guard<std::recursive_mutex> lock(reg.mutex);
    return reg.fallbacks;
}

void FontRegistry::registerLanguageFace(const std::string& language, std::shared_ptr<FontFace> face){
    if (!face || language.empty())
        return;
    Registry& reg = registry();
    std::lock_guard<std::recursive_mutex> lock(reg.mutex);
    const std::string code = TitleArtLanguage::canonicalize(language);
    for (auto& entry : reg.languageFaces){
        if (entry.first != code)
            continue;
        if (std::find(entry.second.begin(), entry.second.end(), face) == entry.second.end())
            entry.second.push_back(std::move(face));
        return;
    }
    reg.languageFaces.push_back({code, {std::move(face)}});
}

void FontRegistry::clearLanguageFaces(){
    Registry& reg = registry();
    std::lock_guard<std::recursive_mutex> lock(reg.mutex);
    reg.languageFaces.clear();
}

std::string FontRegistry::matchLanguage(const std::string& language){
    Registry& reg = registry();
    std::lock_guard<std::recursive_mutex> lock(reg.mutex);
    if (language.empty() || reg.languageFaces.empty())
        return std::string();
    std::vector<std::string> available;
    for (const auto& entry : reg.languageFaces) available.push_back(entry.first);
    return TitleArtLanguage::match(language, available);
}

std::vector<std::shared_ptr<FontFace>> FontRegistry::getFallbackFaces(const std::string& language, int weight, bool italic){
    Registry& reg = registry();
    std::lock_guard<std::recursive_mutex> lock(reg.mutex);
    if (weight <= 0)
        weight = 400;

    std::vector<std::shared_ptr<FontFace>> faces;
    const std::string matched = matchLanguage(language);
    for (const auto& entry : reg.languageFaces){
        if (entry.first == matched) faces.insert(faces.end(), entry.second.begin(), entry.second.end());
    }
    for (const std::shared_ptr<FontFace>& fallback : reg.fallbacks){
        std::shared_ptr<FontFace> chosen = fallback;
        const Entry* own = nullptr;
        for (const Entry& entry : reg.entries){
            if (entry.face == fallback){
                own = &entry;
                break;
            }
        }
        if (own){
            std::vector<const Entry*> sameStyle;
            std::vector<const Entry*> otherStyle;
            for (const Entry& entry : reg.entries){
                if (entry.family == own->family)
                    (entry.italic == italic ? sameStyle : otherStyle).push_back(&entry);
            }
            if (const Entry* best = matchWeight(sameStyle.empty() ? otherStyle : sameStyle, weight))
                chosen = best->face;
        }
        if (std::find(faces.begin(), faces.end(), chosen) == faces.end())
            faces.push_back(chosen);
    }
    return faces;
}

std::shared_ptr<FontFace> FontRegistry::getBuiltInLatin(){
    Registry& reg = registry();
    std::lock_guard<std::recursive_mutex> lock(reg.mutex);
    ensureBuiltIns(reg);
    return reg.builtInLatin;
}

std::shared_ptr<FontFace> FontRegistry::getBuiltInArabic(){
    Registry& reg = registry();
    std::lock_guard<std::recursive_mutex> lock(reg.mutex);
    ensureBuiltIns(reg);
    return reg.builtInArabic;
}

std::shared_ptr<FontFace> FontRegistry::getOrLoadFile(const std::string& path, int faceIndex, std::string* error){
    Registry& reg = registry();
    std::lock_guard<std::recursive_mutex> lock(reg.mutex);

    std::string key = path + "#" + std::to_string(faceIndex);
    auto it = reg.files.find(key);
    if (it != reg.files.end()){
        if (std::shared_ptr<FontFace> face = it->second.lock())
            return face;
    }

    std::shared_ptr<FontFace> face = FontFace::loadFile(path, faceIndex, error);
    if (face){
        reg.files[key] = face;
    }else if (it != reg.files.end()){
        reg.files.erase(it);
    }
    return face;
}

int FontRegistry::reloadFile(const std::string& path, std::string* error, std::vector<std::shared_ptr<FontFace>>* replacedFaces){
    Registry& reg = registry();
    std::lock_guard<std::recursive_mutex> lock(reg.mutex);

    int replaced = 0;
    const std::string prefix = path + "#";
    for (auto& file : reg.files){
        if (file.first.compare(0, prefix.size(), prefix) != 0)
            continue;
        std::shared_ptr<FontFace> oldFace = file.second.lock();
        if (!oldFace)
            continue;

        const int faceIndex = std::atoi(file.first.c_str() + prefix.size());
        std::shared_ptr<FontFace> newFace = FontFace::loadFile(path, faceIndex, error);
        if (!newFace)
            continue;

        for (Entry& entry : reg.entries){
            if (entry.face == oldFace)
                entry.face = newFace;
        }
        std::replace(reg.fallbacks.begin(), reg.fallbacks.end(), oldFace, newFace);
        file.second = newFace;
        if (replacedFaces)
            replacedFaces->push_back(oldFace);
        replaced++;
    }
    return replaced;
}

void FontRegistry::clear(){
    Registry& reg = registry();
    std::lock_guard<std::recursive_mutex> lock(reg.mutex);

    reg.entries.clear();
    reg.fallbacks.clear();
    reg.languageFaces.clear();
    reg.files.clear();
    reg.builtInLatin.reset();
    reg.builtInArabic.reset();
    reg.builtInsRegistered = false;
}
