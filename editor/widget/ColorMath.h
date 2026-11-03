// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The Color window's numbers (docs/specs/S27-editor-tools.md R2, AC11), without ImGui: sRGB 8-bit
// hex, HSV, HDR intensity in stops, and ColorPickerModel, what the window edits. Fields store
// linear values (or sRGB ones, ColorSRGB); every slider, number and the hex show sRGB.

#include "math/Vector3.h"
#include "math/Vector4.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace jasperengine::editor::colormath {

    Vector3 linearToSrgb(const Vector3& linear);
    Vector3 srgbToLinear(const Vector3& srgb);

    // a channel in [0, 1] as a byte (clamped, rounded) and back
    uint8_t toByte(float value);
    float fromByte(uint8_t value);

    // "RRGGBB" or "RRGGBBAA" (upper case) of sRGB values in [0, 1]
    std::string toHex(const Vector4& srgb, bool alpha);
    // "#RRGGBB", "RRGGBB", "RRGGBBAA" (any case, with or without '#', spaces around ignored);
    // alpha 1 when the text has none. False when the text is not one of those.
    bool fromHex(std::string_view text, Vector4& srgb, bool* hasAlpha = nullptr);

    // hue, saturation, value in [0, 1] (hue 0 = 1 = red)
    Vector3 rgbToHsv(const Vector3& rgb);
    Vector3 hsvToRgb(const Vector3& hsv);

    // An HDR colour as an LDR base and an intensity in stops: linear = base * 2^intensity, with
    // intensity = max(0, log2(the largest component)), so (4, 2, 0) is (1, 0.5, 0) at 2
    void hdrDecompose(const Vector3& linear, Vector3& base, float& intensity);
    Vector3 hdrCompose(const Vector3& base, float intensity);

    constexpr float kMinIntensity = -10.0f;
    constexpr float kMaxIntensity = 10.0f;

    enum class SliderMode : uint8_t { Rgb255, Rgb01, Hsv };

    // What the Color window edits: the stored colour of the field it opened on, as the window
    // shows it (an sRGB base in [0, 1], alpha, and the HDR intensity). Untouched, value() gives
    // the original back bit for bit, so opening and closing the window changes nothing.
    class ColorPickerModel {
    public:
        ColorPickerModel() = default;
        // `stored`: the field's value (linear when linearStorage, else sRGB as shown)
        ColorPickerModel(const Vector4& stored, bool hdr, bool linearStorage, bool alpha);

        // the shown base colour (sRGB, each channel in [0, 1]) and alpha
        const Vector4& shown() const { return base; }
        float intensity() const { return stops; }
        bool isHdr() const { return hdr; }
        bool hasAlpha() const { return alpha; }
        bool linearStorage() const { return linear; }

        void setShown(const Vector4& srgb);
        void setShownRgb(const Vector3& srgb);
        void setAlpha(float a);
        void setIntensity(float intensity);
        // a hex text over the shown colour (alpha only when the text has it); false when not hex
        bool setHex(std::string_view text);
        // a stored colour of the field's kind (a swatch, an eyedropper sample in linear):
        // clamped to 0-1 on an LDR field (*clamped tells)
        void setLinear(const Vector4& linear, bool* clamped = nullptr);

        std::string hex() const;
        bool changed() const { return dirty; }
        // the value to store: the original while untouched
        Vector4 value() const;
        // the value as linear RGBA, whatever the storage (swatches keep linear values)
        Vector4 linearValue() const;
        const Vector4& original() const { return start; }

    private:
        Vector4 start = Vector4(1.0f, 1.0f, 1.0f, 1.0f);
        Vector4 base = Vector4(1.0f, 1.0f, 1.0f, 1.0f);
        float stops = 0.0f;
        bool hdr = false;
        bool linear = true;
        bool alpha = true;
        bool dirty = false;
    };

}
