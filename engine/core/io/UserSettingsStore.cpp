// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "UserSettingsStore.h"

#include "FileData.h"
#include "FileUtils.h"
#include "Log.h"
#include "System.h"
#include "tinyxml2.h"
#include "util/Base64.h"

#include <chrono>
#include <cstdio>

using namespace jasperengine;

namespace {

    const char* const ROOT = "userSettings";
    const char* const FILE_PATH = "data://UserSettings.xml";

    // Element names every XML reader accepts (tinyxml2 is more lenient); other keys are
    // written as <entry key="...">
    bool isPlainName(const std::string& key){
        if (key.empty()) return false;
        for (size_t i = 0; i < key.size(); i++){
            const unsigned char c = static_cast<unsigned char>(key[i]);
            const bool start = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
            if (start) continue;
            if (i > 0 && ((c >= '0' && c <= '9') || c == '.' || c == '-')) continue;
            return false;
        }
        return true;
    }

    // What XML text does not keep: NUL, CR (read back as LF), control characters, and a value
    // of nothing but whitespace (the parser drops it)
    bool needsBase64(const std::string& value){
        bool onlySpace = !value.empty();
        for (unsigned char c : value){
            if (c < 0x20 && c != '\t' && c != '\n') return true;
            if (c != ' ' && c != '\t' && c != '\n') onlySpace = false;
        }
        return onlySpace;
    }

}

UserSettingsStore& UserSettingsStore::instance(){
    // never destroyed: ~System of a static System object may still use it at exit
    static UserSettingsStore* store = new UserSettingsStore();
    return *store;
}

