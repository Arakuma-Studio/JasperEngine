// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The gradient sampling (midpointBlend, the OKLCH `vivid` blend, the stop walk) is ported from
// Pachislop src/model/gradient.ts (sampleGradientRgba) by SafEight, used with the author's
// permission, as engine/core/titleart/render/TitleLuts.cpp is: the two must stay identical so the
// row shows what the renderer draws (RampFieldTest compares them over the LUT). The editor
// behaviour follows Pachislop src/ui/GradientEditor.tsx (add by click, drag down to delete,
// midpoint diamonds, reverse with mirrored midpoints, Vivid).

#include "RampField.h"

#include "Theme.h"
#include "external/IconsFontAwesome6.h"
#include "widget/ColorField.h"
#include "widget/ColorPickerWindow.h"
#include "widget/GradientField.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <unordered_map>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    namespace ta = jasperengine::titleart;
    using ui::ramp::TitleGradient;
    using ui::ramp::TitleGradientStop;
    using Rgb = std::array<double, 3>;

    // ---- the renderer's blend (TitleLuts.cpp)

    double clamp01(double x) { return x < 0.0 ? 0.0 : x > 1.0 ? 1.0 : x; }

    // Photoshop's colour midpoint: the half-way colour sits at `mid` of the gap
    double midpointBlend(double x, double mid) {
        const double m = std::clamp(mid, 0.05, 0.95);
        if (std::abs(m - 0.5) < 1e-4 || x <= 0.0 || x >= 1.0) return x;
        return std::pow(x, std::log(0.5) / std::log(m));
    }

    double toLin(double c) { return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); }
    double toSrgb(double c) { return c <= 0.0031308 ? c * 12.92 : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055; }

    // sRGB 0..1 -> OKLCH (L, C, h radians)
    Rgb oklch(const Rgb& c) {
        const double R = toLin(c[0]), G = toLin(c[1]), B = toLin(c[2]);
        const double l = std::cbrt(0.4122214708 * R + 0.5363325363 * G + 0.0514459929 * B);
        const double m = std::cbrt(0.2119034982 * R + 0.6806995451 * G + 0.1073969566 * B);
        const double s = std::cbrt(0.0883024619 * R + 0.2817188376 * G + 0.6299787005 * B);
        const double L = 0.2104542553 * l + 0.793617785 * m - 0.0040720468 * s;
        const double A = 1.9779984951 * l - 2.428592205 * m + 0.4505937099 * s;
        const double Bb = 0.0259040371 * l + 0.7827717662 * m - 0.808675766 * s;
        return {L, std::hypot(A, Bb), std::atan2(Bb, A)};
    }

    Rgb labToLinear(double L, double A, double B) {
        const double l = std::pow(L + 0.3963377774 * A + 0.2158037573 * B, 3.0);
        const double m = std::pow(L - 0.1055613458 * A - 0.0638541728 * B, 3.0);
        const double s = std::pow(L - 0.0894841775 * A - 1.291485548 * B, 3.0);
        return {4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s, -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s,
                -0.0041960863 * l - 0.7034186147 * m + 1.707614701 * s};
    }

    // OKLCH -> sRGB, chroma reduced (hue and lightness kept) until it fits the gamut
    Rgb fromOklch(double L, double C, double h) {
        auto fits = [](const Rgb& c) { return std::all_of(c.begin(), c.end(), [](double v) { return v >= -1e-4 && v <= 1.0 + 1e-4; }); };
        Rgb lin = labToLinear(L, C * std::cos(h), C * std::sin(h));
        if (!fits(lin)) {
            double lo = 0.0, hi = C;
            for (int i = 0; i < 14; i++) {
                const double mid = (lo + hi) / 2.0;
                if (fits(labToLinear(L, mid * std::cos(h), mid * std::sin(h)))) lo = mid;
                else hi = mid;
            }
            lin = labToLinear(L, lo * std::cos(h), lo * std::sin(h));
        }
        return {clamp01(toSrgb(clamp01(lin[0]))), clamp01(toSrgb(clamp01(lin[1]))), clamp01(toSrgb(clamp01(lin[2])))};
    }

    // below this chroma a colour's hue is meaningless: it takes the other stop's
    constexpr double ACHROMATIC = 0.004;
    constexpr double PI = 3.14159265358979323846;

    Rgb mixVivid(const Rgb& a, const Rgb& b, double t) {
        const Rgb A = oklch(a), B = oklch(b);
        const double ha = A[1] < ACHROMATIC ? B[2] : A[2];
        const double hb = B[1] < ACHROMATIC ? A[2] : B[2];
        double dh = hb - ha;
        if (dh > PI) dh -= 2.0 * PI;
        else if (dh < -PI) dh += 2.0 * PI;
        return fromOklch(A[0] + (B[0] - A[0]) * t, A[1] + (B[1] - A[1]) * t, ha + dh * t);
    }

    Rgb rgbOf(const ta::TitleColor& c) { return {c.r, c.g, c.b}; }

    ui::ramp::Rgba sampleSorted(const TitleGradient& g, const std::vector<TitleGradientStop>& s, double pos) {
        if (s.empty()) return {};
        auto at = [](const TitleGradientStop& st) { return ui::ramp::Rgba{st.color.r, st.color.g, st.color.b, st.alpha}; };
        if (pos <= s.front().pos) return at(s.front());
        for (size_t i = 0; i + 1 < s.size(); i++) {
            const TitleGradientStop& a = s[i];
            const TitleGradientStop& b = s[i + 1];
            if (pos > b.pos) continue;
            const double span = b.pos - a.pos;
            const double x = midpointBlend(span > 0.0 ? (pos - a.pos) / span : 0.0, a.mid);
            const Rgb ca = rgbOf(a.color), cb = rgbOf(b.color);
            const bool sameColor = a.color.r == b.color.r && a.color.g == b.color.g && a.color.b == b.color.b;
            Rgb c;
            if (g.vivid && !sameColor) c = mixVivid(ca, cb, x);
            else c = {ca[0] + (cb[0] - ca[0]) * x, ca[1] + (cb[1] - ca[1]) * x, ca[2] + (cb[2] - ca[2]) * x};
            return {c[0], c[1], c[2], a.alpha + (b.alpha - a.alpha) * x};
        }
        return at(s.back());
    }

    bool byPosition(const TitleGradientStop& a, const TitleGradientStop& b) { return a.pos < b.pos; }

    bool sameStop(const TitleGradientStop& a, const TitleGradientStop& b) {
        return a.pos == b.pos && a.color.r == b.color.r && a.color.g == b.color.g && a.color.b == b.color.b &&
               a.alpha == b.alpha && a.mid == b.mid;
    }

    // ---- drawing

    ImU32 toImColor(const ui::ramp::Rgba& c) {
        return IM_COL32((int)std::lround(clamp01(c.r) * 255.0), (int)std::lround(clamp01(c.g) * 255.0),
                        (int)std::lround(clamp01(c.b) * 255.0), (int)std::lround(clamp01(c.a) * 255.0));
    }

    ImU32 toImColor(const ta::TitleColor& c) {
        return toImColor(ui::ramp::Rgba{c.r, c.g, c.b, 1.0});
    }

    // the bar's colours at `count` + 1 evenly spaced positions
    std::vector<ImU32> barColors(const TitleGradient& gradient, int count) {
        std::vector<ImU32> colors((size_t)count + 1);
        for (int i = 0; i <= count; i++) colors[(size_t)i] = toImColor(ui::ramp::sample(gradient, (double)i / count));
        return colors;
    }

    void drawBar(ImDrawList* drawList, ImVec2 min, ImVec2 max, const std::vector<ImU32>& colors, bool mixed) {
        // a checker under the colours for their alpha
        const float cell = std::max(3.0f, (max.y - min.y) * 0.5f);
        for (float y = min.y; y < max.y; y += cell) {
            for (float x = min.x; x < max.x; x += cell) {
                const bool dark = ((int)((x - min.x) / cell) + (int)((y - min.y) / cell)) % 2 == 0;
                drawList->AddRectFilled(ImVec2(x, y), ImVec2(std::min(x + cell, max.x), std::min(y + cell, max.y)),
                                        dark ? IM_COL32(128, 128, 128, 255) : IM_COL32(200, 200, 200, 255));
            }
        }
        const size_t segments = colors.size() > 1 ? colors.size() - 1 : 1;
        const float step = (max.x - min.x) / (float)segments;
        for (size_t i = 0; i + 1 < colors.size(); i++) {
            const float x0 = min.x + step * (float)i;
            const float x1 = i + 2 == colors.size() ? max.x : x0 + step;
            drawList->AddRectFilledMultiColor(ImVec2(x0, min.y), ImVec2(x1, max.y), colors[i], colors[i + 1], colors[i + 1], colors[i]);
        }
        if (mixed) {
            // several targets differ: diagonal stripes over the first one's gradient
            drawList->PushClipRect(min, max, true);
            const float height = max.y - min.y;
            const float spacing = Theme::dpi(6.0f);
            for (float x = min.x - height; x < max.x; x += spacing) {
                drawList->AddLine(ImVec2(x, max.y), ImVec2(x + height, min.y), IM_COL32(255, 255, 255, 110), Theme::dpi(2.0f));
            }
            drawList->PopClipRect();
        }
        drawList->AddRect(min, max, IM_COL32(0, 0, 0, 200));
    }

    // ---- the field's state between frames

    struct FieldState {
        ui::RampEditor editor;
        bool focused = false;              // the last click was on this field: Delete removes its stop
        bool alphaChanged = false;         // the alpha field changed during its current edit
        bool positionChanged = false;
        TitleGradient cached;              // the gradient `colors` shows
        std::vector<ImU32> colors;
        int lastFrame = 0;
    };

    std::unordered_map<ImGuiID, FieldState>& fieldStates() {
        static std::unordered_map<ImGuiID, FieldState> states;
        return states;
    }

    FieldState& stateFor(ImGuiID id) {
        std::unordered_map<ImGuiID, FieldState>& states = fieldStates();
        const int frame = ImGui::GetFrameCount();
        // fields that have not been drawn for a while (a closed page, a search result gone)
        if (states.size() > 64) {
            for (auto it = states.begin(); it != states.end();) {
                if (it->first != id && frame - it->second.lastFrame > 120 && !it->second.editor.dragging()) it = states.erase(it);
                else ++it;
            }
        }
        FieldState& state = states[id];
        state.lastFrame = frame;
        return state;
    }

    // An ImGui item's edit as FieldEdit phases: Begin on activation, Changing per change, then
    // Committed (or Cancelled when nothing changed) on deactivation
    template <class Edit>
    void itemPhases(bool changed, bool& changedDuringEdit, Edit&& edit) {
        if (ImGui::IsItemActivated()) {
            changedDuringEdit = false;
            edit(ui::EditPhase::Begin);
        }
        if (changed) {
            changedDuringEdit = true;
            edit(ui::EditPhase::Changing);
        }
        if (ImGui::IsItemDeactivated()) {
            edit(changedDuringEdit ? ui::EditPhase::Committed : ui::EditPhase::Cancelled);
            changedDuringEdit = false;
        }
    }

}

