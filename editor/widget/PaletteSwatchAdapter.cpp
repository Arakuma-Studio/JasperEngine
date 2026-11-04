// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "PaletteSwatchAdapter.h"

#include "pixelart/PixelColor.h"
#include "util/FileUtils.h"

using namespace jasperengine;
using namespace jasperengine::editor;

SwatchLibrary editor::paletteSwatchLibrary(const jasperengine::pixelart::Palette& palette, const std::filesystem::path& file) {
    SwatchLibrary library;
    library.name = palette.name.empty() ? FileUtils::pathToUtf8(file.stem()) : palette.name;
    library.scope = LibraryScope::Project;
    library.path = file;
    library.readOnly = true;
    library.swatches.reserve(palette.colors.size());
    for (int i = 0; i < palette.count(); i++) {
        Swatch swatch;
        swatch.name = palette.colorName(i);
        swatch.linear = jasperengine::pixelart::toEngineLinear(palette.colors[i]);
        swatch.hdr = false;
        library.swatches.push_back(std::move(swatch));
    }
    return library;
}
