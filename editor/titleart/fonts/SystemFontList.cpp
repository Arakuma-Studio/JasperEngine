// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#if defined(_WIN32)
    // before anything includes windows.h (CommandRunner.h does)
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
    #include <dwrite.h>
#endif

#include "SystemFontList.h"

#include "Log.h"
#include "jobs/JobSystem.h"
#include "util/CommandRunner.h"

#include <algorithm>
#include <cstdlib>
#include <set>
#include <utility>

namespace jasperengine::editor::titlefonts {

    struct SystemFontList::Shared {
        mutable std::mutex mutex;
        State state = State::Idle;
        std::vector<SystemFontFace> faces;
        // a reset() while a listing runs: that listing's result is dropped
        unsigned generation = 0;
    };

    namespace {

        // fontconfig's value list: values separated by unescaped commas, backslash escapes
        // (fc-list writes "\-" and "\," inside names); the first value, unescaped
        std::string firstValue(std::string_view field) {
            std::string out;
            for (size_t i = 0; i < field.size(); i++) {
                const char c = field[i];
                if (c == '\\' && i + 1 < field.size()) {
                    out += field[++i];
                } else if (c == ',') {
                    break;
                } else {
                    out += c;
                }
            }
            return out;
        }

        std::vector<std::string_view> splitTabs(std::string_view line) {
            std::vector<std::string_view> fields;
            size_t start = 0;
            while (true) {
                const size_t tab = line.find('\t', start);
                fields.push_back(line.substr(start, tab == std::string_view::npos ? std::string_view::npos : tab - start));
                if (tab == std::string_view::npos) break;
                start = tab + 1;
            }
            return fields;
        }

        void sortFaces(std::vector<SystemFontFace>& faces) {
            std::sort(faces.begin(), faces.end(), [](const SystemFontFace& a, const SystemFontFace& b) {
                if (a.family != b.family) return a.family < b.family;
                if (a.style != b.style) return a.style < b.style;
                if (a.path != b.path) return a.path < b.path;
                return a.faceIndex < b.faceIndex;
            });
        }

#if defined(_WIN32)
        // the interface ids, spelled out: MinGW's dwrite.h does not declare every __uuidof
        const GUID IID_DWriteFactory = {0xb859ee5a, 0xd838, 0x4b5b, {0xa2, 0xe8, 0x1a, 0xdc, 0x7d, 0x93, 0xdb, 0x48}};
        const GUID IID_DWriteLocalFontFileLoader = {0xb2d9f3ec, 0xc9fe, 0x4a11, {0xa2, 0xec, 0xd8, 0x62, 0x08, 0xf7, 0xc0, 0xa2}};

        template <class T>
        void release(T*& object) {
            if (object) object->Release();
            object = nullptr;
        }