// ---- ramp: the pure helpers

ui::ramp::Rgba ui::ramp::sample(const TitleGradient& gradient, double pos) {
    if (std::is_sorted(gradient.stops.begin(), gradient.stops.end(), byPosition)) return sampleSorted(gradient, gradient.stops, pos);
    std::vector<TitleGradientStop> stops = gradient.stops;
    std::stable_sort(stops.begin(), stops.end(), byPosition);
    return sampleSorted(gradient, stops, pos);
}

bool ui::ramp::sameGradient(const TitleGradient& a, const TitleGradient& b) {
    if (a.vivid != b.vivid || a.stops.size() != b.stops.size()) return false;
    for (size_t i = 0; i < a.stops.size(); i++) {
        if (!sameStop(a.stops[i], b.stops[i])) return false;
    }
    return true;
}

void ui::ramp::sortStops(TitleGradient& gradient) {
    std::stable_sort(gradient.stops.begin(), gradient.stops.end(), byPosition);
}

int ui::ramp::insertStop(TitleGradient& gradient, double pos, std::string* error) {
    if (gradient.stops.size() >= MAX_STOPS) {
        if (error) *error = TOO_MANY_STOPS;
        return -1;
    }
    pos = clamp01(pos);
    const Rgba color = sample(gradient, pos);
    TitleGradientStop stop;
    stop.pos = pos;
    stop.color = {(float)color.r, (float)color.g, (float)color.b};
    stop.alpha = color.a;
    stop.mid = 0.5;
    sortStops(gradient);
    const auto at = std::upper_bound(gradient.stops.begin(), gradient.stops.end(), stop, byPosition);
    // the index before inserting: insert() may reallocate, and `begin()` would be read before or after it
    const int index = (int)(at - gradient.stops.begin());
    gradient.stops.insert(at, stop);
    return index;
}

