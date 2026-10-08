// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef COMPAT_RUNTIME_SVGELEMENTHOOK_H
#define COMPAT_RUNTIME_SVGELEMENTHOOK_H

#include "Export.h"
#include "htmlui/compat/runtime/SvgPreprocess.h"

#include <RmlUi/SVG/ElementSVG.h>

#include <cstdint>
#include <memory>
#include <string>

namespace Rml {
    class Element;
}

namespace jasperengine::htmlui::compat {

    // Phase-2 hook H8: compat's side of the SVG plugin's source hook (the fork patch
    // `rmlui: SVG source hook`). Every <svg> (inline or src) goes through SvgPreprocess with
    // the element's SVG paint properties as root overrides; then
    // - Color: the white rasterization is multiplied by the CSS color (x image-color x opacity),
    //   so a color change or transition only makes new geometry, never a new raster;
    // - Substitute: the CSS color is written into the SVG, which is rasterized again when it
    //   changes (HTMLUI-SVG-0001);
    // - None: drawn as is.
    // Results are cached by source content and overrides. registerRuntime installs it.
    class JASPERENGINE_INTERNAL SvgElementHook : public Rml::SVG::SourceHook {
    public:
        bool ProcessSource(Rml::Element* element, const Rml::String& path, const Rml::String& source, Result& result) override;
        bool IsSourceProperty(const Rml::PropertyIdSet& changed_properties) override;

        static SvgElementHook& instance();
        static void install();     // Rml::SVG::SetSourceHook(&instance())
        static void uninstall();   // and clears the cache

        // the paint overrides an element's computed SVG paint properties give
        static SvgPaintOverrides overridesOf(Rml::Element& element);
        // the preprocessed SVG for a source (cached by content and overrides)
        static std::shared_ptr<const SvgResult> preprocess(const std::string& source, SvgUse use, const SvgPaintOverrides& overrides);

        // tests
        static size_t cacheSize();
        static size_t processCount();   // SvgPreprocess::process calls (cache misses)
        static void clearCache();
    };

}

#endif //COMPAT_RUNTIME_SVGELEMENTHOOK_H
