// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The swatch palette (rows of 19, "click the current colour to add it") follows OpenEngine's
// Apps/Editor/Source/Panels/ColorPicker.cpp, Copyright (c) 2026 Gamecrafters Guild Limited and
// contributors, MIT License. See NOTICE.md.

#include "ColorPickerWindow.h"

#include "AppSettings.h"
#include "Platform.h"
#include "Theme.h"
#include "external/IconsFontAwesome6.h"
#include "platform/ScreenColor.h"
#include "shortcuts/CommandCatalog.h"
#include "texture/Framebuffer.h"
#include "util/GraphicUtils.h"

#include "imgui_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::ui;

namespace {

    struct ViewImage {
        ImGuiID viewport = 0;
        ImVec2 min, max;
        Framebuffer* framebuffer = nullptr;
    };

    // the views drawn this frame (registerViewImage) and the frame before (what the picker,
    // drawn after them, reads)
    std::vector<ViewImage>& viewImages() {
        static std::vector<ViewImage> views;
        return views;
    }

    Vector4 shownOf(const Vector4& stored, const ColorFieldOptions& options) {
        Vector3 rgb(stored.x, stored.y, stored.z);
        if (options.hdr) {
            Vector3 base;
            float stops;
            colormath::hdrDecompose(rgb, base, stops);
            rgb = base;
        }
        if (options.linearStorage) rgb = colormath::linearToSrgb(Vector3(std::clamp(rgb.x, 0.0f, 1.0f),
                                                                         std::clamp(rgb.y, 0.0f, 1.0f), std::clamp(rgb.z, 0.0f, 1.0f)));
        return Vector4(rgb, options.alpha ? stored.w : 1.0f);
    }

    ImU32 imColor(const Vector4& srgb, float alpha) {
        return IM_COL32(colormath::toByte(srgb.x), colormath::toByte(srgb.y), colormath::toByte(srgb.z), colormath::toByte(alpha));
    }

    // the texel of a view image under an ImGui position, as sRGB 0-1 (what the view shows)
    bool readViewTexel(const ViewImage& view, ImVec2 at, Vector3& srgb, std::array<uint8_t, 81 * 3>* loupe) {
        if (!view.framebuffer || !view.framebuffer->isCreated()) return false;
        const float w = view.max.x - view.min.x, h = view.max.y - view.min.y;
        if (w <= 0.0f || h <= 0.0f) return false;
        const int fw = (int)view.framebuffer->getWidth(), fh = (int)view.framebuffer->getHeight();
        const int tx = std::clamp((int)((at.x - view.min.x) / w * (float)fw), 0, fw - 1);
        const int ty = std::clamp((int)((at.y - view.min.y) / h * (float)fh), 0, fh - 1);
        if (loupe) {
            const int x0 = std::clamp(tx - 4, 0, std::max(0, fw - 9)), y0 = std::clamp(ty - 4, 0, std::max(0, fh - 9));
            const int bw = std::min(9, fw), bh = std::min(9, fh);
            std::vector<uint8_t> block;
            if (GraphicUtils::readFramebufferRect(view.framebuffer, x0, y0, bw, bh, block)) {
                loupe->fill(0);
                for (int y = 0; y < bh; y++) {
                    for (int x = 0; x < bw; x++) {
                        uint8_t* out = loupe->data() + (y * 9 + x) * 3;
                        const uint8_t* in = block.data() + (y * bw + x) * 4;
                        out[0] = in[0];
                        out[1] = in[1];
                        out[2] = in[2];
                    }
                }
            }
        }
        std::vector<uint8_t> texel;
        if (!GraphicUtils::readFramebufferRect(view.framebuffer, tx, ty, 1, 1, texel) || texel.size() < 4) return false;
        srgb = Vector3(colormath::fromByte(texel[0]), colormath::fromByte(texel[1]), colormath::fromByte(texel[2]));
        return true;
    }

}

// ---- the field