bool ui::ramp::removeStop(TitleGradient& gradient, size_t index) {
    if (index >= gradient.stops.size() || gradient.stops.size() <= MIN_STOPS) return false;
    gradient.stops.erase(gradient.stops.begin() + (std::ptrdiff_t)index);
    return true;
}

int ui::ramp::moveStop(TitleGradient& gradient, size_t index, double pos) {
    if (index >= gradient.stops.size()) return -1;
    TitleGradientStop stop = gradient.stops[index];
    stop.pos = clamp01(pos);
    gradient.stops.erase(gradient.stops.begin() + (std::ptrdiff_t)index);
    sortStops(gradient);
    const auto at = std::upper_bound(gradient.stops.begin(), gradient.stops.end(), stop, byPosition);
    // the index before inserting: insert() may reallocate, and `begin()` would be read before or after it
    const int placed = (int)(at - gradient.stops.begin());
    gradient.stops.insert(at, stop);
    return placed;
}

void ui::ramp::setMidpoint(TitleGradient& gradient, size_t left, double mid) {
    if (left >= gradient.stops.size()) return;
    gradient.stops[left].mid = std::round(std::clamp(mid, MID_MIN, MID_MAX) * 1000.0) / 1000.0;
}

void ui::ramp::reverse(TitleGradient& gradient) {
    sortStops(gradient);
    const std::vector<TitleGradientStop> stops = gradient.stops;
    const size_t n = stops.size();
    for (size_t i = 0; i < n; i++) {
        TitleGradientStop stop = stops[n - 1 - i];
        stop.pos = 1.0 - stop.pos;
        // the gap after the new stop i is the old gap before stop n - 1 - i, mirrored (on the
        // midpoint drag's 0.001 grid, so reversing twice gives the same numbers back)
        stop.mid = i + 1 < n ? std::round(std::clamp(1.0 - stops[n - 2 - i].mid, MID_MIN, MID_MAX) * 1000.0) / 1000.0 : 0.5;
        gradient.stops[i] = stop;
    }
}