        std::string narrow(const wchar_t* text) {
            if (!text || !*text) return "";
            const int length = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
            if (length <= 1) return "";
            std::string out((size_t)length - 1, '\0');
            WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), length, nullptr, nullptr);
            return out;
        }

        // the English string of a localized list, else its first
        std::string englishString(IDWriteLocalizedStrings* strings) {
            if (!strings || strings->GetCount() == 0) return "";
            UINT32 index = 0;
            BOOL exists = FALSE;
            if (FAILED(strings->FindLocaleName(L"en-us", &index, &exists)) || !exists) index = 0;
            UINT32 length = 0;
            if (FAILED(strings->GetStringLength(index, &length))) return "";
            std::wstring text((size_t)length + 1, L'\0');
            if (FAILED(strings->GetString(index, text.data(), length + 1))) return "";
            return narrow(text.c_str());
        }

        // one face's file path and index (false for a font that is not a local file)
        bool faceFile(IDWriteFont* font, std::string& path, int& index, DWRITE_FONT_FACE_TYPE& type) {
            IDWriteFontFace* face = nullptr;
            if (FAILED(font->CreateFontFace(&face)) || !face) return false;
            bool ok = false;
            UINT32 fileCount = 0;
            if (SUCCEEDED(face->GetFiles(&fileCount, nullptr)) && fileCount == 1) {
                IDWriteFontFile* file = nullptr;
                if (SUCCEEDED(face->GetFiles(&fileCount, &file)) && file) {
                    const void* key = nullptr;
                    UINT32 keySize = 0;
                    IDWriteFontFileLoader* loader = nullptr;
                    IDWriteLocalFontFileLoader* local = nullptr;
                    if (SUCCEEDED(file->GetReferenceKey(&key, &keySize)) && SUCCEEDED(file->GetLoader(&loader)) && loader &&
                        SUCCEEDED(loader->QueryInterface(IID_DWriteLocalFontFileLoader, reinterpret_cast<void**>(&local))) && local) {
                        UINT32 length = 0;
                        if (SUCCEEDED(local->GetFilePathLengthFromKey(key, keySize, &length))) {
                            std::wstring wide((size_t)length + 1, L'\0');
                            if (SUCCEEDED(local->GetFilePathFromKey(key, keySize, wide.data(), length + 1))) {
                                path = narrow(wide.c_str());
                                index = (int)face->GetIndex();
                                type = face->GetType();
                                ok = !path.empty();
                            }
                        }
                    }
                    release(local);
                    release(loader);
                    release(file);
                }
            }
            release(face);
            return ok;
        }

        std::vector<SystemFontFace> listDirectWriteFonts() {
            std::vector<SystemFontFace> out;
            IDWriteFactory* factory = nullptr;
            if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, IID_DWriteFactory, reinterpret_cast<IUnknown**>(&factory))) || !factory) {
                Log::warn("Title Designer: DirectWrite is not available; the System fonts section stays empty");
                return out;
            }
            IDWriteFontCollection* collection = nullptr;
            if (SUCCEEDED(factory->GetSystemFontCollection(&collection, FALSE)) && collection) {
                std::set<std::pair<std::string, int>> seen;
                const UINT32 familyCount = collection->GetFontFamilyCount();
                for (UINT32 f = 0; f < familyCount; f++) {
                    IDWriteFontFamily* family = nullptr;
                    if (FAILED(collection->GetFontFamily(f, &family)) || !family) continue;
                    IDWriteLocalizedStrings* familyNames = nullptr;
                    std::string familyName;
                    if (SUCCEEDED(family->GetFamilyNames(&familyNames))) familyName = englishString(familyNames);
                    release(familyNames);
                    const UINT32 fontCount = family->GetFontCount();
                    for (UINT32 i = 0; i < fontCount; i++) {
                        IDWriteFont* font = nullptr;
                        if (FAILED(family->GetFont(i, &font)) || !font) continue;
                        // synthesized bold and oblique faces are not files of their own
                        if (font->GetSimulations() == DWRITE_FONT_SIMULATIONS_NONE) {
                            SystemFontFace entry;
                            DWRITE_FONT_FACE_TYPE type = DWRITE_FONT_FACE_TYPE_UNKNOWN;
                            if (faceFile(font, entry.path, entry.faceIndex, type) &&
                                (type == DWRITE_FONT_FACE_TYPE_CFF || type == DWRITE_FONT_FACE_TYPE_TRUETYPE ||
                                 type == DWRITE_FONT_FACE_TYPE_TRUETYPE_COLLECTION) &&
                                seen.insert({entry.path, entry.faceIndex}).second) {
                                IDWriteLocalizedStrings* faceNames = nullptr;
                                if (SUCCEEDED(font->GetFaceNames(&faceNames))) entry.style = englishString(faceNames);
                                release(faceNames);
                                entry.family = familyName;
                                entry.format = type == DWRITE_FONT_FACE_TYPE_CFF ? SystemFontFormat::Cff : SystemFontFormat::TrueType;
                                out.push_back(std::move(entry));
                            }
                        }
                        release(font);
                    }
                    release(family);
                }
            }
            release(collection);
            release(factory);
            return out;
        }
#endif

