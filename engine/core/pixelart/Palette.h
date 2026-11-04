// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef PIXELART_PALETTE_H
#define PIXELART_PALETTE_H

// The Palette asset (`.palette`, docs/specs/S43-palettes.md R1, §6.2): exact sRGB8 colours, their
// names, locked colours, the transparent index, named ramps (optionally colour cycling) and
// variants aligned index by index with the colours. PaletteCodec reads and writes its JSON
// without the editor; PaletteLibrary is the runtime cache by asset path.

#include "Export.h"
#include "pixelart/PixelColor.h"
#include "pixelart/Rgba8.h"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace jasperengine::pixelart {

    // A typed problem of a palette: kind ("invalid_value", "too_many_colors", ...), the field
    // path ("ramps[1].colors[3]"), what is wrong and how to fix it
    struct Diagnostic {
        enum class Severity : uint8_t { Error, Warning };
        Severity severity = Severity::Error;
        std::string kind;
        std::string field;
        std::string message;
        std::string fix;
    };

    JASPERENGINE_API bool hasErrors(const std::vector<Diagnostic>& diagnostics);

    struct PaletteRamp {
        std::string name;
        std::vector<uint16_t> colors;    // palette indices, in ramp order
        float cycle = 0.0f;              // colour cycling, in steps per second (0: none)

        bool operator==(const PaletteRamp& other) const = default;
    };

    struct PaletteVariant {
        std::string name;
        std::vector<std::optional<Rgba8>> colors;   // aligned with the palette's; nullopt (or past the end) keeps the base colour

        bool operator==(const PaletteVariant& other) const = default;
    };

    // Where a palette came from: kind is user, builtin, lospec, file, texture or aseprite; the
    // other members are kept as read
    struct PaletteSource {
        std::string kind;
        std::string id;
        std::string url;
        std::string fetched;     // ISO date
        std::string file;        // the original file name (file, texture, aseprite)
        std::vector<std::pair<std::string, std::string>> extra;   // other keys, compact JSON

        bool empty() const { return kind.empty() && id.empty() && url.empty() && fetched.empty() && file.empty() && extra.empty(); }
        bool operator==(const PaletteSource& other) const = default;
    };

    struct JASPERENGINE_API Palette {
        static constexpr const char* kFormat = "jasperengine.palette";
        static constexpr int kVersion = 1;
        static constexpr int kMaxColors = 256;
        static constexpr int kMaxRamps = 64;
        static constexpr int kMaxVariants = 15;
        static constexpr int kMaxColumns = 64;
        static constexpr int kMaxCyclingRamps = 8;
        static constexpr int kDefaultColumns = 8;

        std::string name;
        std::string author;
        std::string description;
        PaletteSource source;
        int columns = kDefaultColumns;   // a display hint
        std::vector<Rgba8> colors;
        std::map<int, std::string> names;
        std::vector<uint16_t> locked;
        int transparent = -1;
        std::vector<PaletteRamp> ramps;
        std::vector<PaletteVariant> variants;
        // top-level keys this version does not know, compact JSON in file order: written back
        std::vector<std::pair<std::string, std::string>> unknown;
        // read from a newer format version: usable, never written back over the file
        bool readOnly = false;

        int count() const { return (int)colors.size(); }
        bool validIndex(int index) const { return index >= 0 && index < (int)colors.size(); }
        bool isLocked(int index) const;
        std::string colorName(int index) const;

        const PaletteRamp* findRamp(std::string_view rampName) const;
        int findRampIndex(std::string_view rampName) const;
        int findVariant(std::string_view variantName) const;
        // the ramp's colours, fitted to `steps` (fitRamp) when steps > 0; empty when there is no
        // such ramp
        std::vector<Rgba8> rampColors(std::string_view rampName, int steps = 0) const;
        // a variant's colour at an index, falling back to the base colour
        Rgba8 variantColor(int variant, int index) const;
        // every colour of a variant (the base where it keeps it)
        std::vector<Rgba8> variantColors(int variant) const;
        // uncached convenience for one-off lookups (S46 R8); hot loops build a PaletteMatcher
        int nearest(Rgba8 color, ColorMetric metric = ColorMetric::OkLab) const;
        // the first index holding exactly this colour (alpha included), or -1
        int indexOf(Rgba8 color) const;

        // Renumbers the colours: new colour i is old colour oldOfNew[i] (-1: a new colour, which
        // `fill` gives). Names, locked indices, the transparent index, ramps and variants follow
        // their colours; references to a dropped colour are dropped.
        void remap(const std::vector<int>& oldOfNew, Rgba8 fill = Rgba8{0, 0, 0, 255});

        bool operator==(const Palette& other) const = default;
    };

    namespace PaletteCodec {
        // Parses a `.palette`. Errors and warnings go to `diagnostics`; false on any error. A newer
        // version is read as far as this one understands it, with readOnly set and a warning.
        JASPERENGINE_API bool read(std::string_view json, Palette& out, std::vector<Diagnostic>& diagnostics);
        // The canonical text: fixed key order, the header keys always, the optional ones only
        // when set, the colours `columns` to a line, unknown keys after the known ones
        JASPERENGINE_API std::string write(const Palette& palette);
        // The R2 checks on a palette in memory (what read runs after parsing); true without errors
        JASPERENGINE_API bool validate(const Palette& palette, std::vector<Diagnostic>& diagnostics);
    }

    // The runtime's palettes by asset path (relative to the assets directory), read on first use
    // through the engine's file layer. Hot reload replaces an entry and bumps its version, which
    // the swap and quantize textures built from it check.
    class JASPERENGINE_API PaletteLibrary {
    public:
        // nullptr when the file cannot be read or has errors (diagnostics says why)
        static std::shared_ptr<const Palette> get(const std::string& path, std::vector<Diagnostic>* diagnostics = nullptr);
        static void reload(const std::string& path);
        static void set(const std::string& path, const Palette& palette);
        static void clear();
        // A palette made at run time (Lua's Palette.new): a "runtime:<n>" key that names it
        // anywhere a palette path goes, for as long as something else holds it (the library
        // keeps it weakly: a Lua reference or a Palette Swap row using it)
        static std::string addRuntime(std::shared_ptr<const Palette> palette);
        // 0 while nothing has read the path; changes on every reload or set
        static uint32_t version(const std::string& path);
        // changes whenever any entry changes
        static uint32_t generation();
    };

}

#endif // PIXELART_PALETTE_H
