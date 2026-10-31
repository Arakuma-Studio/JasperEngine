// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef SIGNATURE_H
#define SIGNATURE_H

#include <bitset>

namespace jasperengine {

    // one bit per registered component type (ComponentManager ids are the bit positions); 128 leaves room for
    // the engine's types (61 in 2026-10) and the ones the engine-gaps features add
    using Signature = std::bitset<128>;

}

#endif //SIGNATURE_H