// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef SokolPipelineRecord_h
#define SokolPipelineRecord_h

#include "sokol_gfx.h"

#include <string>

namespace jasperengine {

    // A pipeline description as the shader warm-up list stores it (spec S32 §6.6): every field
    // interning compares, as JSON with sokol's enum names (a "default" format stays "default", the
    // swapchain's at creation), without the shader, which the list names by its key. The same text
    // on every backend: a list recorded on one machine warms another.
    namespace SokolPipelineRecord {

        std::string toJson(const sg_pipeline_desc& desc);
        // false (with error) for malformed text or an unknown token; desc.shader is left invalid
        bool fromJson(const std::string& text, sg_pipeline_desc& desc, std::string* error = nullptr);

    }

}

#endif // SokolPipelineRecord_h
