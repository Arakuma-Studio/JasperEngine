// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef JASPER_TEST_GPU_ALLOCATIONS_H
#define JASPER_TEST_GPU_ALLOCATIONS_H

#include "sokol_gfx.h"

#include <cstddef>

namespace jasperengine { namespace test {

    // Observe actual resource creation at the graphics API boundary. Each scope
    // restores the prior diagnostic hooks, including when an assertion exits early.
    class GpuAllocations {
    public:
        GpuAllocations(){
            sg_trace_hooks hooks = {};
            hooks.user_data = this;
            hooks.make_buffer = [](const sg_buffer_desc*, sg_buffer, void* data){
                ++static_cast<GpuAllocations*>(data)->count;
            };
            hooks.make_image = [](const sg_image_desc*, sg_image, void* data){
                ++static_cast<GpuAllocations*>(data)->count;
            };
            previous = sg_install_trace_hooks(&hooks);
        }

        ~GpuAllocations(){ sg_install_trace_hooks(&previous); }
        GpuAllocations(const GpuAllocations&) = delete;
        GpuAllocations& operator=(const GpuAllocations&) = delete;

        size_t created() const{ return count; }

    private:
        sg_trace_hooks previous = {};
        size_t count = 0;
    };

}}

#endif
