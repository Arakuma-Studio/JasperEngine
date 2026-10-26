// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ParticleKernels.h"

#include <atomic>

using namespace jasperengine;
using namespace jasperengine::particles;

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

const KernelTable& particles::nativeKernels(){
    static const KernelTable table = makeTable<simd::Native>();
    return table;
}

const KernelTable& particles::scalarKernels(){
    static const KernelTable table = makeTable<simd::Scalar>();
    return table;
}

const KernelTable& particles::activeKernels(){
    return forcedScalar.load(std::memory_order_relaxed) ? scalarKernels() : nativeKernels();
}

void particles::setForceScalarKernels(bool forceScalar){
    forcedScalar.store(forceScalar, std::memory_order_relaxed);
}

bool particles::forceScalarKernels(){
    return forcedScalar.load(std::memory_order_relaxed);
}
