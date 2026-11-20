// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "input/TextEntry.h"

#include <cctype>

using namespace jasperengine;

namespace {

    const char* const keyboardNames[] = {"text", "numeric", "decimal", "tel", "email", "url", "search", "none"};
    const char* const enterKeyNames[] = {"", "enter", "done", "go", "next", "previous", "search", "send"};
    const char* const capitalizeNames[] = {"off", "sentences", "words", "characters"};

    bool equalsLower(std::string_view name, const char* lower){
        size_t i = 0;
        for (; i < name.size(); i++){
            if (lower[i] == '\0' || (char)std::tolower((unsigned char)name[i]) != lower[i])
                return false;
        }
        return lower[i] == '\0';
    }

    template<typename E, size_t N>
    bool fromNames(std::string_view name, const char* const (&names)[N], E& out){
        for (size_t i = 0; i < N; i++){
            if (equalsLower(name, names[i])){
                out = (E)i;
                return true;
            }
        }
        return false;
    }

}

const char* jasperengine::keyboardTypeName(KeyboardType type){
    return keyboardNames[(size_t)type];
}

bool jasperengine::keyboardTypeFromName(std::string_view name, KeyboardType& type){
    // "number" is the HTML input type's name for the same keyboard
    if (equalsLower(name, "number")){
        type = KeyboardType::Number;
        return true;
    }
    return !name.empty() && fromNames(name, keyboardNames, type);
}

const char* jasperengine::enterKeyName(EnterKey key){
    return enterKeyNames[(size_t)key];
}

bool jasperengine::enterKeyFromName(std::string_view name, EnterKey& key){
    if (equalsLower(name, "default")){
        key = EnterKey::Default;
        return true;
    }
    return !name.empty() && fromNames(name, enterKeyNames, key);
}

const char* jasperengine::autoCapitalizeName(AutoCapitalize capitalize){
    return capitalizeNames[(size_t)capitalize];
}

bool jasperengine::autoCapitalizeFromName(std::string_view name, AutoCapitalize& capitalize){
    if (equalsLower(name, "none")){
        capitalize = AutoCapitalize::Off;
        return true;
    }
    if (equalsLower(name, "on")){
        capitalize = AutoCapitalize::Sentences;
        return true;
    }
    return fromNames(name, capitalizeNames, capitalize);
}