TitleGradient ui::ramp::fromPreset(const Gradient& preset) {
    std::vector<float> times;
    for (const GradientColorKey& key : preset.colorKeys) times.push_back(std::clamp(key.time, 0.0f, 1.0f));
    for (const GradientAlphaKey& key : preset.alphaKeys) times.push_back(std::clamp(key.time, 0.0f, 1.0f));
    std::sort(times.begin(), times.end());
    times.erase(std::unique(times.begin(), times.end()), times.end());
    // a stop at each end at least: a single key holds over the whole bar
    if (times.empty() || times.front() > 0.0f) times.insert(times.begin(), 0.0f);
    if (times.back() < 1.0f) times.push_back(1.0f);
    if (times.size() > MAX_STOPS) times.resize(MAX_STOPS);

    TitleGradient gradient;
    for (float time : times) {
        // the colour as the preset blends it (sRGB, the file's form): evaluate()'s linear value
        // went through the engine's approximate sRGB curve and would not come back exactly
        const Vector3 encoded = preset.evaluateEncoded(time);
        TitleGradientStop stop;
        stop.pos = time;
        stop.color = {std::clamp(encoded.x, 0.0f, 1.0f), std::clamp(encoded.y, 0.0f, 1.0f), std::clamp(encoded.z, 0.0f, 1.0f)};
        stop.alpha = std::clamp((double)preset.evaluateAlpha(time), 0.0, 1.0);
        gradient.stops.push_back(stop);
    }
    return gradient;
}

jasperengine::titleart::Json ui::ramp::toJson(const TitleGradient& gradient) {
    ta::Json stops = ta::Json::array();
    for (const TitleGradientStop& stop : gradient.stops) {
        stops.push_back(ta::Json{{"pos", stop.pos}, {"color", ta::formatTitleColor(stop.color)}, {"alpha", stop.alpha}, {"mid", stop.mid}});
    }
    return ta::Json{{"stops", std::move(stops)}, {"interp", gradient.vivid ? "vivid" : "rgb"}};
}

float ui::ramp::xAt(float x0, float width, double pos) {
    return x0 + (float)clamp01(pos) * width;
}

double ui::ramp::posAt(float x0, float width, float x) {
    return width > 0.0f ? clamp01((double)(x - x0) / width) : 0.0;
}

int ui::ramp::hitStop(const TitleGradient& gradient, float x0, float width, float x, float radius) {
    int best = -1;
    float bestDistance = radius;
    for (size_t i = 0; i < gradient.stops.size(); i++) {
        const float distance = std::abs(xAt(x0, width, gradient.stops[i].pos) - x);
        if (distance <= bestDistance) {
            bestDistance = distance;
            best = (int)i;
        }
    }
    return best;
}

double ui::ramp::midpointPos(const TitleGradient& gradient, size_t left) {
    if (left + 1 >= gradient.stops.size()) return left < gradient.stops.size() ? gradient.stops[left].pos : 0.0;
    const TitleGradientStop& a = gradient.stops[left];
    const TitleGradientStop& b = gradient.stops[left + 1];
    return a.pos + (b.pos - a.pos) * a.mid;
}

