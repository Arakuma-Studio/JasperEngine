// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S87-title-localisation-interchange.md R34, D12, §6.2 import_svg, create.from)
// - Purpose: SVG files into Vector layers. `split: none` makes one layer; `groups` one per
//   top-level <g> or drawable element of the root; `paths` one per drawable element at any depth.
//   Each split part is its own small .svg (D12): the original root element with every attribute
//   (viewBox, width, height, namespaces, presentation), the root's definitions (<defs>, <style>,
//   gradients, patterns, symbols...), the elements outside them that the part references
//   (`href="#id"`, `url(#id)`) in a <defs>, then the element inside shallow copies of its ancestor
//   groups (their transforms and inherited presentation attributes, ids dropped). Every part keeps
//   the root's viewport, so the layers share size and position and register exactly with no new
//   document member.
// - `mode: auto` picks `shape` when every fill and stroke of the part is one colour (or none, or
//   black), else `artwork`. A shape-mode part lists what shape mode cannot use as notes (<text>,
//   <image>, clipPath, mask, filter).
// - Files: a source outside the project's assets is copied to `<doc stem>_assets/`; split parts
//   go beside the (copied) original, a file with the same content already there is reused. They
//   are returned (TitleOpsResult::files / TitleCreateResult::files) and written with the
//   document in its one undo step.
// - Thread-safety: main thread (the op registration); the split functions are pure.

#pragma once

#include "titleart/TitleArtOps.h"

#include <string>
#include <vector>

namespace jasperengine::editor {

    struct SvgPart {
        std::string name;                   // the element's id, else "Group 2", "Path 3" ...
        std::string text;                   // the part's own SVG document
        std::string mode;                   // shape | artwork (auto's choice for this part)
        std::vector<std::string> notes;     // what shape mode cannot use, and per-part approximations
    };

    struct SvgSplit {
        bool ok = false;
        std::string error;
        double width = 0.0, height = 0.0;   // the root's viewport (viewBox, else width / height)
        std::vector<SvgPart> parts;         // in paint order (bottom first)
    };

    // split: "none" | "groups" | "paths"; mode: "auto" | "shape" | "artwork"
    SvgSplit splitSvg(const std::string& text, const std::string& split, const std::string& mode = "auto");
    // auto's rule over a whole SVG text: "shape" or "artwork"
    std::string svgAutoMode(const std::string& text);

    // the op `import_svg` inside a batch; `create.from` runs it on a new document
    bool importSvgOp(TitleOpContext& ctx, const TitleJson& op);

    void registerSvgInterchange();

}