void ui::drawColorSwatch(ImDrawList* drawList, ImVec2 min, ImVec2 max, const Vector4& shown, bool alphaChecker, bool hdrTag, bool mixed) {
    drawList->AddRectFilled(min, max, imColor(shown, 1.0f));
    if (alphaChecker && shown.w < 1.0f) {
        // the right half over a checker, at its alpha
        const ImVec2 half((min.x + max.x) * 0.5f, min.y);
        const float cell = std::max(3.0f, (max.y - min.y) * 0.5f);
        for (float y = half.y; y < max.y; y += cell) {
            for (float x = half.x; x < max.x; x += cell) {
                const bool dark = ((int)((x - half.x) / cell) + (int)((y - half.y) / cell)) % 2 == 0;
                drawList->AddRectFilled(ImVec2(x, y), ImVec2(std::min(x + cell, max.x), std::min(y + cell, max.y)),
                                        dark ? IM_COL32(128, 128, 128, 255) : IM_COL32(200, 200, 200, 255));
            }
        }
        drawList->AddRectFilled(half, max, imColor(shown, shown.w));
    }
    if (mixed) {
        const ImVec2 mid((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f);
        drawList->AddLine(ImVec2(mid.x - 4.0f, mid.y), ImVec2(mid.x + 4.0f, mid.y), IM_COL32(255, 255, 255, 255), 2.0f);
    }
    if (hdrTag) {
        const char* tag = "HDR";
        const ImVec2 size = ImGui::CalcTextSize(tag);
        const ImVec2 at(max.x - size.x - 3.0f, min.y + (max.y - min.y - size.y) * 0.5f);
        drawList->AddRectFilled(ImVec2(at.x - 2.0f, at.y), ImVec2(max.x - 1.0f, at.y + size.y), IM_COL32(0, 0, 0, 160), 2.0f);
        drawList->AddText(at, IM_COL32(255, 255, 255, 255), tag);
    }
    drawList->AddRect(min, max, IM_COL32(0, 0, 0, 200));
}

EditPhase ui::colorField(const char* id, Vector4& rgba, const ColorFieldOptions& options) {
    ImGui::PushID(id);
    const ImGuiID fieldId = ImGui::GetID("##colorField");
    const float width = options.width > 0.0f ? options.width : std::max(ImGui::GetContentRegionAvail().x, ImGui::GetFrameHeight());
    const ImVec2 size(width, ImGui::GetFrameHeight());
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton("##colorField", size);
    const ImVec2 max(min.x + size.x, min.y + size.y);
    ColorPickerWindow& window = ColorPickerWindow::get();
    EditPhase phase = window.consume(fieldId, rgba);
    drawColorSwatch(ImGui::GetWindowDrawList(), min, max, shownOf(rgba, options), options.alpha, options.hdr, options.mixed);
    if (window.targets(fieldId)) {
        ImGui::GetWindowDrawList()->AddRect(ImVec2(min.x - 1, min.y - 1), ImVec2(max.x + 1, max.y + 1), ImGui::GetColorU32(ImGuiCol_NavCursor));
    }
    if (ImGui::IsItemHovered() && options.label) ImGui::SetTooltip("%s: %s", options.label, colormath::toHex(shownOf(rgba, options), options.alpha).c_str());
    if (clicked && !window.targets(fieldId)) {
        window.open(fieldId, rgba, options);
        phase = EditPhase::Begin;
    }
    ImGui::PopID();
    return phase;
}

EditPhase ui::colorField(const char* id, Vector3& rgb, ColorFieldOptions options) {
    options.alpha = false;
    Vector4 value(rgb, 1.0f);
    const EditPhase phase = colorField(id, value, options);
    if (phase == EditPhase::Changing || phase == EditPhase::Cancelled) rgb = Vector3(value.x, value.y, value.z);
    return phase;
}

void ui::registerViewImage(ImGuiID viewport, ImVec2 min, ImVec2 max, Framebuffer* framebuffer) {
    if (!framebuffer) return;
    viewImages().push_back({viewport, min, max, framebuffer});
}

// ---- the window

ColorPickerWindow& ColorPickerWindow::get() {
    static ColorPickerWindow window;
    return window;
}

void ColorPickerWindow::setProjectPath(const std::filesystem::path& path) {
    if (path == projectPath) return;
    projectPath = path;
    librariesLoaded = false;
}

std::filesystem::path ColorPickerWindow::userLibraryDirectory() const {
    return swatches::userDirectory(AppSettings::getConfigDirectory());
}

std::filesystem::path ColorPickerWindow::projectLibraryDirectory() const {
    return projectPath.empty() ? std::filesystem::path() : swatches::projectDirectory(projectPath);
}

void ColorPickerWindow::reloadLibraries() {
    std::vector<std::string> errors;
    const std::string activeName = activeLibrary < loaded.size() ? loaded[activeLibrary].name : std::string();
    loaded = swatches::loadAll(userLibraryDirectory(), projectLibraryDirectory(), &errors);
    librariesLoaded = true;
    activeLibrary = 0;
    for (size_t i = 0; i < loaded.size(); i++) {
        if (loaded[i].name == activeName) activeLibrary = i;
    }
    if (!errors.empty()) warning = errors.front();
}

const std::vector<SwatchLibrary>& ColorPickerWindow::libraries() {
    if (!librariesLoaded) reloadLibraries();
    return loaded;
}

void ColorPickerWindow::open(ImGuiID id, const Vector4& stored, const ColorFieldOptions& fieldOptions) {
    if (target != 0) close(EditPhase::Committed);
    setBeginEditHandler(&ColorPickerWindow::beginEdit);
    standalone = false;
    target = id;
    options = fieldOptions;
    title = options.label ? options.label : "Color";
    current = colormath::ColorPickerModel(stored, options.hdr, options.linearStorage, options.alpha);
    hsvHue = colormath::rgbToHsv(Vector3(current.shown().x, current.shown().y, current.shown().z)).x;
    changed = false;
    ending = EditPhase::None;
    lastSeenFrame = ImGui::GetFrameCount();
    focusOnOpen = true;
    warning.clear();
    hexEditing = false;
    if (!librariesLoaded) reloadLibraries();
}

EditPhase ColorPickerWindow::consume(ImGuiID id, Vector4& value) {
    if (id == 0) return EditPhase::None;
    if (ending != EditPhase::None && endedTarget == id) {
        const EditPhase phase = ending;
        if (phase == EditPhase::Cancelled) value = endedOriginal;
        ending = EditPhase::None;
        endedTarget = 0;
        return phase;
    }
    if (id != target) return EditPhase::None;
    lastSeenFrame = ImGui::GetFrameCount();
    if (!changed) return EditPhase::None;
    changed = false;
    value = current.value();
    return EditPhase::Changing;
}

void ColorPickerWindow::markChanged() {
    changed = true;
    hexEditing = false;
}

void ColorPickerWindow::close(EditPhase how) {
    if (target == 0) return;
    if (sampling || macPicking) stopSampling(how != EditPhase::Cancelled);
    // the session the field's glue began (beginColorEdit): this window ends it
    if (how == EditPhase::Cancelled) FieldEditSession::cancel(this);
    else FieldEditSession::commit(this);
    endedTarget = target;
    endedOriginal = current.original();
    ending = how;
    target = 0;
    changed = false;
    pendingClose = EditPhase::None;
    standalone = false;
}

void ColorPickerWindow::requestClose(EditPhase how) {
    // a change the field has not had yet reaches it first (the window draws after the fields:
    // the next frame's field, then this window closes)
    if (how == EditPhase::Committed && changed) {
        pendingClose = how;
        return;
    }
    close(how);
}

void ColorPickerWindow::openStandalone() {
    if (standalone && target != 0) {
        focusOnOpen = true;
        return;
    }
    ColorFieldOptions standaloneOptions;
    standaloneOptions.alpha = true;
    standaloneOptions.linearStorage = false;
    standaloneOptions.label = "Color Picker";
    // an id no field has (ImGui ids are hashes; this one is never drawn)
    open(0x5C27C010u, Vector4(1.0f, 1.0f, 1.0f, 1.0f), standaloneOptions);
    standalone = true;
}

void ColorPickerWindow::commit() {
    requestClose(EditPhase::Committed);
}

void ColorPickerWindow::cancel() {
    close(EditPhase::Cancelled);
}

void ColorPickerWindow::applySwatch(const Swatch& swatch) {
    // the model shows sRGB for either storage: the swatch's linear value as shown
    bool clamped = false;
    current.setLinear(swatch.linear, &clamped);
    warning = (swatch.hdr && !current.isHdr()) || clamped ? "An HDR swatch on an LDR field: clamped to 0-1." : std::string();
    markChanged();
}

void ColorPickerWindow::saveActive() {
    if (activeLibrary >= loaded.size()) return;
    std::string error;
    if (!swatches::write(loaded[activeLibrary], error)) warning = error;
}

void ColorPickerWindow::show() {
    // a field's last change reaches it before the session ends: the window is drawn after the
    // fields, and a field not drawn for a frame (its window closed) ends the session as a commit
    if (target != 0 && !standalone && ImGui::GetFrameCount() - lastSeenFrame > 1) close(EditPhase::Committed);
    // no field takes a standalone window's changes
    if (standalone) changed = false;
    if (target != 0 && pendingClose != EditPhase::None && !changed) close(pendingClose);
    windowFocused = false;
    if (target == 0) {
        viewImages().clear();
        return;
    }
    if (sampling) updateSampling();

    bool open = true;
    ImGui::SetNextWindowSize(ImVec2(Theme::dpi(300.0f), Theme::dpi(560.0f)), ImGuiCond_FirstUseEver);
    if (focusOnOpen) {
        ImGui::SetNextWindowFocus();
        focusOnOpen = false;
    }
    const std::string windowTitle = title + "###" + WINDOW_NAME;
    if (ImGui::Begin(windowTitle.c_str(), &open, ImGuiWindowFlags_NoCollapse)) {
        const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        windowFocused = focused;
        drawPicker();
        drawSliders();
        drawHexAlphaIntensity();
        if (!warning.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.3f, 1.0f));
            ImGui::TextWrapped("%s", warning.c_str());
            ImGui::PopStyleColor();
        }
        ImGui::Separator();
        drawSwatches();
        if (sampling) drawLoupe(ImVec2(), true);

        if (focused && !ImGui::IsAnyItemActive() && !sampling && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopup)) {
            if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) cancelRequested = true;
            if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) {
                open = false;
            }
        }
    }
    ImGui::End();
    if (sampling) {
        int x, y;
        if (screencolor::cursor(x, y)) {
            // the loupe near the cursor, in whichever editor window holds it
            for (ImGuiViewport* viewport : ImGui::GetPlatformIO().Viewports) {
                float lx, ly;
                if (screencolor::toWindow(viewport->PlatformHandleRaw, x, y, lx, ly)) {
                    drawLoupe(ImVec2(viewport->Pos.x + lx + 24.0f, viewport->Pos.y + ly + 24.0f), false);
                    break;
                }
            }
        }
    }
    if (cancelRequested) {
        cancelRequested = false;
        cancel();
    } else if (!open) {
        commit();
    }
    viewImages().clear();
}