#if !defined(_WIN32) && !defined(__APPLE__)
        std::vector<SystemFontFace> listFontconfigFonts() {
            const std::string output = CommandRunner::capture(
                {"fc-list", "--format", "%{file}\\t%{index}\\t%{family}\\t%{style}\\t%{fontformat}\\n"}, 5000);
            if (output.empty()) Log::warn("Title Designer: fc-list gave no fonts (is fontconfig installed?); the System fonts section stays empty");
            return parseFcList(output);
        }
#endif

    }

    std::vector<SystemFontFace> parseFcList(std::string_view output) {
        std::vector<SystemFontFace> faces;
        std::set<std::pair<std::string, int>> seen;
        size_t start = 0;
        while (start < output.size()) {
            size_t end = output.find('\n', start);
            if (end == std::string_view::npos) end = output.size();
            std::string_view line = output.substr(start, end - start);
            start = end + 1;
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            const std::vector<std::string_view> fields = splitTabs(line);
            if (fields.size() < 5 || fields[0].empty()) continue;
            SystemFontFace face;
            const std::string format = firstValue(fields[4]);
            if (format == "TrueType") face.format = SystemFontFormat::TrueType;
            else if (format == "CFF") face.format = SystemFontFormat::Cff;
            else continue;
            const std::string indexText(fields[1]);
            char* parsedEnd = nullptr;
            const long index = std::strtol(indexText.c_str(), &parsedEnd, 10);
            if (indexText.empty() || (parsedEnd && *parsedEnd) || index < 0) continue;
            // fontconfig numbers a variable font's named instances (instance << 16) | face
            if (index >= 65536) continue;
            face.path = std::string(fields[0]);
            face.faceIndex = (int)index;
            face.family = firstValue(fields[2]);
            face.style = firstValue(fields[3]);
            if (face.family.empty()) continue;
            if (!seen.insert({face.path, face.faceIndex}).second) continue;
            faces.push_back(std::move(face));
        }
        sortFaces(faces);
        return faces;
    }

    SystemFontList& SystemFontList::instance() {
        static SystemFontList list;
        return list;
    }

    SystemFontList::SystemFontList() : shared(std::make_shared<Shared>()) {}

    SystemFontList::~SystemFontList() = default;

    void SystemFontList::request() {
        unsigned generation = 0;
        {
            std::lock_guard<std::mutex> lock(shared->mutex);
            if (shared->state != State::Idle) return;
            shared->state = State::Listing;
            generation = shared->generation;
        }
        // fontconfig, CoreText and DirectWrite each take from tens of milliseconds to seconds
        jobs::enqueueDetached(
            [state = shared, run = lister, generation]() {
                std::vector<SystemFontFace> found = run ? run() : listPlatform();
                std::lock_guard<std::mutex> lock(state->mutex);
                if (state->generation != generation) return;
                state->faces = std::move(found);
                state->state = State::Ready;
            },
            jobs::Lane::Background, "title-system-fonts");
    }

    SystemFontList::State SystemFontList::state() const {
        std::lock_guard<std::mutex> lock(shared->mutex);
        return shared->state;
    }

    std::vector<SystemFontFace> SystemFontList::faces() const {
        std::lock_guard<std::mutex> lock(shared->mutex);
        return shared->state == State::Ready ? shared->faces : std::vector<SystemFontFace>();
    }

    std::vector<SystemFontFace> SystemFontList::listPlatform() {
        std::vector<SystemFontFace> faces;
#if defined(_WIN32)
        faces = listDirectWriteFonts();
#elif defined(__APPLE__)
        faces = listCoreTextFonts();
#else
        faces = listFontconfigFonts();
#endif
        sortFaces(faces);
        return faces;
    }

    void SystemFontList::setLister(Lister newLister) {
        lister = std::move(newLister);
    }

    void SystemFontList::reset() {
        std::lock_guard<std::mutex> lock(shared->mutex);
        shared->generation++;
        shared->state = State::Idle;
        shared->faces.clear();
    }

}
