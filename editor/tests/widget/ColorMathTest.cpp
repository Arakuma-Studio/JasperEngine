// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Color window's numbers (docs/specs/S27-editor-tools.md R2, AC11)

#include "widget/ColorMath.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>

using namespace jasperengine;
using namespace jasperengine::editor;

TEST(ColorMath, HdrDecompose) {
    Vector3 base;
    float intensity = 0.0f;
    colormath::hdrDecompose(Vector3(4.0f, 2.0f, 0.0f), base, intensity);
    EXPECT_FLOAT_EQ(intensity, 2.0f);
    EXPECT_FLOAT_EQ(base.x, 1.0f);
    EXPECT_FLOAT_EQ(base.y, 0.5f);
    EXPECT_FLOAT_EQ(base.z, 0.0f);
    const Vector3 back = colormath::hdrCompose(base, intensity);
    EXPECT_EQ(back, Vector3(4.0f, 2.0f, 0.0f));
    // LDR: intensity 0, the colour itself
    colormath::hdrDecompose(Vector3(0.25f, 0.5f, 1.0f), base, intensity);
    EXPECT_EQ(intensity, 0.0f);
    EXPECT_EQ(base, Vector3(0.25f, 0.5f, 1.0f));

    // a window opened and closed without a change gives the value back bit for bit
    for (const Vector4& stored : {Vector4(4.0f, 2.0f, 0.0f, 1.0f), Vector4(3.7f, 1.3f, 0.21f, 0.5f), Vector4(0.0123f, 0.5f, 0.99f, 1.0f)}) {
        const colormath::ColorPickerModel model(stored, true, true, true);
        EXPECT_FALSE(model.changed());
        const Vector4 value = model.value();
        EXPECT_EQ(std::memcmp(&value, &stored, sizeof(Vector4)), 0);
    }
    // the opened base is the decomposition (shown sRGB of linear (1, 0.5, 0)) at intensity 2
    const colormath::ColorPickerModel opened(Vector4(4.0f, 2.0f, 0.0f, 1.0f), true, true, false);
    EXPECT_FLOAT_EQ(opened.intensity(), 2.0f);
    EXPECT_NEAR(opened.shown().x, 1.0f, 1e-6f);
    EXPECT_NEAR(colormath::srgbToLinear(Vector3(opened.shown().x, opened.shown().y, opened.shown().z)).y, 0.5f, 1e-5f);
}

TEST(ColorMath, HexRoundTripIsLosslessForEveryByte) {
    for (int b = 0; b < 256; b++) {
        // a stored linear value from the byte, shown, hex, read back: the same byte
        const float srgb = colormath::fromByte((uint8_t)b);
        const Vector3 linear = colormath::srgbToLinear(Vector3(srgb, srgb, srgb));
        colormath::ColorPickerModel model(Vector4(linear, 1.0f), false, true, false);
        const std::string hex = model.hex();
        Vector4 parsed;
        ASSERT_TRUE(colormath::fromHex(hex, parsed));
        EXPECT_EQ(colormath::toByte(parsed.x), b) << hex;
        model.setHex(hex);
        const Vector4 stored = model.value();
        EXPECT_EQ(colormath::toByte(colormath::linearToSrgb(Vector3(stored.x, stored.y, stored.z)).x), b);
    }
    Vector4 c;
    bool alpha = false;
    EXPECT_TRUE(colormath::fromHex(" #ff8000 ", c, &alpha));
    EXPECT_FALSE(alpha);
    EXPECT_FLOAT_EQ(c.y, 128.0f / 255.0f);
    EXPECT_TRUE(colormath::fromHex("FF800080", c, &alpha));
    EXPECT_TRUE(alpha);
    EXPECT_FALSE(colormath::fromHex("#FF80", c));
    EXPECT_FALSE(colormath::fromHex("#GG8000", c));
    EXPECT_EQ(colormath::toHex(Vector4(1.0f, 0.5f, 0.0f, 0.25f), true), "FF800040");
    EXPECT_EQ(colormath::toHex(Vector4(2.0f, -1.0f, 0.0f, 1.0f), false), "FF0000");
}

TEST(ColorMath, Hsv) {
    for (const Vector3& rgb : {Vector3(1, 0, 0), Vector3(0.2f, 0.7f, 0.3f), Vector3(0.5f, 0.5f, 0.5f), Vector3(0.1f, 0.2f, 0.9f)}) {
        const Vector3 back = colormath::hsvToRgb(colormath::rgbToHsv(rgb));
        EXPECT_NEAR(back.x, rgb.x, 1e-5f);
        EXPECT_NEAR(back.y, rgb.y, 1e-5f);
        EXPECT_NEAR(back.z, rgb.z, 1e-5f);
    }
    EXPECT_NEAR(colormath::rgbToHsv(Vector3(0, 1, 0)).x, 1.0f / 3.0f, 1e-6f);
}

TEST(ColorMath, ModelEdits) {
    // an LDR field gets a clamped HDR colour (and is told)
    colormath::ColorPickerModel ldr(Vector4(0.5f, 0.5f, 0.5f, 1.0f), false, true, true);
    bool clamped = false;
    ldr.setLinear(Vector4(4.0f, 0.0f, 0.0f, 1.0f), &clamped);
    EXPECT_TRUE(clamped);
    EXPECT_FLOAT_EQ(ldr.value().x, 1.0f);
    // an HDR field takes the intensity of the swatch
    colormath::ColorPickerModel hdr(Vector4(1.0f, 1.0f, 1.0f, 1.0f), true, true, true);
    hdr.setLinear(Vector4(8.0f, 4.0f, 0.0f, 1.0f));
    EXPECT_FLOAT_EQ(hdr.intensity(), 3.0f);
    EXPECT_NEAR(hdr.value().x, 8.0f, 1e-4f);
    // an sRGB field stores what is shown
    colormath::ColorPickerModel srgb(Vector4(0.25f, 0.25f, 0.25f, 1.0f), false, false, true);
    EXPECT_FLOAT_EQ(srgb.shown().x, 0.25f);
    srgb.setShownRgb(Vector3(0.75f, 0.5f, 0.25f));
    EXPECT_FLOAT_EQ(srgb.value().x, 0.75f);
    // alpha only on fields with alpha
    colormath::ColorPickerModel opaque(Vector4(1.0f, 1.0f, 1.0f, 0.3f), false, true, false);
    opaque.setAlpha(0.1f);
    EXPECT_FLOAT_EQ(opaque.value().w, 0.3f);
    // the intensity range
    hdr.setIntensity(40.0f);
    EXPECT_FLOAT_EQ(hdr.intensity(), colormath::kMaxIntensity);
}