int ui::ramp::hitMidpoint(const TitleGradient& gradient, float x0, float width, float x, float radius) {
    int best = -1;
    float bestDistance = radius;
    for (size_t i = 0; i + 1 < gradient.stops.size(); i++) {
        if (gradient.stops[i + 1].pos - gradient.stops[i].pos < MIN_MIDPOINT_GAP) continue;
        const float distance = std::abs(xAt(x0, width, midpointPos(gradient, i)) - x);
        if (distance <= bestDistance) {
            bestDistance = distance;
            best = (int)i;
        }
    }
    return best;
}

// ---- RampEditor

void ui::RampEditor::sync(const ramp::TitleGradient& value) {
    if (drag != Drag::None || !queue.empty()) return;
    working = value;
    ramp::sortStops(working);
    clampSelection();
}

void ui::RampEditor::select(int index) {
    selection = index;
    clampSelection();
}

void ui::RampEditor::clampSelection() {
    selection = working.stops.empty() ? -1 : std::clamp(selection, 0, (int)working.stops.size() - 1);
}

void ui::RampEditor::push(EditPhase phase) {
    if (phase == EditPhase::Changing || phase == EditPhase::Applied) message.clear();
    queue.push_back({phase, working});
}

void ui::RampEditor::apply(const ramp::TitleGradient& value) {
    working = value;
    clampSelection();
    push(EditPhase::Applied);
}

void ui::RampEditor::pressBar(const Geometry& geometry, float x) {
    if (drag != Drag::None) return;
    ramp::TitleGradient next = working;
    std::string error;
    const int index = ramp::insertStop(next, ramp::posAt(geometry.x0, geometry.width, x), &error);
    if (index < 0) {
        message = error;
        return;
    }
    selection = index;
    apply(next);
}

void ui::RampEditor::pressStop(const Geometry& geometry, int index, float x, float y) {
    if (drag != Drag::None || index < 0 || index >= (int)working.stops.size()) return;
    selection = index;
    drag = Drag::Stop;
    dragIndex = index;
    before = working;
    startX = x;
    startY = y;
    grabOffset = x - ramp::xAt(geometry.x0, geometry.width, working.stops[(size_t)index].pos);
    begun = false;
    removed = false;
}

void ui::RampEditor::pressMidpoint(int left, float x) {
    if (drag != Drag::None || left < 0 || left + 1 >= (int)working.stops.size()) return;
    drag = Drag::Midpoint;
    dragIndex = left;
    before = working;
    startX = x;
    startY = 0.0f;
    begun = false;
    removed = false;
}

void ui::RampEditor::doubleClickMidpoint(int left) {
    if (drag != Drag::None || left < 0 || left + 1 >= (int)working.stops.size()) return;
    if (working.stops[(size_t)left].mid == 0.5) return;
    ramp::TitleGradient next = working;
    next.stops[(size_t)left].mid = 0.5;
    apply(next);
}

void ui::RampEditor::moveTo(const Geometry& geometry, float x, float y) {
    if (drag == Drag::None) return;
    if (!begun) {
        // a click selects; the drag starts past the threshold, its Begin carrying no change
        if (std::abs(x - startX) < geometry.dragThreshold && std::abs(y - startY) < geometry.dragThreshold) return;
        begun = true;
        push(EditPhase::Begin);
    }
    ramp::TitleGradient next = before;
    bool refused = false;
    if (drag == Drag::Stop) {
        // recomputed from the press every move: dragging back up brings the stop back
        removed = false;
        if (y - startY > geometry.removeDistance) {
            if (ramp::removeStop(next, (size_t)dragIndex)) {
                removed = true;
                selection = std::max(0, dragIndex - 1);
            } else {
                refused = true;
            }
        }
        if (!removed) selection = ramp::moveStop(next, (size_t)dragIndex, ramp::posAt(geometry.x0, geometry.width, x - grabOffset));
    } else {
        const TitleGradientStop& a = before.stops[(size_t)dragIndex];
        const TitleGradientStop& b = before.stops[(size_t)dragIndex + 1];
        const double fraction = (ramp::posAt(geometry.x0, geometry.width, x) - a.pos) / std::max(b.pos - a.pos, 1e-6);
        ramp::setMidpoint(next, (size_t)dragIndex, fraction);
    }
    if (!ramp::sameGradient(next, working)) {
        working = next;
        push(EditPhase::Changing);
    }
    if (refused) message = ramp::TOO_FEW_STOPS;
}

void ui::RampEditor::release() {
    if (drag == Drag::None) return;
    const bool wasBegun = begun;
    drag = Drag::None;
    begun = false;
    removed = false;
    if (!wasBegun) return;     // a click: the selection only
    push(ramp::sameGradient(working, before) ? EditPhase::Cancelled : EditPhase::Committed);
}

