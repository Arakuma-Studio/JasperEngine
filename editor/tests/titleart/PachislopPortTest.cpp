// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S84 AC11, AC13, AC18 (R20-R22, R38): the Pachislop port's stage 2 (editor/titleart/library/
// PachislopPort) on the committed dump reproduces the committed library files and
// provenance/pachislop.json byte for byte, after `normalize`. Red controls: one changed byte of a
// committed look is named; a dump member with no mapping and no `dropped` entry fails the run
// naming it (fixtures/pachislop-port-unmapped.json, whose twin without it maps cleanly); a denied
// term in a mapped name fails the run. ReviewRecords holds review.json to the port's items;
// DeniedTerms checks the matcher with terms read from tools/titleart/denied_terms.txt.

#include <gtest/gtest.h>

#include "titleart/library/DeniedTerms.h"
#include "titleart/library/PachislopPort.h"
#include "titleart/library/TitleLibraryFormats.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::editor::titlelibrary;
namespace fs = std::filesystem;

namespace {

    const fs::path REPO = fs::path(JASPER_TEST_REPO_DIR);

    std::string readText(const fs::path& path) {
        std::ifstream in(path, std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    Json readJson(const fs::path& path) {
        return Json::parse(readText(path), nullptr, false);
    }

    void writeText(const fs::path& path, const std::string& text) {
        fs::create_directories(path.parent_path());
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << text;
    }

    std::string joined(const std::vector<std::string>& lines, size_t max = 40) {
        std::string out;
        for (size_t i = 0; i < lines.size() && i < max; i++) out += lines[i] + "\n";
        if (lines.size() > max) out += "... (" + std::to_string(lines.size() - max) + " more)\n";
        return out;
    }

    bool anyContains(const std::vector<std::string>& lines, const std::string& needle) {
        for (const std::string& l : lines) {
            if (l.find(needle) != std::string::npos) return true;
        }
        return false;
    }

    class PachislopPortTest : public ::testing::Test {
    protected:
        static void SetUpTestSuite() {
            setDeniedTermsText(readText(REPO / "tools/titleart/denied_terms.txt"));
            options = std::make_unique<PortOptions>();
            std::string error;
            loadOk = loadPortOptions(REPO, *options, &error);
            loadError = error;
            const Json dump = readJson(REPO / PACHISLOP_DUMP_PATH);
            result = std::make_unique<PortResult>(portDump(dump, *options));
            std::vector<std::string> errors;
            normalized = std::make_unique<std::map<std::string, std::string>>(normalizedPortFiles(*result, &errors));
            normalizeErrors = errors;
        }
        static void TearDownTestSuite() {
            options.reset();
            result.reset();
            normalized.reset();
        }

        static inline std::unique_ptr<PortOptions> options;
        static inline std::unique_ptr<PortResult> result;
        static inline std::unique_ptr<std::map<std::string, std::string>> normalized;
        static inline std::vector<std::string> normalizeErrors;
        static inline bool loadOk = false;
        static inline std::string loadError;
    };

    fs::path fixturePath() {
        return REPO / "editor/tests/titleart/fixtures/pachislop-port-unmapped.json";
    }

    // the JSON path of the first text layer of a document's tree ("layers[2].children[0]")
    bool firstTextLayer(const Json& layers, const std::string& path, std::vector<std::string>& keys, std::string& text) {
        for (size_t i = 0; i < layers.size(); i++) {
            const std::string at = path + "[" + std::to_string(i) + "]";
            keys.push_back("#" + std::to_string(i));
            if (layers[i].value("type", std::string()) == "text") {
                text = at;
                return true;
            }
            if (layers[i].contains("children")) {
                keys.push_back("children");
                if (firstTextLayer(layers[i]["children"], at + ".children", keys, text)) return true;
                keys.pop_back();
            }
            keys.pop_back();
        }
        return false;
    }

    fs::path scratchDir(const char* name) {
        const fs::path dir = fs::temp_directory_path() / (std::string("jasper-pachislop-port-test-") + name);
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir);
        return dir;
    }

}

TEST_F(PachislopPortTest, CheckReproducesCommittedFiles) {
    ASSERT_TRUE(loadOk) << loadError;
    ASSERT_TRUE(result->ok) << joined(result->errors);
    EXPECT_EQ(result->looks, 104);
    EXPECT_EQ(result->templates, 37);
    EXPECT_TRUE(normalizeErrors.empty()) << joined(normalizeErrors);
    ASSERT_TRUE(result->files.count(PACHISLOP_PROVENANCE_PATH));
    const std::vector<PortFinding> findings = comparePortFiles(*normalized, REPO);
    std::vector<std::string> lines;
    for (const PortFinding& f : findings) lines.push_back(f.what + ": " + f.path + " (" + f.detail + ")");
    EXPECT_TRUE(findings.empty()) << joined(lines);
    // the port's output is canonical already: normalize changes nothing
    for (const auto& [path, text] : result->files) EXPECT_EQ(text, normalized->at(path)) << path;
}

TEST_F(PachislopPortTest, ChangedByteInCommittedLookIsNamed) {
    ASSERT_TRUE(result->ok) << joined(result->errors);
    const fs::path root = scratchDir("tree");
    for (const auto& [path, text] : *normalized) writeText(root / path, text);
    EXPECT_TRUE(comparePortFiles(*normalized, root).empty());

    const std::string look = std::string(TITLE_LIBRARY_RESOURCES_PATH) + "/looks/metal/classic-gold.titlelook";
    ASSERT_TRUE(normalized->count(look));
    std::string text = normalized->at(look);
    const size_t at = text.find("\"revision\": 1");
    ASSERT_NE(at, std::string::npos);
    text[at + 12] = '2';
    writeText(root / look, text);
    // a look with Pachislop provenance the port does not produce is stale
    const std::string extra = std::string(TITLE_LIBRARY_RESOURCES_PATH) + "/looks/metal/zz-extra.titlelook";
    writeText(root / extra, normalized->at(look));

    const std::vector<PortFinding> findings = comparePortFiles(*normalized, root);
    ASSERT_EQ(findings.size(), 2u);
    EXPECT_EQ(findings[0].path, look);
    EXPECT_EQ(findings[0].what, "differs");
    EXPECT_NE(findings[0].detail.find("revision"), std::string::npos) << findings[0].detail;
    EXPECT_EQ(findings[1].path, extra);
    EXPECT_EQ(findings[1].what, "stale");
    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_F(PachislopPortTest, UnmappedMemberFailsNamingIt) {
    ASSERT_TRUE(loadOk) << loadError;
    Json dump = readJson(fixturePath());
    ASSERT_TRUE(dump.is_object());
    const PortResult bad = portDump(dump, *options);
    EXPECT_FALSE(bad.ok);
    ASSERT_EQ(bad.errors.size(), 1u) << joined(bad.errors);
    EXPECT_NE(bad.errors[0].find("look.style.face.sparkleDust"), std::string::npos) << bad.errors[0];
    EXPECT_EQ(bad.files.count(std::string(TITLE_LIBRARY_RESOURCES_PATH) + "/looks/casino/outline-white.titlelook"), 0u);

    // the twin: the same look without it maps, colours lowercase and the rim offset turned y-up
    dump["looks"][0]["preset"]["style"]["face"].erase("sparkleDust");
    const PortResult good = portDump(dump, *options);
    ASSERT_TRUE(good.ok) << joined(good.errors);
    const std::string path = std::string(TITLE_LIBRARY_RESOURCES_PATH) + "/looks/casino/outline-white.titlelook";
    ASSERT_TRUE(good.files.count(path));
    const LookParseResult look = parseLook(good.files.at(path));
    ASSERT_TRUE(look.ok) << look.error;
    EXPECT_EQ(look.look.root["style"]["rims"][0]["offset"], Json::array({0, -3.2}));
    EXPECT_EQ(look.look.root["effects"][0]["params"]["color"], "#1a2040");
    EXPECT_GE(look.look.tags().size(), 3u);
    EXPECT_EQ(look.look.root["provenance"]["item"], "f-outline-white");
}

TEST_F(PachislopPortTest, DeniedTermInMappedNameFails) {
    ASSERT_TRUE(loadOk) << loadError;
    Json dump = readJson(fixturePath());
    dump["looks"][0]["preset"]["style"]["face"].erase("sparkleDust");
    PortOptions tainted = *options;
    tainted.map["names"]["looks"]["casino/outline-white"]["en"] = "Hokuto Outline White";  // denied-term fixture
    const PortResult r = portDump(dump, tainted);
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(anyContains(r.errors, "name.en")) << joined(r.errors);
    EXPECT_TRUE(anyContains(r.errors, "hokuto")) << joined(r.errors);  // denied-term fixture
}

TEST_F(PachislopPortTest, ReviewRecords) {
    ASSERT_TRUE(result->ok) << joined(result->errors);
    const Json review = readJson(REPO / PACHISLOP_REVIEW_PATH);
    const Json overrides = readJson(REPO / PACHISLOP_OVERRIDES_PATH);
    ASSERT_TRUE(review.is_object() && review["records"].is_array());
    std::map<std::pair<std::string, std::string>, Json> records;
    for (const Json& r : review["records"]) {
        const auto key = std::make_pair(r.value("kind", std::string()), r.value("id", std::string()));
        EXPECT_EQ(records.count(key), 0u) << "two records for " << key.first << " " << key.second;
        records[key] = r;
    }
    auto section = [](const std::string& kind) { return kind == "look" ? "looks" : kind == "template" ? "templates" : "color_schemes"; };
    size_t ported = 0;
    for (const Json& item : result->provenance["items"]) {
        const std::string kind = item["kind"], ref = item["ref"];
        if (kind == "color_scheme" && !result->files.count(std::string(TITLE_LIBRARY_RESOURCES_PATH) + "/color-schemes/" + ref + ".titlecolors")) continue;
        ported++;
        EXPECT_TRUE(records.count({kind, ref})) << "no review record for " << kind << " " << ref;
    }
    EXPECT_GE(ported, 141u);
    for (const auto& [key, r] : records) {
        const std::string verdict = r.value("verdict", std::string());
        EXPECT_TRUE(verdict == "match" || verdict == "close" || verdict == "tuned" || verdict == "differs") << key.second << ": " << verdict;
        if (verdict != "match") {
            EXPECT_FALSE(r.value("note", std::string()).empty()) << key.second << " needs a note";
        }
        const bool overridden = overrides.contains(section(key.first)) && overrides[section(key.first)].contains(key.second);
        // the gate's own metadata (exceptions, the intended background) is not a visual tuning
        bool visual = false;
        if (overridden) {
            for (const auto& [member, value] : overrides[section(key.first)][key.second].items()) {
                if (member == "template") {
                    for (const auto& [inner, unused] : value.items()) visual = visual || (inner != "check_allow" && inner != "lint_allow" && inner != "background");
                } else {
                    visual = visual || (member != "check_allow" && member != "lint_allow" && member != "background");
                }
            }
        }
        if (verdict == "tuned") {
            EXPECT_TRUE(overridden) << key.first << " " << key.second << ": `tuned` needs an overrides.json entry";
        }
        if (visual) {
            EXPECT_EQ(verdict, "tuned") << key.first << " " << key.second << ": a visual overrides.json entry is a `tuned` record";
        }
    }
}

TEST_F(PachislopPortTest, DeniedTerms) {
    const std::vector<std::string> fromFile = parseDeniedTerms(readText(REPO / "tools/titleart/denied_terms.txt"));
    ASSERT_FALSE(fromFile.empty());
    EXPECT_EQ(deniedTerms(), fromFile);
    // the first Latin one-word term and the first term in Japanese script of the file
    std::string latin, cjk, phrase;
    for (const std::string& t : fromFile) {
        if (cjk.empty() && hasJapaneseScript(t)) cjk = t;
        if (latin.empty() && !hasJapaneseScript(t) && t.find(' ') == std::string::npos) latin = t;
        if (phrase.empty() && !hasJapaneseScript(t) && t.find(' ') != std::string::npos) phrase = t;
    }
    ASSERT_FALSE(latin.empty());
    ASSERT_FALSE(cjk.empty());
    ASSERT_FALSE(phrase.empty());
    std::string upper = latin;
    for (char& c : upper) c = (char)std::toupper((unsigned char)c);
    std::string term;
    EXPECT_TRUE(containsDeniedTerm("The " + upper + " plate", &term));
    EXPECT_EQ(term, latin);
    EXPECT_TRUE(containsDeniedTerm("casino/" + latin + "-plate", nullptr));
    EXPECT_FALSE(containsDeniedTerm("s" + latin + "x plate", nullptr)) << "a whole word only, not inside a longer one";
    EXPECT_FALSE(containsDeniedTerm(latin + "ish", nullptr));
    EXPECT_TRUE(containsDeniedTerm("大" + cjk + "星", &term)) << "a CJK term matches as a substring";
    EXPECT_EQ(term, cjk);
    std::string dashed = phrase;
    std::replace(dashed.begin(), dashed.end(), ' ', '-');
    std::string glued = phrase;
    glued.erase(std::remove(glued.begin(), glued.end(), ' '), glued.end());
    EXPECT_TRUE(containsDeniedTerm("A " + dashed + " look", nullptr));
    EXPECT_FALSE(containsDeniedTerm("A " + glued + "s look", nullptr));
    EXPECT_FALSE(containsDeniedTerm("Classic Gold", nullptr));

    // every place the port and `verify` check: name, synonym, tag, template sample, text layer
    ASSERT_TRUE(result->ok) << joined(result->errors);
    const Json look = Json::parse(result->files.at(std::string(TITLE_LIBRARY_RESOURCES_PATH) + "/looks/metal/classic-gold.titlelook"));
    const Json tmpl = Json::parse(result->files.at(std::string(TITLE_LIBRARY_RESOURCES_PATH) + "/templates/casino/confirm-rainbow.titletemplate"));
    EXPECT_TRUE(findDeniedTerms(look).empty());
    EXPECT_TRUE(findDeniedTerms(tmpl).empty());
    struct Injection {
        const Json* base;
        std::vector<std::string> path;      // object keys and array indexes ("#0")
        bool append;                        // into a list
        std::string expect;
    };
    std::vector<std::string> textKeys = {"layers"};
    std::string textPath;
    ASSERT_TRUE(firstTextLayer(tmpl["layers"], "layers", textKeys, textPath));
    textKeys.push_back("text");
    const std::vector<Injection> injections = {
        {&look, {"name", "en"}, false, "name.en"},
        {&look, {"synonyms", "ja"}, true, "synonyms.ja[0]"},
        {&look, {"tags"}, true, "tags[0]"},
        {&tmpl, {"template", "placeholders", "#0", "sample", "en"}, false, "template.placeholders[0].sample.en"},
        {&tmpl, textKeys, false, textPath + ".text"},
    };
    for (const Injection& inj : injections) {
        Json j = *inj.base;
        Json* at = &j;
        for (const std::string& k : inj.path) at = k[0] == '#' ? &(*at)[(size_t)std::stoul(k.substr(1))] : &(*at)[k];
        const std::string value = (inj.path.back() == "ja" ? cjk : "Super " + latin + " Title");
        if (inj.append) at->insert(at->begin(), value);
        else *at = value;
        const std::vector<DeniedTermHit> hits = findDeniedTerms(j);
        ASSERT_EQ(hits.size(), 1u) << inj.expect;
        EXPECT_EQ(hits[0].path, inj.expect);
    }
}