void ColorPickerWindow::drawPicker() {
    // previous | current
    const float rowHeight = ImGui::GetFrameHeight();
    const float width = ImGui::GetContentRegionAvail().x;
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const Vector4 previous = colormath::ColorPickerModel(current.original(), options.hdr, options.linearStorage, options.alpha).shown();
    drawColorSwatch(drawList, at, ImVec2(at.x + width * 0.5f, at.y + rowHeight), previous, options.alpha, false, false);
    drawColorSwatch(drawList, ImVec2(at.x + width * 0.5f, at.y), ImVec2(at.x + width, at.y + rowHeight), current.shown(), options.alpha,
                    false, false);
    ImGui::InvisibleButton("##previous", ImVec2(width * 0.5f, rowHeight));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Previous colour (click to restore)");
    if (ImGui::IsItemClicked()) {
        current = colormath::ColorPickerModel(current.original(), options.hdr, options.linearStorage, options.alpha);
        markChanged();
    }
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::InvisibleButton("##current", ImVec2(width * 0.5f, rowHeight));

    // the eyedropper and the square / wheel toggle
    if (options.eyedropper) {
        const bool canSample = screencolor::liveSupported() || screencolor::pickOnceSupported();
        ImGui::BeginDisabled(!canSample || sampling || macPicking);
        if (ImGui::Button(ICON_FA_EYE_DROPPER)) startSampling();
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", CommandCatalog::editor().tooltip("colorPicker.eyedropper", "Eyedropper").c_str());
        ImGui::SameLine();
    }
    if (ImGui::Button(hueWheel ? "Square" : "Wheel")) hueWheel = !hueWheel;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(hueWheel ? "Show the saturation/value square" : "Show the hue wheel");

    float rgba[4] = {current.shown().x, current.shown().y, current.shown().z, current.shown().w};
    ImGuiColorEditFlags flags = ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_NoSmallPreview |
                                ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_NoTooltip |
                                (hueWheel ? ImGuiColorEditFlags_PickerHueWheel : ImGuiColorEditFlags_PickerHueBar);
    flags |= options.alpha ? ImGuiColorEditFlags_AlphaBar : ImGuiColorEditFlags_NoAlpha;
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    if (ImGui::ColorPicker4("##picker", rgba, flags)) {
        current.setShown(Vector4(rgba[0], rgba[1], rgba[2], options.alpha ? rgba[3] : current.shown().w));
        const Vector3 hsv = colormath::rgbToHsv(Vector3(rgba[0], rgba[1], rgba[2]));
        if (hsv.y > 0.0f && hsv.z > 0.0f) hsvHue = hsv.x;
        markChanged();
    }
}

