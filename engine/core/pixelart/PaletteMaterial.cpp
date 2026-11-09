// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "PaletteMaterial.h"

#include "pixelart/PaletteSwap.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;
using namespace jasperengine::pixelart;

// file-local, inside the namespace: a unity build's other sources never see these names
namespace jasperengine::pixelart {
namespace {

    const Vector4* findParam(const Material& material, const char* name) {
        for (const auto& [member, value] : material.shaderParams) {
            if (member == name) return &value;
        }
        return nullptr;
    }

    void writeParam(Material& material, const char* name, float value) {
        for (auto& [member, current] : material.shaderParams) {
            if (member == name) {
                current = Vector4(value, 0.0f, 0.0f, 0.0f);
                return;
            }
        }
        material.shaderParams.push_back({name, Vector4(value, 0.0f, 0.0f, 0.0f)});
    }

}
}

const PaletteBinding* pixelart::findPaletteBinding(const Material& material, const std::string& uniform) {
    for (const auto& [name, binding] : material.shaderPalettes.get()) {
        if (name == uniform) return &binding;
    }
    return nullptr;
}

PaletteBinding& pixelart::editPaletteBinding(Material& material, const std::string& uniform) {
    ShaderPalettes& palettes = material.shaderPalettes.edit();
    for (auto& [name, binding] : palettes) {
        if (name == uniform) return binding;
    }
    palettes.push_back({uniform, PaletteBinding()});
    return palettes.back().second;
}

std::vector<std::string> pixelart::paletteRowNames(const Material& material, const std::string& uniform) {
    std::vector<std::string> names = {""};
    const PaletteBinding* binding = findPaletteBinding(material, uniform);
    if (!binding || binding->source.empty()) return names;
    std::shared_ptr<const Palette> source = PaletteLibrary::get(binding->source.path);
    if (!source) return names;
    std::vector<Diagnostic> diagnostics;
    for (const PaletteSwapRow& row : resolvePaletteRows(*source, *binding, diagnostics)) {
        if ((int)names.size() == kSwapTargetRows) break;
        names.push_back(row.name);
    }
    return names;
}

int pixelart::getPaletteRow(const Material& material) {
    const Vector4* value = findParam(material, kPaletteRowParam);
    return value ? (int)std::floor(value->x + 0.5f) : 0;
}

void pixelart::setPaletteRow(Material& material, int row) {
    writeParam(material, kPaletteRowParam, (float)std::clamp(row, 0, kSwapTargetRows - 1));
}

float pixelart::getPaletteCycleSpeed(const Material& material) {
    const Vector4* value = findParam(material, kPaletteCycleSpeedParam);
    return value ? value->x : 1.0f;
}

void pixelart::setPaletteCycleSpeed(Material& material, float speed) {
    writeParam(material, kPaletteCycleSpeedParam, std::isfinite(speed) ? speed : 0.0f);
}

bool pixelart::setPaletteTarget(Material& material, int row, const std::string& palette, std::string* error) {
    if (row < 1 || row >= kSwapTargetRows) {
        if (error) *error = "row " + std::to_string(row) + " is not a target row: rows 1 to 15 (0 is the source's own colours)";
        return false;
    }
    PaletteBinding& binding = editPaletteBinding(material);
    if (binding.source.empty()) {
        if (error) *error = "the material has no Source Palette to swap from";
        return false;
    }
    std::vector<PaletteTarget> targets;
    if (binding.useVariants) {
        if (std::shared_ptr<const Palette> source = PaletteLibrary::get(binding.source.path)) {
            for (const PaletteVariant& variant : source->variants) {
                PaletteTarget target;
                target.palette = binding.source;
                target.variant = variant.name;
                targets.push_back(target);
            }
        }
    }
    targets.insert(targets.end(), binding.targets.begin(), binding.targets.end());
    while ((int)targets.size() < row) {
        PaletteTarget padding;
        padding.palette = binding.source;
        targets.push_back(padding);
    }
    targets[row - 1] = PaletteTarget::parse(palette);
    if (palette.rfind("runtime:", 0) == 0) targets[row - 1].runtime = PaletteLibrary::get(targets[row - 1].palette.path);
    binding.targets = std::move(targets);
    binding.useVariants = false;
    return true;
}
