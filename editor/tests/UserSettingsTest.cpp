// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// UserSettings / PlayerPrefs on the desktop and editor store (feature script-data, spec 6.2 and
// 9.9-9.13): hasKey / deleteKey / deleteAll, present empty values, locale-independent numbers,
// getters that never throw, the write-behind and its atomic file, the file format (keys that
// are no XML names, values XML text cannot hold, files of older builds), and PlayerPrefs.

#include "ScriptDataTestSupport.h"

#include "io/UserSettings.h"
#include "io/UserSettingsStore.h"

#include <gtest/gtest.h>

#include <clocale>
#include <cstdlib>
#include <limits>
#include <string>

#include "support/PosixCompat.h"

using namespace jasperengine;
using scriptdata::ScriptDataTest;
namespace fs = std::filesystem;

namespace {

class UserSettingsTest : public ScriptDataTest {
protected:
    fs::path settingsFile() const { return root / "data" / "UserSettings.xml"; }

    // what a new run of the game reads: the store forgets what it has in memory
    void reload() { UserSettingsStore::instance().unload(); }
};

// ---------------------------------------------------------------- 9. keys

TEST_F(UserSettingsTest, HasKeyDeleteKeyDeleteAllAndDefaults) {
    EXPECT_FALSE(UserSettings::hasKey("coins"));
    EXPECT_EQ(UserSettings::getIntegerForKey("coins"), 0);
    EXPECT_EQ(UserSettings::getIntegerForKey("coins", 7), 7);
    EXPECT_EQ(UserSettings::getStringForKey("name", "anon"), "anon");
    EXPECT_FALSE(UserSettings::getBoolForKey("flag", false));
    EXPECT_TRUE(UserSettings::getBoolForKey("flag", true));
    EXPECT_DOUBLE_EQ(UserSettings::getDoubleForKey("d", 2.5), 2.5);

    UserSettings::setIntegerForKey("coins", 120);
    UserSettings::setStringForKey("name", "");
    EXPECT_TRUE(UserSettings::hasKey("coins"));
    EXPECT_TRUE(UserSettings::hasKey("name")) << "an empty value is present";
    EXPECT_EQ(UserSettings::getStringForKey("name", "anon"), "") << "a present empty string is returned, not the default";
    EXPECT_EQ(UserSettings::getIntegerForKey("coins", 7), 120);

    UserSettings::deleteKey("coins");
    EXPECT_FALSE(UserSettings::hasKey("coins"));
    UserSettings::deleteKey("never-there");  // no-op
    UserSettings::removeKey("name");
    EXPECT_FALSE(UserSettings::hasKey("name"));

    UserSettings::setBoolForKey("a", true);
    UserSettings::setFloatForKey("b", 0.25f);
    UserSettings::setLongForKey("c", 1234567890L);
    UserSettings::save();
    reload();
    EXPECT_TRUE(UserSettings::getBoolForKey("a"));
    EXPECT_FLOAT_EQ(UserSettings::getFloatForKey("b"), 0.25f);
    EXPECT_EQ(UserSettings::getLongForKey("c"), 1234567890L);
    UserSettings::deleteAll();
    EXPECT_FALSE(UserSettings::hasKey("a"));
    EXPECT_FALSE(UserSettings::hasKey("b"));
    UserSettings::save();
    reload();
    EXPECT_FALSE(UserSettings::hasKey("c")) << "deleteAll reaches the file";
    EXPECT_TRUE(UserSettingsStore::instance().entries().empty());
}

TEST_F(UserSettingsTest, ValuesRoundTripExactlyThroughTheFile) {
    const std::string bytes("a\0b", 3);
    const std::vector<std::pair<std::string, std::string>> values = {
        {"plain", "hello world"},
        {"spaces", "  padded  "},
        {"only spaces", "   "},
        {"newlines", "line 1\nline 2\n"},
        {"crlf", "a\r\nb"},
        {"markup", "<b>&amp;\"quoted\"</b>"},
        {"utf8", "\xC3\xA9\xF0\x9F\x98\x80"},
        {"json", "{\"list\": [1, 2], \"name\": \"x\"}"},
        {"zero", bytes},
        {"control", "\x01\x02"},
        {"1starts with a digit", "k"},
        {"with/slash and:colon", "k"},
        {"entry", "a key named like the fallback element"},
    };
    for (const auto& [key, value] : values) UserSettings::setStringForKey(key.c_str(), value);
    UserSettings::save();

    // a file any XML reader takes: keys that are no element names are attributes
    const std::string xml = scriptdata::readFile(settingsFile());
    EXPECT_NE(xml.find("<?xml version=\"1.0\" encoding=\"UTF-8\"?>"), std::string::npos) << xml;
    EXPECT_NE(xml.find("<plain>hello world</plain>"), std::string::npos) << xml;
    EXPECT_NE(xml.find("<entry key=\"with/slash and:colon\">k</entry>"), std::string::npos) << xml;
    EXPECT_NE(xml.find("<entry key=\"only spaces\" encoding=\"base64\">"), std::string::npos) << xml;

    reload();
    for (const auto& [key, value] : values) {
        EXPECT_TRUE(UserSettings::hasKey(key.c_str())) << key;
        EXPECT_EQ(UserSettings::getStringForKey(key.c_str(), "MISSING"), value) << key;
    }
    EXPECT_EQ(UserSettingsStore::instance().entries().size(), values.size());
}

TEST_F(UserSettingsTest, ReadsFilesOfOlderBuilds) {
    // the format before this feature: one element per key, numbers from std::to_string
    scriptdata::writeFile(settingsFile(),
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<userSettings>\n"
        "    <Minigame_Money>120</Minigame_Money>\n"
        "    <volume>0.500000</volume>\n"
        "    <commaVolume>0,250000</commaVolume>\n"
        "    <music>true</music>\n"
        "    <playerName>Ana</playerName>\n"
        "    <blob>AAEC</blob>\n"
        "</userSettings>\n");
    EXPECT_EQ(UserSettings::getIntegerForKey("Minigame_Money"), 120);
    EXPECT_FLOAT_EQ(UserSettings::getFloatForKey("volume"), 0.5f);
    EXPECT_FLOAT_EQ(UserSettings::getFloatForKey("commaVolume"), 0.25f) << "an older build's locale decimal comma";
    EXPECT_TRUE(UserSettings::getBoolForKey("music"));
    EXPECT_EQ(UserSettings::getStringForKey("playerName"), "Ana");
    Data blob = UserSettings::getDataForKey("blob");
    ASSERT_EQ(blob.length(), 3u);
    // a corrupt file is moved aside (never overwritten) and the store starts empty
    reload();
    scriptdata::writeFile(settingsFile(), "<userSettings><broken></userSettings");
    EXPECT_FALSE(UserSettings::hasKey("Minigame_Money"));
    EXPECT_TRUE(fs::exists(root / "data" / "UserSettings.xml.corrupt"));
    UserSettings::setIntegerForKey("fresh", 1);
    UserSettings::save();
    reload();
    EXPECT_EQ(UserSettings::getIntegerForKey("fresh"), 1);
}

TEST_F(UserSettingsTest, KeysAreNeverEmptyOrCutAtAZeroByte) {
    // the same keys are refused on every platform (UserSettings checks them itself)
    UserSettings::setIntegerForKey("", 1);
    UserSettings::setStringForKey("", "x");
    UserSettings::setBoolForKey(nullptr, true);
    EXPECT_FALSE(UserSettings::hasKey(""));
    EXPECT_TRUE(UserSettingsStore::instance().entries().empty());
    // "a\0b" would be stored as "a" by the const char* API
    EXPECT_EQ(eval("PlayerPrefs.setString('a\\0b', 'v')"), "ERROR: PlayerPrefs.setString: key must not contain a zero byte");
    EXPECT_EQ(eval("PlayerPrefs.hasKey('a\\0b')"), "ERROR: PlayerPrefs.hasKey: key must not contain a zero byte");
    EXPECT_EQ(eval("PlayerPrefs.deleteKey('a\\0b')"), "ERROR: PlayerPrefs.deleteKey: key must not contain a zero byte");
    EXPECT_EQ(eval("UserSettings.setStringForKey('a\\0b', 'v') return tostring(UserSettings.hasKey('a'))"), "false");
    EXPECT_EQ(eval("return tostring(UserSettings.hasKey('a\\0b')) .. UserSettings.getIntegerForKey('a\\0b', 7)"), "false7");
    EXPECT_TRUE(UserSettingsStore::instance().entries().empty());
    // values keep their zero bytes (the web store passes lengths too)
    EXPECT_EQ(eval("PlayerPrefs.setString('k', 'x\\0y') UserSettings.save() return #PlayerPrefs.getString('k')"), "3");
    reload();
    EXPECT_EQ(eval("return #UserSettings.getStringForKey('k')"), "3");
}

// ---------------------------------------------------------------- 10. locale

TEST_F(UserSettingsTest, NumbersAreWrittenTheSameInEveryLocale) {
    // a German locale prints 0,5; the file must still say 0.5
    const char* previousLocpath = std::getenv("LOCPATH");
    const std::string savedLocpath = previousLocpath ? previousLocpath : "";
    const char* names[] = {"de_DE.UTF-8", "de_DE.utf8", "de_DE", "fr_FR.UTF-8", "pt_BR.UTF-8"};
    const char* applied = nullptr;
    for (const char* name : names) {
        if ((applied = std::setlocale(LC_NUMERIC, name))) break;
    }
    fs::path generated;
    if (!applied && fs::exists("/usr/bin/localedef") && fs::exists("/usr/share/i18n/locales/de_DE")) {
        // none installed: one made for this test (glibc reads LOCPATH)
        generated = root / "locales";
        fs::create_directories(generated);
        const std::string command = "/usr/bin/localedef -i de_DE -f UTF-8 '" + (generated / "de_DE.UTF-8").string() + "' >/dev/null 2>&1";
        if (std::system(command.c_str()) == 0) {
            setenv("LOCPATH", generated.c_str(), 1);
            applied = std::setlocale(LC_NUMERIC, "de_DE.UTF-8");
        }
    }
    if (!applied && fs::exists("/usr/bin/localedef")) {
        // no locale sources either (the build image): a locale of nothing but a decimal comma,
        // over an ASCII character map, compiled here (no skipped editor test)
        const fs::path sources = root / "locale-sources";
        if (generated.empty()) generated = root / "locales";
        fs::create_directories(sources);
        fs::create_directories(generated);
        std::ofstream(sources / "comma") << "LC_NUMERIC\ndecimal_point \"<U002C>\"\nthousands_sep \"\"\ngrouping -1\nEND LC_NUMERIC\n";
        {
            std::ofstream charmap(sources / "ascii");
            charmap << "<code_set_name> JASPER_TEST_ASCII\n<comment_char> %\n<escape_char> /\n<mb_cur_max> 1\nCHARMAP\n";
            char line[32];
            for (int c = 0; c < 128; c++) {
                std::snprintf(line, sizeof(line), "<U%04X> /x%02x\n", c, c);
                charmap << line;
            }
            charmap << "END CHARMAP\n";
        }
        const std::string command = "/usr/bin/localedef -c -i '" + (sources / "comma").string() + "' -f '" + (sources / "ascii").string() +
                                    "' '" + (generated / "de_TEST").string() + "' >/dev/null 2>&1";
        // -c: written despite the categories it leaves out (its status says so: setlocale decides)
        const int made = std::system(command.c_str());
        (void)made;
        setenv("LOCPATH", generated.c_str(), 1);
        applied = std::setlocale(LC_NUMERIC, "de_TEST");
    }
    ASSERT_TRUE(applied) << "no locale with a decimal comma (nor localedef to make one)";
    char printed[32];
    std::snprintf(printed, sizeof(printed), "%g", 0.5);
    ASSERT_STREQ(printed, "0,5") << "the locale under test uses a decimal comma";

    UserSettings::setFloatForKey("f", 0.1f);
    UserSettings::setDoubleForKey("d", 1234.5678);
    UserSettings::setFloatForKey("small", 1.5e-7f);
    UserSettings::save();
    EXPECT_FLOAT_EQ(UserSettings::getFloatForKey("f"), 0.1f);
    EXPECT_EQ(UserSettings::getFloatForKey("f"), 0.1f);
    EXPECT_EQ(UserSettings::getDoubleForKey("d"), 1234.5678);
    EXPECT_EQ(UserSettings::getFloatForKey("small"), 1.5e-7f);
    const std::string xml = scriptdata::readFile(settingsFile());

    std::setlocale(LC_NUMERIC, "C");
    if (previousLocpath) setenv("LOCPATH", savedLocpath.c_str(), 1);
    else unsetenv("LOCPATH");

    EXPECT_NE(xml.find("<f>0.1</f>"), std::string::npos) << xml;
    EXPECT_NE(xml.find("<d>1234.5678</d>"), std::string::npos) << xml;
    EXPECT_EQ(xml.find(','), std::string::npos) << xml;
    reload();
    EXPECT_EQ(UserSettings::getFloatForKey("f"), 0.1f);
    EXPECT_EQ(UserSettings::getDoubleForKey("d"), 1234.5678);
}

// ---------------------------------------------------------------- 11. bad values

TEST_F(UserSettingsTest, TypedGettersNeverThrow) {
    scriptdata::writeFile(settingsFile(),
        "<userSettings><k>abc</k><big>99999999999999999999</big><f>1e999</f><b>yes</b><empty></empty></userSettings>");
    EXPECT_NO_THROW({
        EXPECT_EQ(UserSettings::getIntegerForKey("k", 5), 5);
        EXPECT_EQ(UserSettings::getIntegerForKey("big", 6), 6);
        EXPECT_EQ(UserSettings::getLongForKey("k", 7L), 7L);
        EXPECT_FLOAT_EQ(UserSettings::getFloatForKey("k", 1.5f), 1.5f);
        EXPECT_FLOAT_EQ(UserSettings::getFloatForKey("f", 2.5f), 2.5f);
        EXPECT_DOUBLE_EQ(UserSettings::getDoubleForKey("k", 3.5), 3.5);
        EXPECT_TRUE(UserSettings::getBoolForKey("b", true));
        EXPECT_EQ(UserSettings::getIntegerForKey("empty", 8), 8);
        EXPECT_EQ(UserSettings::getStringForKey("k"), "abc");
    });
    // an int that is a float, an int out of the 32-bit range
    UserSettings::setStringForKey("x", "3.5");
    EXPECT_EQ(UserSettings::getIntegerForKey("x", -1), -1);
    UserSettings::setLongForKey("wide", 5000000000L);
    EXPECT_EQ(UserSettings::getIntegerForKey("wide", -1), -1);
    // non-finite floats round-trip
    UserSettings::setFloatForKey("inf", std::numeric_limits<float>::infinity());
    EXPECT_EQ(UserSettings::getFloatForKey("inf"), std::numeric_limits<float>::infinity());
}

// ---------------------------------------------------------------- 12. write-behind

TEST_F(UserSettingsTest, WritesASecondAfterAChangeOrOnSave) {
    double now = 100.0;
    UserSettingsStore::instance().setClock([&now]() { return now; });

    UserSettings::setIntegerForKey("score", 1);
    EXPECT_TRUE(UserSettingsStore::instance().hasUnsavedChanges());
    EXPECT_FALSE(fs::exists(settingsFile())) << "a setter does not write";
    now += 0.5;
    UserSettings::tick();
    EXPECT_FALSE(fs::exists(settingsFile())) << "not before a second";
    UserSettings::setIntegerForKey("score", 2);  // the oldest unsaved change counts
    now += 0.5;
    UserSettings::tick();
    ASSERT_TRUE(fs::exists(settingsFile())) << "written a second after the first change";
    EXPECT_NE(scriptdata::readFile(settingsFile()).find("<score>2</score>"), std::string::npos);
    EXPECT_FALSE(UserSettingsStore::instance().hasUnsavedChanges());
    EXPECT_TRUE(scriptdata::temporaryFiles(root / "data").empty()) << "the temporary file is renamed over it";

    // the same value again is no change
    UserSettings::setIntegerForKey("score", 2);
    EXPECT_FALSE(UserSettingsStore::instance().hasUnsavedChanges());

    // save() writes at once
    UserSettings::setIntegerForKey("score", 3);
    UserSettings::save();
    EXPECT_NE(scriptdata::readFile(settingsFile()).find("<score>3</score>"), std::string::npos);

    // a file that cannot be written: the values stay in memory, tried again a second later
    // (the data folder replaced by a file for a moment)
    fs::rename(root / "data", root / "data-away");
    scriptdata::writeFile(root / "data", "not a folder");
    UserSettings::setIntegerForKey("score", 4);
    UserSettings::save();
    EXPECT_TRUE(UserSettingsStore::instance().hasUnsavedChanges());
    EXPECT_EQ(UserSettings::getIntegerForKey("score"), 4);
    fs::remove(root / "data");
    fs::rename(root / "data-away", root / "data");
    EXPECT_NE(scriptdata::readFile(settingsFile()).find("<score>3</score>"), std::string::npos) << "the old file stays whole";
    now += 0.5;
    UserSettings::tick();
    EXPECT_TRUE(UserSettingsStore::instance().hasUnsavedChanges()) << "not again before a second";
    now += 0.6;
    UserSettings::tick();
    EXPECT_FALSE(UserSettingsStore::instance().hasUnsavedChanges());
    EXPECT_NE(scriptdata::readFile(settingsFile()).find("<score>4</score>"), std::string::npos);
}

TEST_F(UserSettingsTest, AFileThatCannotBeReadIsNeverReplaced) {
    fs::create_directories(settingsFile());   // something unreadable where the file goes
    UserSettings::setIntegerForKey("k", 1);
    EXPECT_EQ(UserSettings::getIntegerForKey("k"), 1) << "the value works in memory";
    UserSettings::save();
    EXPECT_TRUE(UserSettingsStore::instance().hasUnsavedChanges());
    EXPECT_TRUE(fs::is_directory(settingsFile())) << "not replaced";
    EXPECT_TRUE(scriptdata::temporaryFiles(root / "data").empty());
}

TEST_F(UserSettingsTest, ADestroyedSystemWritesWhatIsUnsaved) {
    scriptdata::TestSystem other;
    other.root = (root / "other").string();
    fs::create_directories(root / "other" / "data");
    System* mine = SystemTestAccess::swap(&other);
    UserSettings::setStringForKey("left", "behind");
    SystemTestAccess::swap(mine);
    // the store follows System::instance(): the other System's change went to its file first
    EXPECT_FALSE(UserSettings::hasKey("left"));
    EXPECT_NE(scriptdata::readFile(root / "other" / "data" / "UserSettings.xml").find("<left>behind</left>"), std::string::npos);

    {
        scriptdata::TestSystem shortLived;
        shortLived.root = (root / "short").string();
        fs::create_directories(root / "short" / "data");
        System* before = SystemTestAccess::swap(&shortLived);
        UserSettings::setStringForKey("unsaved", "1");
        SystemTestAccess::swap(before);
    }   // ~System
    EXPECT_NE(scriptdata::readFile(root / "short" / "data" / "UserSettings.xml").find("<unsaved>1</unsaved>"), std::string::npos);
}

// ---------------------------------------------------------------- 13. PlayerPrefs

TEST_F(UserSettingsTest, PlayerPrefsIsTheSameStore) {
    EXPECT_EQ(eval("PlayerPrefs.setInt('Minigame_Money', 120) return UserSettings.getIntegerForKey('Minigame_Money')"), "120");
    UserSettings::setStringForKey("name", "Ana");
    UserSettings::setFloatForKey("volume", 0.75f);
    EXPECT_EQ(eval("return PlayerPrefs.getString('name')"), "Ana");
    EXPECT_EQ(eval("return PlayerPrefs.getFloat('volume')"), "0.75");
    EXPECT_EQ(eval("return math.type(PlayerPrefs.getInt('Minigame_Money'))"), "integer");
    // defaults
    EXPECT_EQ(eval("return PlayerPrefs.getInt('missing')"), "0");
    EXPECT_EQ(eval("return PlayerPrefs.getInt('missing', 5)"), "5");
    EXPECT_EQ(eval("return PlayerPrefs.getFloat('missing')"), "0.0");
    EXPECT_EQ(eval("return PlayerPrefs.getString('missing')"), "");
    EXPECT_EQ(eval("return PlayerPrefs.getString('missing', 'x')"), "x");
    // hasKey / deleteKey / deleteAll / save
    EXPECT_EQ(eval("return tostring(PlayerPrefs.hasKey('OnboardingDone'))"), "false");
    EXPECT_EQ(eval("PlayerPrefs.setString('OnboardingDone', '') return tostring(PlayerPrefs.hasKey('OnboardingDone'))"), "true");
    EXPECT_EQ(eval("PlayerPrefs.deleteKey('OnboardingDone') return tostring(UserSettings.hasKey('OnboardingDone'))"), "false");
    EXPECT_EQ(eval("PlayerPrefs.save() return 'saved'"), "saved");
    EXPECT_NE(scriptdata::readFile(settingsFile()).find("<Minigame_Money>120</Minigame_Money>"), std::string::npos);
    EXPECT_EQ(eval("PlayerPrefs.deleteAll() return tostring(UserSettings.hasKey('name'))"), "false");
    // UserSettings got the Unity names too
    EXPECT_EQ(eval("UserSettings.setIntegerForKey('k', 1) UserSettings.deleteKey('k') return tostring(UserSettings.hasKey('k'))"), "false");
    EXPECT_EQ(eval("UserSettings.setIntegerForKey('k', 1) UserSettings.deleteAll() UserSettings.save() return tostring(UserSettings.hasKey('k'))"), "false");
    // integers: 6 / 2 is fine, 3.5 is not
    EXPECT_EQ(eval("PlayerPrefs.setInt('half', 6 / 2) return PlayerPrefs.getInt('half')"), "3");
    EXPECT_EQ(eval("PlayerPrefs.setInt('n', 3.5)"), "ERROR: PlayerPrefs.setInt: integer expected, got 3.5");
    EXPECT_EQ(eval("PlayerPrefs.setInt('n', '3')"), "ERROR: PlayerPrefs.setInt: integer expected, got string");
    EXPECT_NE(eval("PlayerPrefs.setInt('n', 2^40 // 1)").find("outside the 32-bit int range"), std::string::npos);
    EXPECT_EQ(eval("PlayerPrefs.setFloat('n', 'x')"), "ERROR: PlayerPrefs.setFloat: number expected, got string");
    EXPECT_EQ(eval("PlayerPrefs.setString('n', 5)"), "ERROR: PlayerPrefs.setString: string expected, got number");
    EXPECT_EQ(eval("PlayerPrefs.getInt(5)"), "ERROR: PlayerPrefs.getInt: key must be a non-empty string, got number");
    EXPECT_EQ(eval("PlayerPrefs.getInt('')"), "ERROR: PlayerPrefs.getInt: key must be a non-empty string, got \"\"");
    EXPECT_EQ(eval("PlayerPrefs.getString('k', 1)"), "ERROR: PlayerPrefs.getString: the default must be a string, got number");
    // a value of another type gives the default (read with the type it was written with)
    EXPECT_EQ(eval("PlayerPrefs.setString('word', 'abc') return PlayerPrefs.getInt('word', 9)"), "9");
    // a JSON table in a string key
    EXPECT_EQ(eval("PlayerPrefs.setString('state', Json.encode({level = 3})) return Json.decode(PlayerPrefs.getString('state')).level"), "3");
}

}
