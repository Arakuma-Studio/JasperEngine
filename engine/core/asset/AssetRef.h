// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef ASSETREF_H
#define ASSETREF_H

#include "Guid.h"
#include <filesystem>
#include <string>
#include <utility>

namespace jasperengine {

    // The read-only std::string surface of a path held in `path`, so code that used a plain
    // path string (engine systems, generated code, user C++ scripts) keeps compiling
    template<class Derived>
    struct AssetPathText {
        operator const std::string&() const { return text(); }
        operator std::filesystem::path() const { return std::filesystem::path(text()); }
        const std::string& str() const { return text(); }
        const char* c_str() const { return text().c_str(); }
        const char* data() const { return text().data(); }
        size_t size() const { return text().size(); }
        size_t length() const { return text().size(); }
        char front() const { return text().front(); }
        char back() const { return text().back(); }
        char operator[](size_t i) const { return text()[i]; }
        std::string::const_iterator begin() const { return text().begin(); }
        std::string::const_iterator end() const { return text().end(); }

        template<class... A> size_t find(A&&... a) const { return text().find(std::forward<A>(a)...); }
        template<class... A> size_t rfind(A&&... a) const { return text().rfind(std::forward<A>(a)...); }
        template<class... A> size_t find_first_of(A&&... a) const { return text().find_first_of(std::forward<A>(a)...); }
        template<class... A> size_t find_last_of(A&&... a) const { return text().find_last_of(std::forward<A>(a)...); }
        template<class... A> std::string substr(A&&... a) const { return text().substr(std::forward<A>(a)...); }
        template<class... A> int compare(A&&... a) const { return text().compare(std::forward<A>(a)...); }

        friend std::string operator+(const Derived& a, const std::string& b) { return a.path + b; }
        friend std::string operator+(const std::string& a, const Derived& b) { return a + b.path; }
        friend std::string operator+(const Derived& a, const char* b) { return a.path + b; }
        friend std::string operator+(const char* a, const Derived& b) { return a + b.path; }
        friend std::string operator+(const Derived& a, char b) { return a.path + b; }

        friend bool operator==(const Derived& a, const std::string& b) { return a.path == b; }
        friend bool operator==(const std::string& a, const Derived& b) { return a == b.path; }
        friend bool operator==(const Derived& a, const char* b) { return a.path == b; }
        friend bool operator==(const char* a, const Derived& b) { return b.path == a; }
        friend bool operator!=(const Derived& a, const std::string& b) { return a.path != b; }
        friend bool operator!=(const std::string& a, const Derived& b) { return a != b.path; }
        friend bool operator!=(const Derived& a, const char* b) { return a.path != b; }
        friend bool operator!=(const char* a, const Derived& b) { return b.path != a; }
        friend bool operator<(const Derived& a, const Derived& b) { return a.path < b.path; }

    private:
        const std::string& text() const { return static_cast<const Derived*>(this)->path; }
    };

    // A reference to an asset: its GUID (the identity, which survives renames and moves) and
    // its path (a hint, relative to the same root the field always used). The editor resolves
    // references by GUID and refreshes the hint; exported games get resolved paths, so the
    // runtime loads by path and never looks at the GUID.
    //
    // Assigning a path gives a reference to the file at that path: a different path drops
    // the GUID (it named the asset at the old path) and the editor fills it in again.
    struct AssetRef : AssetPathText<AssetRef> {
        std::string path;
        Guid guid;

        AssetRef() = default;
        AssetRef(std::string path): path(std::move(path)) {}
        AssetRef(const char* path): path(path ? path : "") {}
        AssetRef(std::string path, const Guid& guid): path(std::move(path)), guid(guid) {}

        AssetRef& operator=(std::string newPath) {
            if (newPath != path) {
                guid = Guid();
            }
            path = std::move(newPath);
            return *this;
        }
        AssetRef& operator=(const char* newPath) { return *this = std::string(newPath ? newPath : ""); }

        void set(std::string newPath, const Guid& newGuid) { path = std::move(newPath); guid = newGuid; }
        void clear() { path.clear(); guid = Guid(); }
        // No path (what the runtime checks before loading); isSet() also counts a bare GUID
        bool empty() const { return path.empty(); }
        bool isSet() const { return !path.empty() || !guid.isNull(); }

        bool operator==(const AssetRef& other) const { return path == other.path && guid == other.guid; }
        bool operator!=(const AssetRef& other) const { return !(*this == other); }
    };

    // A reference to a custom shader: one value naming two files, either a shared base path
    // without extension (base.vert + base.frag) or "vert|frag", so it carries the GUIDs of
    // both files. "library:<name>" (an engine library shader) is not a file reference.
    struct ShaderRef : AssetPathText<ShaderRef> {
        std::string path;
        Guid vertGuid;
        Guid fragGuid;

        ShaderRef() = default;
        ShaderRef(std::string path): path(std::move(path)) {}
        ShaderRef(const char* path): path(path ? path : "") {}
        ShaderRef(std::string path, const Guid& vertGuid, const Guid& fragGuid):
            path(std::move(path)), vertGuid(vertGuid), fragGuid(fragGuid) {}

        ShaderRef& operator=(std::string newPath) {
            if (newPath != path) {
                vertGuid = Guid();
                fragGuid = Guid();
            }
            path = std::move(newPath);
            return *this;
        }
        ShaderRef& operator=(const char* newPath) { return *this = std::string(newPath ? newPath : ""); }

        void clear() { path.clear(); vertGuid = Guid(); fragGuid = Guid(); }
        bool empty() const { return path.empty(); }
        bool isSet() const { return !path.empty() || !vertGuid.isNull() || !fragGuid.isNull(); }
        bool isLibrary() const { return path.compare(0, 8, "library:") == 0; }

        bool operator==(const ShaderRef& other) const {
            return path == other.path && vertGuid == other.vertGuid && fragGuid == other.fragGuid;
        }
        bool operator!=(const ShaderRef& other) const { return !(*this == other); }
    };

}

namespace std {
    template<> struct hash<jasperengine::AssetRef> {
        size_t operator()(const jasperengine::AssetRef& ref) const noexcept {
            return hash<string>()(ref.path) ^ (hash<jasperengine::Guid>()(ref.guid) << 1);
        }
    };
}

#endif // ASSETREF_H