void ColorPickerWindow::drawSliders() {
    static const char* const kModes[] = {"RGB 0-255", "RGB 0-1.0", "HSV"};
    int mode = (int)sliderMode;
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    if (ImGui::Combo("##sliderMode", &mode, kModes, 3)) sliderMode = (colormath::SliderMode)mode;

    const Vector4 shown = current.shown();
    const float labelWidth = ImGui::CalcTextSize("W").x + ImGui::GetStyle().ItemSpacing.x * 2.0f;
    auto row = [&](const char* label, auto&& widget) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::SameLine(labelWidth);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        return widget();
    };
    switch (sliderMode) {
        case colormath::SliderMode::Rgb255: {
            int c[3] = {colormath::toByte(shown.x), colormath::toByte(shown.y), colormath::toByte(shown.z)};
            static const char* const kLabels[3] = {"R", "G", "B"};
            for (int i = 0; i < 3; i++) {
                ImGui::PushID(i);
                if (row(kLabels[i], [&] { return ImGui::SliderInt("##c", &c[i], 0, 255); })) {
                    current.setShownRgb(Vector3(colormath::fromByte((uint8_t)c[0]), colormath::fromByte((uint8_t)c[1]),
                                                colormath::fromByte((uint8_t)c[2])));
                    markChanged();
                }
                ImGui::PopID();
            }
            break;
        }
        case colormath::SliderMode::Rgb01: {
            float c[3] = {shown.x, shown.y, shown.z};
            static const char* const kLabels[3] = {"R", "G", "B"};
            for (int i = 0; i < 3; i++) {
                ImGui::PushID(i);
                if (row(kLabels[i], [&] { return ImGui::SliderFloat("##c", &c[i], 0.0f, 1.0f, "%.3f"); })) {
                    current.setShownRgb(Vector3(c[0], c[1], c[2]));
                    markChanged();
                }
                ImGui::PopID();
            }
            break;
        }
        case colormath::SliderMode::Hsv: {
            Vector3 hsv = colormath::rgbToHsv(Vector3(shown.x, shown.y, shown.z));
            if (hsv.y <= 0.0f || hsv.z <= 0.0f) hsv.x = hsvHue;
            float h = hsv.x * 360.0f, s = hsv.y * 100.0f, v = hsv.z * 100.0f;
            bool edited = false;
            ImGui::PushID("h");
            edited |= row("H", [&] { return ImGui::SliderFloat("##c", &h, 0.0f, 360.0f, "%.0f"); });
            ImGui::PopID();
            ImGui::PushID("s");
            edited |= row("S", [&] { return ImGui::SliderFloat("##c", &s, 0.0f, 100.0f, "%.0f"); });
            ImGui::PopID();
            ImGui::PushID("v");
            edited |= row("V", [&] { return ImGui::SliderFloat("##c", &v, 0.0f, 100.0f, "%.0f"); });
            ImGui::PopID();
            if (edited) {
                hsvHue = std::clamp(h, 0.0f, 360.0f) / 360.0f;
                current.setShownRgb(colormath::hsvToRgb(Vector3(hsvHue, std::clamp(s, 0.0f, 100.0f) / 100.0f, std::clamp(v, 0.0f, 100.0f) / 100.0f)));
                markChanged();
            }
            break;
        }
    }
}

