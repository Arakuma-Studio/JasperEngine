// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The module's own TitleContentProvider (TitleContent.h): text layout and geometry
// (geometry::TitleGeometry over TitleTextLayout) behind the renderer's 2D title passes.

#include "TitleContent.h"

#include "titleart/geometry/Shapes.h"
#include "titleart/geometry/TitleGeometry.h"

#include <memory>

using namespace jasperengine;
using namespace jasperengine::titleart;

namespace {

    class GeometryContent : public TitleContentProvider {
    public:
        geometry::LayerElements elements(const Json& layer, const TitleArtDoc& doc, const AssetResolver* assets,
                                         const TitleElementFilter& filter) override {
            return geometryFor(assets).elements(layer, doc, filter.cluster);
        }

        geometry::CoverageMask coverage(const geometry::TitleElement& element, const geometry::MaskFrame& frame) override {
            // the renderer shifts the frame by the element's offset; rasterizeElement would apply it again
            geometry::TitleElement unshifted = element;
            unshifted.offset = {};
            return geometry::rasterizeElement(unshifted, frame);
        }

        bool picture(const Json& layer, const TitleArtDoc& doc, const AssetResolver* assets, const geometry::MaskFrame& frame,
                     TitleImageData& out) override {
            const LayerType type = layerTypeOf(layer);
            if (type != LayerType::Image && type != LayerType::Vector) return false;
            const geometry::LayerElements els = geometryFor(assets).elements(layer, doc);
            for (const geometry::TitleElement& e : els.elements) {
                if (e.kind != geometry::ElementKind::Face || e.source.empty()) continue;
                out = geometry::rasterizeSource(e.source, frame);
                return out.width == frame.width && out.height == frame.height;
            }
            return false;
        }

        bool spans(const Json& layer, const TitleArtDoc& doc, const AssetResolver* assets, const TitleElementFilter& filter,
                   TitleSpans& out) override {
            if (layerTypeOf(layer) != LayerType::Text) return false;
            const geometry::LayerElements els = geometryFor(assets).elements(layer, doc, filter.cluster);
            out.chars.clear();
            out.lines.clear();
            for (const geometry::GlyphSpan& span : els.spans) {
                out.chars.push_back(span.charBox);
                out.lines.push_back(span.lineBox);
            }
            return !out.chars.empty();
        }

        geometry::Path shapeOutline(const Json& shape) override {
            return geometry::shapePath(geometry::decodeShape(shape));
        }

    private:
        // TitleGeometry reads its fonts, images and SVGs through one resolver: a request with
        // another resolver (another project, a transient preview) starts a geometry of its own.
        // Its caches are keyed by the resolved files' paths and revisions, so a resolver made
        // again at the same address never serves another project's outlines
        geometry::TitleGeometry& geometryFor(const AssetResolver* assets) {
            if (!geometry || assets != resolver) {
                geometry = std::make_unique<geometry::TitleGeometry>(assets);
                resolver = assets;
            }
            return *geometry;
        }

        std::unique_ptr<geometry::TitleGeometry> geometry;
        const AssetResolver* resolver = nullptr;
    };

}

std::shared_ptr<TitleContentProvider> titleart::makeGeometryTitleContentProvider() {
    return std::make_shared<GeometryContent>();
}
