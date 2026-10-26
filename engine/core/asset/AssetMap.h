// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef ASSETMAP_H
#define ASSETMAP_H

#include "Export.h"
#include "Guid.h"
#include "AssetType.h"
#include "AssetRef.h"
#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine {

    // GUID <-> path lookups for games. Scene data and generated code always hold resolved
    // paths, so a game only needs this when a script asks (Lua's Asset module, an Asset
    // script property). Exported games read assetmap.bin, written by the exporter with every
    // shipped asset, its former GUIDs and its sub-assets ("<container>#<key>" paths); it is
    // loaded on the first query, so a game that never asks pays nothing. In the editor's Play
    // the editor installs a Resolver backed by its asset database instead.
    //
    // Paths are relative to the root the asset type uses in references: the assets
    // directory for textures, models, audio, fonts, UI documents and data files, the project
    // for scenes, prefabs, materials, shaders and scripts.
    class JASPERENGINE_API AssetMap {
    public:
        struct Entry {
            Guid guid;
            AssetType type = AssetType::Unknown;
            bool former = false;   // a former GUID of the asset at path (a redirect)
            std::string path;
        };

        struct Resolver {
            virtual ~Resolver() = default;
            virtual bool findPath(const Guid& guid, std::string& path, AssetType& type) const = 0;
            virtual Guid findGuid(const std::string& path) const = 0;
        };

        // Editor Play: queries go to the resolver while it is set (nullptr to unset)
        static void setResolver(const Resolver* resolver);

        static bool findPath(const Guid& guid, std::string& path, AssetType* type = nullptr);
        // The live GUID of the asset at path (never a former one); null when unknown
        static Guid findGuid(const std::string& path);
        static AssetType findType(const std::string& path);

        // The assetmap.bin bytes for entries (sorted and deduplicated here)
        static std::vector<uint8_t> encode(std::vector<Entry> entries);
        // Replaces the loaded map (tests, or a map the host loaded itself); false when malformed
        static bool loadFromMemory(const uint8_t* bytes, size_t size);
        // Forgets the loaded map: the next query reads assetmap.bin again
        static void reset();

        static constexpr const char* fileName = "assetmap.bin";
    };

    // What Lua sees as an AssetRef (Asset.ref(...), the value of an Asset script property):
    // the reference, resolved through AssetMap when read
    //
    //   local ref = Asset.ref("textures/hero.png")
    //   print(ref.guid, ref.path, ref.type, ref.valid)
    class JASPERENGINE_API ScriptAssetRef {
    private:
        AssetRef ref;

    public:
        ScriptAssetRef();
        explicit ScriptAssetRef(const AssetRef& ref);
        // A GUID (32 hex digits) or a path
        static ScriptAssetRef make(const std::string& guidOrPath);

        const AssetRef& getRef() const;
        std::string getGuid() const;   // "" when it has none and the map knows none for its path
        std::string getPath() const;   // the current path: the GUID's, else the path hint
        std::string getType() const;   // "texture", "model", ... ("unknown" when not found)
        bool isValid() const;          // the asset exists in this build / project
        bool isEmpty() const;          // references nothing (a script property left at None)

        bool operator==(const ScriptAssetRef& other) const { return ref == other.ref; }
    };

}

#endif // ASSETMAP_H
