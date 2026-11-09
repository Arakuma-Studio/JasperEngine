// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// A Lospec palette by its slug (docs/specs/S43-palettes.md R8): the editor only, never at
// runtime. The request goes through the editor's HTTP client (curl, or the recorded responses of
// JASPERENGINE_IMPORT_MIRROR), so it blocks: call fetchLospec from a worker. The caller checks
// the editor's offline setting before calling.

#include "pixelart/Palette.h"
#include "pixelart/PaletteFormats.h"

#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace jasperengine::editor::htmlui {
    class HttpClient;
}

namespace jasperengine::editor::pixelart {

    constexpr int kLospecTimeoutSeconds = 10;

    // The slug of a slug ("endesga-32"), a palette page URL ("https://lospec.com/palette-list/
    // endesga-32", with or without a trailing '/' or ".json") or a "lospec-palette://endesga-32"
    // link, lower-cased; nullopt unless it matches [a-z0-9-]{1,64}
    std::optional<std::string> normaliseSlug(std::string_view input);

    // https://lospec.com/palette-list/<slug> and the JSON the editor requests, <that>.json
    std::string lospecPageUrl(const std::string& slug);
    std::string lospecJsonUrl(const std::string& slug);

    // The palette of a Lospec answer ({name, author, colors: ["rrggbb", ...]}) with source {kind:
    // lospec, id, url, fetched: todayIso}. HTTP 404, or an {"error": ...} body, is
    // lospec_not_found; anything else unexpected is bad_response.
    std::variant<jasperengine::pixelart::Palette, PaletteError> parseLospecResponse(const std::string& slug, int status, const std::string& body,
                                                                                    const std::string& todayIso);

    // Requests lospecJsonUrl(slug) with a 10 s timeout. No curl, a DNS or connection failure, a
    // timeout or a cancel is network_unavailable.
    std::variant<jasperengine::pixelart::Palette, PaletteError> fetchLospec(const std::string& slug, htmlui::HttpClient& client,
                                                                            const std::string& todayIso);

    // today in UTC, YYYY-MM-DD
    std::string todayIsoDate();

}