void ColorPickerWindow::drawHexAlphaIntensity() {
    const float labelWidth = ImGui::CalcTextSize("Hexadecimal").x + ImGui::GetStyle().ItemSpacing.x * 2.0f;
    if (options.alpha) {
        float a = current.shown().w;
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("A");
        ImGui::SameLine(labelWidth);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        if (sliderMode == colormath::SliderMode::Rgb255) {
            int ai = colormath::toByte(a);
            if (ImGui::SliderInt("##alpha", &ai, 0, 255)) {
                current.setAlpha(colormath::fromByte((uint8_t)ai));
                markChanged();
            }
        } else if (ImGui::SliderFloat("##alpha", &a, 0.0f, 1.0f, "%.3f")) {
            current.setAlpha(a);
            markChanged();
        }
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Hexadecimal");
    ImGui::SameLine(labelWidth);
    if (!hexEditing) {
        const std::string hex = current.hex();
        std::snprintf(hexText.data(), hexText.size(), "%s", hex.c_str());
    }
    const float copyWidth = ImGui::CalcTextSize("Copy").x + ImGui::GetStyle().FramePadding.x * 2.0f + ImGui::GetStyle().ItemSpacing.x;
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - copyWidth);
    ImGui::InputText("##hex", hexText.data(), hexText.size(), ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_CharsUppercase);
    const bool hexActivated = ImGui::IsItemActivated(), hexDeactivated = ImGui::IsItemDeactivated(),
               hexEdited = ImGui::IsItemDeactivatedAfterEdit();
    ImGui::SameLine();
    if (ImGui::Button("Copy##hex")) ImGui::SetClipboardText(("#" + current.hex()).c_str());
    if (hexActivated) hexEditing = true;
    if (hexDeactivated) {
        hexEditing = false;
        if (!hexEdited || !current.setHex(hexText.data())) {
            if (hexEdited) warning = "Hexadecimal is RRGGBB or RRGGBBAA.";
        } else {
            warning.clear();
            markChanged();
        }
    }
    if (options.hdr) {
        float stops = current.intensity();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Intensity");
        ImGui::SameLine(labelWidth);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        if (ImGui::SliderFloat("##intensity", &stops, colormath::kMinIntensity, colormath::kMaxIntensity, "%.2f")) {
            current.setIntensity(stops);
            markChanged();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Stops: the colour is multiplied by 2^Intensity");
    }
}

void ColorPickerWindow::drawLibraryMenu() {
    const char* preview = activeLibrary < loaded.size() ? loaded[activeLibrary].name.c_str() : "(none)";
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight() - ImGui::GetStyle().ItemSpacing.x);
    if (ImGui::BeginCombo("##library", preview)) {
        for (size_t i = 0; i < loaded.size(); i++) {
            const std::string label = loaded[i].name + (loaded[i].scope == LibraryScope::Project ? " (Project)" : "") +
                                      (loaded[i].readOnly ? " (read-only)" : "") + "##" + std::to_string(i);
            if (ImGui::Selectable(label.c_str(), i == activeLibrary)) activeLibrary = i;
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_ELLIPSIS_VERTICAL "##libraryMenu")) ImGui::OpenPopup("##libraryActions");
    if (ImGui::BeginPopup("##libraryActions")) {
        if (ImGui::MenuItem("New Library...")) {
            newLibraryPopup = true;
            newLibraryProject = false;
            nameText.fill(0);
        }
        if (!projectPath.empty() && ImGui::MenuItem("New Project Library...")) {
            newLibraryPopup = true;
            newLibraryProject = true;
            nameText.fill(0);
        }
        const bool editable = activeLibrary < loaded.size() && !loaded[activeLibrary].readOnly;
        if (ImGui::MenuItem("Rename...", nullptr, false, editable)) {
            renameLibraryPopup = true;
            std::snprintf(nameText.data(), nameText.size(), "%s", loaded[activeLibrary].name.c_str());
        }
        if (ImGui::MenuItem("Delete", nullptr, false, activeLibrary < loaded.size())) {
            std::string error;
            if (swatches::deleteLibrary(loaded[activeLibrary], error)) reloadLibraries();
            else warning = error;
        }
        if (ImGui::MenuItem("Reveal in File Browser", nullptr, false, activeLibrary < loaded.size())) {
            std::string error;
            if (!Platform::openWithSystemDefault(loaded[activeLibrary].path.parent_path().string(), &error)) warning = error;
        }
        ImGui::EndPopup();
    }
    if (newLibraryPopup) {
        ImGui::OpenPopup("New Library");
        newLibraryPopup = false;
    }
    if (renameLibraryPopup) {
        ImGui::OpenPopup("Rename Library");
        renameLibraryPopup = false;
    }
    for (const char* popup : {"New Library", "Rename Library"}) {
        if (!ImGui::BeginPopupModal(popup, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) continue;
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        const bool entered = ImGui::InputText("Name", nameText.data(), nameText.size(), ImGuiInputTextFlags_EnterReturnsTrue);
        if (entered || ImGui::Button("OK")) {
            std::string error;
            bool ok;
            if (std::strcmp(popup, "New Library") == 0) {
                SwatchLibrary created;
                ok = swatches::createLibrary(newLibraryProject ? projectLibraryDirectory() : userLibraryDirectory(),
                                             newLibraryProject ? LibraryScope::Project : LibraryScope::User, nameText.data(), created, error);
                if (ok) {
                    reloadLibraries();
                    for (size_t i = 0; i < loaded.size(); i++) {
                        if (loaded[i].path == created.path) activeLibrary = i;
                    }
                }
            } else {
                ok = swatches::renameLibrary(loaded[activeLibrary], nameText.data(), error);
            }
            if (!ok) warning = error;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

void ColorPickerWindow::drawSwatches() {
    ImGui::TextUnformatted("Swatches");
    if (!librariesLoaded) reloadLibraries();
    drawLibraryMenu();
    if (activeLibrary >= loaded.size()) return;
    SwatchLibrary& library = loaded[activeLibrary];
    if (library.readOnly) ImGui::TextDisabled("%s", library.warning.c_str());

    const float spacing = 2.0f;
    const float cell = std::floor((ImGui::GetContentRegionAvail().x - spacing * (SwatchLibrary::kRowLength - 1)) / SwatchLibrary::kRowLength);
    const float size = std::max(cell, 6.0f);
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    for (size_t i = 0; i <= library.swatches.size(); i++) {
        if (i % SwatchLibrary::kRowLength != 0) ImGui::SameLine(0.0f, spacing);
        ImGui::PushID((int)i);
        const ImVec2 min = ImGui::GetCursorScreenPos();
        const ImVec2 max(min.x + size, min.y + size);
        if (i == library.swatches.size()) {
            // "+": the current colour appended
            if (library.swatches.size() < SwatchLibrary::kMaxSwatches && !library.readOnly) {
                ImGui::InvisibleButton("##add", ImVec2(size, size));
                drawList->AddRect(min, max, ImGui::GetColorU32(ImGuiCol_Border));
                const ImVec2 mid((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f);
                const ImU32 text = ImGui::GetColorU32(ImGuiCol_Text);
                drawList->AddLine(ImVec2(mid.x - size * 0.3f, mid.y), ImVec2(mid.x + size * 0.3f, mid.y), text);
                drawList->AddLine(ImVec2(mid.x, mid.y - size * 0.3f), ImVec2(mid.x, mid.y + size * 0.3f), text);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Add the current colour");
                if (ImGui::IsItemClicked()) {
                    std::string error;
                    if (swatches::add(library, {std::string(), current.linearValue(), current.isHdr() && current.intensity() > 0.0f}, error)) {
                        saveActive();
                    } else {
                        warning = error;
                    }
                }
            }
            ImGui::PopID();
            break;
        }
        const Swatch& swatch = library.swatches[i];
        ImGui::InvisibleButton("##swatch", ImVec2(size, size));
        Vector3 shown = colormath::linearToSrgb(Vector3(std::clamp(swatch.linear.x, 0.0f, 1.0f), std::clamp(swatch.linear.y, 0.0f, 1.0f),
                                                        std::clamp(swatch.linear.z, 0.0f, 1.0f)));
        drawColorSwatch(drawList, min, max, Vector4(shown, swatch.linear.w), true, false, false);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s%s#%s", swatch.name.c_str(), swatch.name.empty() ? "" : " ", colormath::toHex(Vector4(shown, swatch.linear.w), true).c_str());
        }
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) applySwatch(swatch);
        if (ImGui::BeginPopupContextItem("##swatchMenu")) {
            std::string error;
            bool edited = false;
            ImGui::BeginDisabled(library.readOnly);
            if (ImGui::MenuItem("Replace")) edited = swatches::replace(library, i, current.linearValue(), current.isHdr(), error);
            if (ImGui::MenuItem("Rename...")) {
                renameSwatch = (int)i;
                std::snprintf(nameText.data(), nameText.size(), "%s", swatch.name.c_str());
            }
            if (ImGui::MenuItem("Delete")) edited = swatches::remove(library, i, error);
            if (ImGui::MenuItem("Move Left", nullptr, false, i > 0)) edited = swatches::move(library, i, -1, error);
            if (ImGui::MenuItem("Move Right", nullptr, false, i + 1 < library.swatches.size())) edited = swatches::move(library, i, 1, error);
            ImGui::EndDisabled();
            if (edited) saveActive();
            else if (!error.empty()) warning = error;
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    if (renameSwatch >= 0) {
        ImGui::OpenPopup("Rename Swatch");
    }
    if (ImGui::BeginPopupModal("Rename Swatch", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        const bool entered = ImGui::InputText("Name", nameText.data(), nameText.size(), ImGuiInputTextFlags_EnterReturnsTrue);
        if (entered || ImGui::Button("OK")) {
            std::string error;
            if (swatches::rename(library, (size_t)renameSwatch, nameText.data(), error)) saveActive();
            else warning = error;
            renameSwatch = -1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            renameSwatch = -1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

// ---- the eyedropper

bool ColorPickerWindow::canEyedropper() const {
    return target != 0 && options.eyedropper && !sampling && !macPicking && !ImGui::IsAnyItemActive() &&
           (screencolor::liveSupported() || screencolor::pickOnceSupported());
}

void ColorPickerWindow::eyedropper() {
    if (canEyedropper()) startSampling();
}

void ColorPickerWindow::startSampling() {
    if (sampling || macPicking) return;
    beforeSampling = current;
    if (screencolor::liveSupported()) {
        std::string why;
        if (!screencolor::begin(why)) {
            warning = "Eyedropper: " + why;
            return;
        }
        sampling = true;
        // the clicks are the eyedropper's until it ends: no widget gets them
        ImGuiIO& io = ImGui::GetIO();
        savedConfigFlags = io.ConfigFlags;
        io.ConfigFlags |= ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoMouseCursorChange;
        loupeValid = false;
        return;
    }
    if (screencolor::pickOnceSupported()) {
        macPicking = true;
        const ImGuiID pickTarget = target;
        screencolor::pickOnce([this, pickTarget](bool picked, float r, float g, float b) {
            macPicking = false;
            if (!picked || target != pickTarget) return;
            current.setShownRgb(Vector3(r, g, b));
            markChanged();
        });
    }
}

void ColorPickerWindow::stopSampling(bool keep) {
    if (sampling) {
        screencolor::end();
        ImGui::GetIO().ConfigFlags = savedConfigFlags;
        sampling = false;
    }
    macPicking = false;
    if (!keep) {
        current = beforeSampling;
        markChanged();
    }
}

void ColorPickerWindow::updateSampling() {
    const screencolor::Button button = screencolor::pollButton();
    if (button == screencolor::Button::Cancel || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        stopSampling(false);
        return;
    }
    int x, y;
    if (!screencolor::cursor(x, y)) return;
    // a Scene or Game view under the cursor: its framebuffer's texel, exact
    Vector3 srgb;
    bool picked = false;
    loupeFromView = false;
    for (ImGuiViewport* viewport : ImGui::GetPlatformIO().Viewports) {
        float lx, ly;
        if (!screencolor::toWindow(viewport->PlatformHandleRaw, x, y, lx, ly)) continue;
        const ImVec2 at(viewport->Pos.x + lx, viewport->Pos.y + ly);
        for (const ViewImage& view : viewImages()) {
            if (view.viewport != viewport->ID || at.x < view.min.x || at.y < view.min.y || at.x >= view.max.x || at.y >= view.max.y) continue;
            if (readViewTexel(view, at, srgb, &loupe)) {
                picked = true;
                loupeFromView = true;
                loupeValid = true;
            }
            break;
        }
        break;
    }
    if (!picked) {
        if (screencolor::read9x9(x, y, loupe.data())) {
            const uint8_t* c = loupe.data() + (4 * 9 + 4) * 3;
            srgb = Vector3(colormath::fromByte(c[0]), colormath::fromByte(c[1]), colormath::fromByte(c[2]));
            picked = true;
            loupeValid = true;
            loupeMessage.clear();
        } else {
            loupeValid = false;
            loupeMessage = screencolor::waylandSession() ? "Outside the editor: not available on Wayland" : "The screen cannot be read here";
        }
    }
    if (picked) {
        current.setShownRgb(srgb);
        markChanged();
    }
    if (button == screencolor::Button::Pick) stopSampling(picked);
}

void ColorPickerWindow::drawLoupe(ImVec2 at, bool inWindow) {
    const float cell = Theme::dpi(9.0f);
    const float size = cell * screencolor::kLoupeSize;
    ImDrawList* drawList = nullptr;
    ImVec2 min;
    if (inWindow) {
        min = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(size, size + ImGui::GetTextLineHeightWithSpacing()));
        drawList = ImGui::GetWindowDrawList();
    } else {
        ImGui::SetNextWindowPos(at);
        ImGui::SetNextWindowBgAlpha(0.95f);
        if (!ImGui::Begin("##ColorLoupe", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                                                       ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings |
                                                       ImGuiWindowFlags_NoNav | ImGuiWindowFlags_Tooltip)) {
            ImGui::End();
            return;
        }
        min = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(size, size));
        drawList = ImGui::GetWindowDrawList();
    }
    if (loupeValid) {
        for (int y = 0; y < screencolor::kLoupeSize; y++) {
            for (int x = 0; x < screencolor::kLoupeSize; x++) {
                const uint8_t* c = loupe.data() + (y * 9 + x) * 3;
                const ImVec2 p(min.x + x * cell, min.y + y * cell);
                drawList->AddRectFilled(p, ImVec2(p.x + cell, p.y + cell), IM_COL32(c[0], c[1], c[2], 255));
            }
        }
        const ImVec2 centre(min.x + 4 * cell, min.y + 4 * cell);
        drawList->AddRect(centre, ImVec2(centre.x + cell, centre.y + cell), IM_COL32(255, 255, 255, 255), 0.0f, 0, 2.0f);
        drawList->AddRect(ImVec2(centre.x - 1, centre.y - 1), ImVec2(centre.x + cell + 1, centre.y + cell + 1), IM_COL32(0, 0, 0, 255));
    } else {
        drawList->AddRectFilled(min, ImVec2(min.x + size, min.y + size), IM_COL32(40, 40, 40, 255));
    }
    if (inWindow) {
        ImGui::TextDisabled("%s", loupeValid ? (loupeFromView ? "View texel (exact)" : "Screen") : loupeMessage.c_str());
    } else {
        if (!loupeValid) ImGui::TextUnformatted(loupeMessage.c_str());
        else ImGui::Text("#%s", colormath::toHex(current.shown(), false).c_str());
        ImGui::End();
    }
}

void ColorPickerWindow::beginEdit(CommandHistory* history, const char* label) {
    FieldEditSession::begin(history, std::string("Change ") + (label && *label ? label : "Color"), &get());
}