double UserSettingsStore::now() const{
    if (clock) return clock();
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

void UserSettingsStore::setClock(std::function<double()> clock){
    std::scoped_lock lock(mutex);
    this->clock = std::move(clock);
}

void UserSettingsStore::ensureLoaded(){
    const System* current = System::hasInstance() ? &System::instance() : nullptr;
    if (loaded && loadedFor == current)
        return;
    if (loaded){
        // another System (a test, a tool): what the old one changed goes to its file first
        if (dirty.load(std::memory_order_acquire)) writeLocked();
        values.clear();
        loaded = false;
    }
    loadedFor = current;
    load();
}

void UserSettingsStore::load(){
    loaded = true;
    values.clear();
    dirty.store(false, std::memory_order_release);
    writeErrorLogged = false;
    if (!System::hasInstance()){
        filePath.clear();
        return;
    }
    filePath = FileData::getSystemPath(FILE_PATH);

    std::string text, error;
    if (!FileUtils::readText(FILE_PATH, text, error)){
        if (error.rfind("not found", 0) != 0)
            Log::error("UserSettings: cannot read %s (%s); starting empty", filePath.c_str(), error.c_str());
        return;
    }
    if (text.find_first_not_of(" \t\r\n") == std::string::npos)
        return;

    tinyxml2::XMLDocument doc(true, tinyxml2::PRESERVE_WHITESPACE);
    tinyxml2::XMLElement* root = nullptr;
    if (doc.Parse(text.data(), text.size()) == tinyxml2::XML_SUCCESS)
        root = doc.RootElement();
    if (!root){
        // kept aside, never overwritten: what can still be read of it stays recoverable
        const std::string aside = filePath + ".corrupt";
        std::string moveError;
        FileUtils::renameFile(filePath, aside, moveError);
        Log::error("UserSettings: %s is not valid XML (%s); moved to %s, starting empty",
                   filePath.c_str(), doc.ErrorStr(), aside.c_str());
        return;
    }
    for (tinyxml2::XMLElement* node = root->FirstChildElement(); node; node = node->NextSiblingElement()){
        const char* keyAttribute = node->Attribute("key");
        const std::string key = keyAttribute ? keyAttribute : node->Name();
        const char* content = node->GetText();
        std::string value = content ? content : "";
        const char* encoding = node->Attribute("encoding");
        if (encoding && std::string(encoding) == "base64"){
            const std::vector<unsigned char> bytes = Base64::decode(value);
            value.assign(bytes.begin(), bytes.end());
        }
        values[key] = value;
    }
}

bool UserSettingsStore::writeLocked(){
    if (filePath.empty()) return false;

    tinyxml2::XMLPrinter printer;
    // the declaration older builds wrote (tinyxml2's NewDeclaration default)
    printer.PushDeclaration("xml version=\"1.0\" encoding=\"UTF-8\"");
    printer.OpenElement(ROOT);
    for (const auto& [key, value] : values){
        const bool plain = isPlainName(key);
        printer.OpenElement(plain ? key.c_str() : "entry");
        if (!plain) printer.PushAttribute("key", key.c_str());
        if (needsBase64(value)){
            printer.PushAttribute("encoding", "base64");
            printer.PushText(Base64::encode(reinterpret_cast<const unsigned char*>(value.data()), value.size()).c_str());
        } else if (!value.empty()){
            printer.PushText(value.c_str());
        }
        printer.CloseElement();
    }
    printer.CloseElement();

    std::string error;
    const std::string bytes(printer.CStr(), static_cast<size_t>(printer.CStrSize() > 0 ? printer.CStrSize() - 1 : 0));
    if (!FileUtils::writeFileAtomic(filePath, bytes, error)){
        if (!writeErrorLogged){
            writeErrorLogged = true;
            Log::error("UserSettings: cannot save (%s); the values stay in memory and saving is tried again", error.c_str());
        }
        // again in a second, not every frame
        dirtySince = now();
        return false;
    }
    dirty.store(false, std::memory_order_release);
    if (System::hasInstance() && loadedFor == &System::instance())
        System::instance().syncFileSystem();
    return true;
}

void UserSettingsStore::markDirty(){
    if (!dirty.load(std::memory_order_acquire)){
        dirtySince = now();
        dirty.store(true, std::memory_order_release);
    }
}

bool UserSettingsStore::get(const std::string& key, std::string& value){
    std::scoped_lock lock(mutex);
    ensureLoaded();
    auto it = values.find(key);
    if (it == values.end()) return false;
    value = it->second;
    return true;
}

bool UserSettingsStore::has(const std::string& key){
    std::scoped_lock lock(mutex);
    ensureLoaded();
    return values.count(key) != 0;
}

void UserSettingsStore::set(const std::string& key, const std::string& value){
    std::scoped_lock lock(mutex);
    ensureLoaded();
    auto it = values.find(key);
    if (it != values.end() && it->second == value) return;
    values[key] = value;
    markDirty();
}

void UserSettingsStore::remove(const std::string& key){
    std::scoped_lock lock(mutex);
    ensureLoaded();
    if (values.erase(key)) markDirty();
}

void UserSettingsStore::clear(){
    std::scoped_lock lock(mutex);
    ensureLoaded();
    if (values.empty()) return;
    values.clear();
    markDirty();
}

std::map<std::string, std::string> UserSettingsStore::entries(){
    std::scoped_lock lock(mutex);
    ensureLoaded();
    return values;
}

bool UserSettingsStore::save(){
    if (!dirty.load(std::memory_order_acquire)) return true;
    std::scoped_lock lock(mutex);
    if (!dirty.load(std::memory_order_acquire)) return true;
    return writeLocked();
}

void UserSettingsStore::tick(){
    // a frame without changes: this branch
    if (!dirty.load(std::memory_order_acquire)) return;
    std::scoped_lock lock(mutex);
    if (dirty.load(std::memory_order_acquire) && now() - dirtySince >= WRITE_DELAY)
        writeLocked();
}

void UserSettingsStore::unload(){
    std::scoped_lock lock(mutex);
    if (loaded && dirty.load(std::memory_order_acquire)) writeLocked();
    values.clear();
    loaded = false;
    loadedFor = nullptr;
    filePath.clear();
    dirty.store(false, std::memory_order_release);
}

void UserSettingsStore::systemDestroyed(const System* system){
    std::scoped_lock lock(mutex);
    if (!loaded || loadedFor != system) return;
    if (dirty.load(std::memory_order_acquire)) writeLocked();
    values.clear();
    loaded = false;
    loadedFor = nullptr;
    filePath.clear();
    dirty.store(false, std::memory_order_release);
}

std::string UserSettingsStore::getFilePath(){
    std::scoped_lock lock(mutex);
    return filePath;
}
