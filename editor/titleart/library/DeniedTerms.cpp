// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "DeniedTerms.h"

#include "TitleLibraryFormats.h"
#include "title_denied_terms.h"

#include <algorithm>
#include <mutex>
#include <set>

using namespace jasperengine;
using namespace jasperengine::editor::titlelibrary;

namespace {

    bool asciiWordChar(unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c >= 0x80;
    }

    // lowercase words, single spaces between them, a space at both ends (so " word " finds a whole word)
    std::string foldWords(std::string_view text) {
        std::string out = " ";
        for (char ch : text) {
            const unsigned char c = (unsigned char)ch;
            if (asciiWordChar(c)) {
                out += (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : ch;
            } else if (out.back() != ' ') {
                out += ' ';
            }
        }
        if (out.back() != ' ') out += ' ';
        return out;
    }

    std::string lowerAscii(std::string_view text) {
        std::string out(text);
        for (char& c : out) {
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        }
        return out;
    }

    std::string trim(const std::string& s) {
        size_t b = 0, e = s.size();
        while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r')) b++;
        while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r')) e--;
        return s.substr(b, e - b);
    }

    struct Term {
        std::string text;       // as listed (lowercase)
        std::string folded;     // " a b " for Latin terms, the text itself for CJK ones
        bool substring = false;
    };

    struct Store {
        std::once_flag once;
        std::vector<std::string> terms;
        std::vector<Term> compiled;

        void set(const std::string& text) {
            terms = parseDeniedTerms(text);
            compiled.clear();
            for (const std::string& t : terms) {
                Term c;
                c.text = t;
                c.substring = hasJapaneseScript(t);
                c.folded = c.substring ? t : foldWords(t);
                compiled.push_back(std::move(c));
            }
        }
    };

    Store& store() {
        static Store* s = new Store();
        std::call_once(s->once, [] { s->set(std::string(reinterpret_cast<const char*>(title_denied_terms_txt), title_denied_terms_txt_size)); });
        return *s;
    }

    void walk(const Json& value, const std::string& path, std::vector<DeniedTermHit>& hits) {
        std::string term;
        if (value.is_string()) {
            if (containsDeniedTerm(value.get_ref<const std::string&>(), &term)) hits.push_back({path, term});
        } else if (value.is_object()) {
            for (auto it = value.begin(); it != value.end(); ++it) {
                const std::string at = path.empty() ? it.key() : path + "." + it.key();
                if (containsDeniedTerm(it.key(), &term)) hits.push_back({at + "{key}", term});
                walk(it.value(), at, hits);
            }
        } else if (value.is_array()) {
            for (size_t i = 0; i < value.size(); i++) walk(value[i], path + "[" + std::to_string(i) + "]", hits);
        }
    }

}

namespace jasperengine::editor::titlelibrary {

std::vector<std::string> parseDeniedTerms(const std::string& text) {
    std::vector<std::string> out;
    std::set<std::string> seen;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(start, end - start);
        const size_t hash = line.find('#');
        if (hash != std::string::npos) line = line.substr(0, hash);
        line = lowerAscii(trim(line));
        if (!line.empty() && seen.insert(line).second) out.push_back(line);
        start = end + 1;
    }
    return out;
}

void setDeniedTermsText(const std::string& text) {
    Store& s = store();
    s.set(text);
}

const std::vector<std::string>& deniedTerms() {
    return store().terms;
}

bool containsDeniedTerm(std::string_view text, std::string* term) {
    const Store& s = store();
    if (text.empty() || s.compiled.empty()) return false;
    const std::string folded = foldWords(text);
    const std::string lowered = lowerAscii(text);
    for (const Term& t : s.compiled) {
        const bool hit = t.substring ? lowered.find(t.folded) != std::string::npos : folded.find(t.folded) != std::string::npos;
        if (hit) {
            if (term) *term = t.text;
            return true;
        }
    }
    return false;
}

std::vector<DeniedTermHit> findDeniedTerms(const Json& value, const std::string& path) {
    std::vector<DeniedTermHit> hits;
    walk(value, path, hits);
    return hits;
}

}