void ui::RampEditor::cancel() {
    if (drag == Drag::None) return;
    const bool wasBegun = begun;
    working = before;
    if (drag == Drag::Stop) selection = dragIndex;
    clampSelection();
    drag = Drag::None;
    begun = false;
    removed = false;
    if (wasBegun) push(EditPhase::Cancelled);
}

void ui::RampEditor::deleteSelected() {
    if (drag != Drag::None || selection < 0) return;
    ramp::TitleGradient next = working;
    if (!ramp::removeStop(next, (size_t)selection)) {
        message = ramp::TOO_FEW_STOPS;
        return;
    }
    selection = std::max(0, selection - 1);
    apply(next);
}

void ui::RampEditor::reverse() {
    if (drag != Drag::None || working.stops.empty()) return;
    ramp::TitleGradient next = working;
    ramp::reverse(next);
    if (selection >= 0) selection = (int)next.stops.size() - 1 - selection;
    apply(next);
}

void ui::RampEditor::setVivid(bool vivid) {
    if (drag != Drag::None || working.vivid == vivid) return;
    ramp::TitleGradient next = working;
    next.vivid = vivid;
    apply(next);
}

void ui::RampEditor::applyPreset(const ramp::TitleGradient& preset) {
    if (drag != Drag::None || preset.stops.size() < ramp::MIN_STOPS || preset.stops.size() > ramp::MAX_STOPS) return;
    ramp::TitleGradient next = preset;
    ramp::sortStops(next);
    selection = 0;
    apply(next);
}

void ui::RampEditor::editSelectedColor(EditPhase phase, const jasperengine::titleart::TitleColor& color) {
    if (selection < 0 || drag != Drag::None) return;
    // Cancelled carries the original colour back (the Color window's Esc)
    if (phase == EditPhase::Changing || phase == EditPhase::Applied || phase == EditPhase::Cancelled) {
        working.stops[(size_t)selection].color = color;
    }
    if (phase != EditPhase::None) push(phase);
}

void ui::RampEditor::editSelectedAlpha(EditPhase phase, double alpha) {
    if (selection < 0 || drag != Drag::None) return;
    if (phase == EditPhase::Changing || phase == EditPhase::Applied || phase == EditPhase::Cancelled) {
        working.stops[(size_t)selection].alpha = clamp01(alpha);
    }
    if (phase != EditPhase::None) push(phase);
}

void ui::RampEditor::editSelectedPosition(EditPhase phase, double pos) {
    if (selection < 0 || drag != Drag::None) return;
    if (phase == EditPhase::Changing || phase == EditPhase::Applied || phase == EditPhase::Cancelled) {
        selection = ramp::moveStop(working, (size_t)selection, pos);   // the stop stays selected
    }
    if (phase != EditPhase::None) push(phase);
}

bool ui::RampEditor::next(Step& out) {
    if (queue.empty()) return false;
    out = std::move(queue.front());
    queue.pop_front();
    return true;
}

// ---- the field

void ui::drawRampBar(ImDrawList* drawList, ImVec2 min, ImVec2 max, const jasperengine::titleart::TitleGradient& gradient, bool mixed) {
    const int segments = std::clamp((int)((max.x - min.x) / 3.0f), 8, 96);
    drawBar(drawList, min, max, barColors(gradient, segments), mixed);
}

