// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ParticleKernels.h"

#include <atomic>

using namespace jasperengine;

namespace jasperengine::particles {

namespace {

    template<class B>
    KernelTable makeTable(){
        KernelTable table;
        table.backend = B::name;
        table.spawn = &Kernels<B>::spawnOp;
        table.update = &Kernels<B>::updateOp;
        return table;
    }

    std::atomic<bool> forcedScalar{false};

}

const KernelTable& nativeKernels(){
    static const KernelTable table = makeTable<simd::Native>();
    return table;
}

const KernelTable& scalarKernels(){
    static const KernelTable table = makeTable<simd::Scalar>();
    return table;
}

const KernelTable& activeKernels(){
    return forcedScalar.load(std::memory_order_relaxed) ? scalarKernels() : nativeKernels();
}

void setForceScalarKernels(bool forceScalar){
    forcedScalar.store(forceScalar, std::memory_order_relaxed);
}

bool forceScalarKernels(){
    return forcedScalar.load(std::memory_order_relaxed);
}

}  // namespace jasperengine::particles
