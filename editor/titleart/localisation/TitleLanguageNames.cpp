// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleLanguageNames.h"

#include "util/TitleArtLanguage.h"

#include <cctype>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    std::string lower(std::string s) {
        for (char& c : s) c = (char)std::tolower((unsigned char)c);
        return s;
    }

}

const std::vector<TitleLanguageName>& editor::titleLanguageNames() {
    static const std::vector<TitleLanguageName> table = {
        {"en", "English", "English"},
        {"en-US", "English (United States)", "English (US)"},
        {"en-GB", "English (United Kingdom)", "English (UK)"},
        {"zh-Hans", "Chinese (Simplified)", "\xe7\xae\x80\xe4\xbd\x93\xe4\xb8\xad\xe6\x96\x87"},
        {"zh-Hant", "Chinese (Traditional)", "\xe7\xb9\x81\xe9\xab\x94\xe4\xb8\xad\xe6\x96\x87"},
        {"ja", "Japanese", "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e"},
        {"ko", "Korean", "\xed\x95\x9c\xea\xb5\xad\xec\x96\xb4"},
        {"de", "German", "Deutsch"},
        {"fr", "French", "Fran\xc3\xa7" "ais"},
        {"fr-CA", "French (Canada)", "Fran\xc3\xa7" "ais (Canada)"},
        {"es", "Spanish", "Espa\xc3\xb1ol"},
        {"es-419", "Spanish (Latin America)", "Espa\xc3\xb1ol (Latinoam\xc3\xa9rica)"},
        {"pt-BR", "Portuguese (Brazil)", "Portugu\xc3\xaas (Brasil)"},
        {"pt-PT", "Portuguese (Portugal)", "Portugu\xc3\xaas (Portugal)"},
        {"it", "Italian", "Italiano"},
        {"ru", "Russian", "\xd0\xa0\xd1\x83\xd1\x81\xd1\x81\xd0\xba\xd0\xb8\xd0\xb9"},
        {"pl", "Polish", "Polski"},
        {"tr", "Turkish", "T\xc3\xbcrk\xc3\xa7" "e"},
        {"nl", "Dutch", "Nederlands"},
        {"sv", "Swedish", "Svenska"},
        {"da", "Danish", "Dansk"},
        {"nb", "Norwegian Bokm\xc3\xa5l", "Norsk bokm\xc3\xa5l"},
        {"fi", "Finnish", "Suomi"},
        {"cs", "Czech", "\xc4\x8c" "e\xc5\xa1tina"},
        {"sk", "Slovak", "Sloven\xc4\x8dina"},
        {"hu", "Hungarian", "Magyar"},
        {"ro", "Romanian", "Rom\xc3\xa2n\xc4\x83"},
        {"bg", "Bulgarian", "\xd0\x91\xd1\x8a\xd0\xbb\xd0\xb3\xd0\xb0\xd1\x80\xd1\x81\xd0\xba\xd0\xb8"},
        {"el", "Greek", "\xce\x95\xce\xbb\xce\xbb\xce\xb7\xce\xbd\xce\xb9\xce\xba\xce\xac"},
        {"uk", "Ukrainian", "\xd0\xa3\xd0\xba\xd1\x80\xd0\xb0\xd1\x97\xd0\xbd\xd1\x81\xd1\x8c\xd0\xba\xd0\xb0"},
        {"hr", "Croatian", "Hrvatski"},
        {"sr-Latn", "Serbian (Latin)", "Srpski"},
        {"sr-Cyrl", "Serbian (Cyrillic)", "\xd0\xa1\xd1\x80\xd0\xbf\xd1\x81\xd0\xba\xd0\xb8"},
        {"sl", "Slovenian", "Sloven\xc5\xa1\xc4\x8dina"},
        {"lt", "Lithuanian", "Lietuvi\xc5\xb3"},
        {"lv", "Latvian", "Latvie\xc5\xa1u"},
        {"et", "Estonian", "Eesti"},
        {"ar", "Arabic", "\xd8\xa7\xd9\x84\xd8\xb9\xd8\xb1\xd8\xa8\xd9\x8a\xd8\xa9"},
        {"he", "Hebrew", "\xd7\xa2\xd7\x91\xd7\xa8\xd7\x99\xd7\xaa"},
        {"fa", "Persian", "\xd9\x81\xd8\xa7\xd8\xb1\xd8\xb3\xdb\x8c"},
        {"hi", "Hindi", "\xe0\xa4\xb9\xe0\xa4\xbf\xe0\xa4\xa8\xe0\xa5\x8d\xe0\xa4\xa6\xe0\xa5\x80"},
        {"bn", "Bengali", "\xe0\xa6\xac\xe0\xa6\xbe\xe0\xa6\x82\xe0\xa6\xb2\xe0\xa6\xbe"},
        {"ta", "Tamil", "\xe0\xae\xa4\xe0\xae\xae\xe0\xae\xbf\xe0\xae\xb4\xe0\xaf\x8d"},
        {"te", "Telugu", "\xe0\xb0\xa4\xe0\xb1\x86\xe0\xb0\xb2\xe0\xb1\x81\xe0\xb0\x97\xe0\xb1\x81"},
        {"th", "Thai", "\xe0\xb9\x84\xe0\xb8\x97\xe0\xb8\xa2"},
        {"vi", "Vietnamese", "Ti\xe1\xba\xbfng Vi\xe1\xbb\x87t"},
        {"id", "Indonesian", "Bahasa Indonesia"},
        {"ms", "Malay", "Bahasa Melayu"},
        {"fil", "Filipino", "Filipino"},
        {"sw", "Swahili", "Kiswahili"},
        {"ca", "Catalan", "Catal\xc3\xa0"},
        {"eu", "Basque", "Euskara"},
        {"gl", "Galician", "Galego"},
        {"is", "Icelandic", "\xc3\x8dslenska"},
        {"ga", "Irish", "Gaeilge"},
        {"cy", "Welsh", "Cymraeg"},
        {"af", "Afrikaans", "Afrikaans"},
        {"kk", "Kazakh", "\xd2\x9a\xd0\xb0\xd0\xb7\xd0\xb0\xd2\x9b\xd1\x88\xd0\xb0"},
        {"mn", "Mongolian", "\xd0\x9c\xd0\xbe\xd0\xbd\xd0\xb3\xd0\xbe\xd0\xbb"},
        {"ur", "Urdu", "\xd8\xa7\xd8\xb1\xd8\xaf\xd9\x88"},
    };
    return table;
}

std::string editor::titleLanguageEnglishName(const std::string& code) {
    const std::string wanted = lower(TitleArtLanguage::canonicalize(code));
    if (wanted.empty()) return "";
    for (const TitleLanguageName& n : titleLanguageNames()) {
        if (lower(n.code) == wanted) return n.english;
    }
    const size_t dash = wanted.find('-');
    if (dash == std::string::npos) return "";
    const std::string base = wanted.substr(0, dash);
    for (const TitleLanguageName& n : titleLanguageNames()) {
        if (lower(n.code) == base) {
            std::string rest = TitleArtLanguage::canonicalize(code).substr(dash + 1);
            return std::string(n.english) + " (" + rest + ")";
        }
    }
    return "";
}

std::string editor::titleLanguageLabel(const std::string& code, const std::string& name) {
    const std::string shown = name.empty() ? titleLanguageEnglishName(code) : name;
    return shown.empty() ? code : shown + " (" + code + ")";
}