ui::EditPhase ui::rampField(const char* id, jasperengine::titleart::TitleGradient& gradient, const RampFieldOptions& options) {
    ImGui::PushID(id);
    FieldState& state = stateFor(ImGui::GetID("##rampState"));
    RampEditor& editor = state.editor;
    editor.sync(gradient);

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const float frame = ImGui::GetFrameHeight();
    const float inset = Theme::dpi(6.0f);            // room for the end stops' markers
    const float diamondBand = Theme::dpi(9.0f);
    const float barHeight = options.height > 0.0f ? options.height : frame;
    const float stopBand = Theme::dpi(13.0f);
    const float width = std::max(ImGui::GetContentRegionAvail().x, frame * 4.0f);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 barMin(origin.x + inset, origin.y + diamondBand);
    const ImVec2 barMax(origin.x + width - inset, barMin.y + barHeight);
    RampEditor::Geometry geometry;
    geometry.x0 = barMin.x;
    geometry.width = std::max(1.0f, barMax.x - barMin.x);
    geometry.removeDistance = Theme::dpi(ramp::REMOVE_DRAG_PIXELS);
    geometry.dragThreshold = Theme::dpi(2.0f);
    const float stopRadius = Theme::dpi(6.0f);
    const float midRadius = Theme::dpi(5.0f);

    ImGui::BeginGroup();
    ImGui::InvisibleButton("##ramp", ImVec2(width, diamondBand + barHeight + stopBand));
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    if (ImGui::IsItemActivated()) {
        state.focused = true;
        const ramp::TitleGradient& value = editor.value();
        if (mouse.y < barMin.y) {
            const int left = ramp::hitMidpoint(value, geometry.x0, geometry.width, mouse.x, midRadius);
            if (left >= 0 && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) editor.doubleClickMidpoint(left);
            else if (left >= 0) editor.pressMidpoint(left, mouse.x);
        } else if (mouse.y <= barMax.y) {
            editor.pressBar(geometry, mouse.x);
        } else {
            const int stop = ramp::hitStop(value, geometry.x0, geometry.width, mouse.x, stopRadius);
            if (stop >= 0) editor.pressStop(geometry, stop, mouse.x, mouse.y);
        }
    } else if (ImGui::IsItemActive() && editor.dragging()) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) editor.cancel();
        else editor.moveTo(geometry, mouse.x, mouse.y);
    }
    if (ImGui::IsItemDeactivated()) editor.release();

    const ramp::TitleGradient& shown = editor.value();
    if (hovered && !ImGui::IsItemActive()) {
        if (mouse.y < barMin.y) {
            const int left = ramp::hitMidpoint(shown, geometry.x0, geometry.width, mouse.x, midRadius);
            if (left >= 0) {
                ImGui::SetTooltip("Midpoint %d%%: drag to bias the blend toward one stop, double-click to reset",
                                  (int)std::lround(shown.stops[(size_t)left].mid * 100.0));
            }
        } else if (mouse.y <= barMax.y) {
            ImGui::SetTooltip("%s", options.mixed ? "The selected gradients differ: an edit gives all of them this one" : "Click to add a stop");
        } else if (ramp::hitStop(shown, geometry.x0, geometry.width, mouse.x, stopRadius) >= 0) {
            ImGui::SetTooltip("Drag to move, drag down to delete");
        }
    }
    if (editor.removing()) ImGui::SetTooltip("Release to delete the stop");

    // the bar (its colours cached until the gradient changes)
    if (state.colors.empty() || !ramp::sameGradient(shown, state.cached)) {
        state.cached = shown;
        state.colors = barColors(shown, std::clamp((int)(geometry.width / 3.0f), 8, 96));
    }
    drawBar(drawList, barMin, barMax, state.colors, options.mixed);

    // midpoint diamonds above the bar
    const float diamondY = origin.y + diamondBand * 0.5f;
    const float diamond = Theme::dpi(3.5f);
    for (size_t i = 0; i + 1 < shown.stops.size(); i++) {
        if (shown.stops[i + 1].pos - shown.stops[i].pos < ramp::MIN_MIDPOINT_GAP) continue;
        const float x = ramp::xAt(geometry.x0, geometry.width, ramp::midpointPos(shown, i));
        const bool moved = std::abs(shown.stops[i].mid - 0.5) > 1e-3;
        const ImVec2 top(x, diamondY - diamond), right(x + diamond, diamondY), bottom(x, diamondY + diamond), left(x - diamond, diamondY);
        drawList->AddQuadFilled(top, right, bottom, left, moved ? ImGui::GetColorU32(ImGuiCol_CheckMark) : IM_COL32(220, 220, 220, 255));
        drawList->AddQuad(top, right, bottom, left, IM_COL32(0, 0, 0, 255));
    }

    // stop markers under the bar: a point on the bar over a swatch of the stop's colour
    const float marker = Theme::dpi(5.0f);
    for (size_t i = 0; i < shown.stops.size(); i++) {
        const TitleGradientStop& stop = shown.stops[i];
        const float x = ramp::xAt(geometry.x0, geometry.width, stop.pos);
        const float top = barMax.y + Theme::dpi(1.0f);
        const bool selected = (int)i == editor.selected();
        const ImVec2 outline[5] = {ImVec2(x, top), ImVec2(x + marker, top + marker), ImVec2(x + marker, top + marker * 2.2f),
                                   ImVec2(x - marker, top + marker * 2.2f), ImVec2(x - marker, top + marker)};
        drawList->AddConvexPolyFilled(outline, 5, toImColor(stop.color));
        drawList->AddPolyline(outline, 5, selected ? IM_COL32(255, 255, 255, 255) : IM_COL32(0, 0, 0, 255), ImDrawFlags_Closed,
                              selected ? Theme::dpi(2.0f) : 1.0f);
    }

    // the selected stop's colour, alpha and location
    const int selection = editor.selected();
    bool sameLine = false;
    if (selection >= 0 && selection < (int)shown.stops.size()) {
        const TitleGradientStop stop = shown.stops[(size_t)selection];
        ImGui::PushID(selection);   // the Color window stays with the stop it opened on
        const std::string colorLabel = std::string(options.label ? options.label : "Gradient") + " Stop";
        ColorFieldOptions colorOptions;
        colorOptions.alpha = false;
        colorOptions.linearStorage = false;     // the stops are sRGB as shown
        colorOptions.label = colorLabel.c_str();
        colorOptions.mixed = options.mixed;
        colorOptions.width = frame * 2.0f;
        Vector3 rgb(stop.color.r, stop.color.g, stop.color.b);
        const EditPhase colorPhase = colorField("##stopColor", rgb, colorOptions);
        if (colorPhase != EditPhase::None) editor.editSelectedColor(colorPhase, {rgb.x, rgb.y, rgb.z});

        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Alpha");
        ImGui::SameLine();
        float alpha = (float)(stop.alpha * 100.0);
        ImGui::SetNextItemWidth(Theme::dpi(56.0f));
        const bool alphaChanged = ImGui::DragFloat("##alpha", &alpha, 0.5f, 0.0f, 100.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp);
        itemPhases(alphaChanged, state.alphaChanged, [&](EditPhase phase) { editor.editSelectedAlpha(phase, alpha / 100.0); });
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Stop opacity");

        ImGui::SameLine();
        ImGui::TextUnformatted("Location");
        ImGui::SameLine();
        float location = (float)(stop.pos * 100.0);
        ImGui::SetNextItemWidth(Theme::dpi(56.0f));
        const bool locationChanged = ImGui::DragFloat("##location", &location, 0.25f, 0.0f, 100.0f, "%.1f%%", ImGuiSliderFlags_AlwaysClamp);
        itemPhases(locationChanged, state.positionChanged, [&](EditPhase phase) { editor.editSelectedPosition(phase, location / 100.0); });
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Stop location");

        ImGui::SameLine();
        ImGui::BeginDisabled(shown.stops.size() <= ramp::MIN_STOPS);
        if (ImGui::Button(ICON_FA_TRASH_CAN "##deleteStop")) editor.deleteSelected();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Delete stop (Delete)");
        ImGui::EndDisabled();
        ImGui::PopID();
        sameLine = true;
    }

    if (sameLine) ImGui::SameLine();
    if (ImGui::Button(ICON_FA_RIGHT_LEFT "##reverse")) editor.reverse();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Reverse");
    ImGui::SameLine();
    bool vivid = shown.vivid;
    if (ImGui::Checkbox("Vivid", &vivid)) editor.setVivid(vivid);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Vivid: blend through OKLCH along the shorter hue arc, so blue to white stays blue instead of greying out in the middle");
    }
    if (options.presets) {
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_SWATCHBOOK "##presets")) ImGui::OpenPopup("##rampPresets");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Presets (the swatch libraries' gradients)");
        if (ImGui::BeginPopup("##rampPresets")) {
            const std::vector<SwatchLibrary>& libraries = options.libraries ? *options.libraries : ColorPickerWindow::get().libraries();
            bool any = false;
            for (size_t l = 0; l < libraries.size(); l++) {
                for (size_t i = 0; i < libraries[l].gradients.size(); i++) {
                    const GradientPreset& preset = libraries[l].gradients[i];
                    ImGui::PushID((int)(l * 1000 + i));
                    const ImVec2 min = ImGui::GetCursorScreenPos();
                    const ImVec2 size(Theme::dpi(96.0f), frame);
                    if (ImGui::InvisibleButton("##preset", size)) {
                        editor.applyPreset(ramp::fromPreset(preset.gradient));
                        ImGui::CloseCurrentPopup();
                    }
                    drawGradientBar(ImGui::GetWindowDrawList(), min, ImVec2(min.x + size.x, min.y + size.y), preset.gradient);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s (%s)", preset.name.c_str(), libraries[l].name.c_str());
                    ImGui::PopID();
                    any = true;
                }
            }
            if (!any) ImGui::TextDisabled("No gradient presets");
            ImGui::EndPopup();
        }
    }
    if (!editor.warning().empty()) ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "%s", editor.warning().c_str());
    ImGui::EndGroup();

    // Delete removes the selected stop while this field holds the last click
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsItemHovered()) state.focused = false;
    if (state.focused && ImGui::IsWindowFocused() && !ImGui::GetIO().WantTextInput &&
        (ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false))) {
        editor.deleteSelected();
    }

    EditPhase phase = EditPhase::None;
    RampEditor::Step step;
    if (editor.next(step)) {
        phase = step.phase;
        gradient = std::move(step.value);
    }
    ImGui::PopID();
    return phase;
}
