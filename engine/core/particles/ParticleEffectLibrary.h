// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef PARTICLES_PARTICLEEFFECTLIBRARY_H
#define PARTICLES_PARTICLEEFFECTLIBRARY_H

// The .particles assets a process has read (docs/specs/S20-particles.md R1, R33): each file
// read and validated once, shared by every Particle System that references it, with a version
// that a reload (hot reload, an edit by the editor or an agent) raises, so its users rebind
// (keeping their particles when only parameters changed). Main thread.

#include "Export.h"
#include "particles/ParticleEffect.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine { namespace particles {

    struct EffectAsset {
        std::shared_ptr<const ParticleEffectDocument> document;   // null when it could not be read
        std::vector<Diagnostic> diagnostics;
        uint64_t version = 0;
    };

    class JASPERENGINE_API ParticleEffectLibrary {
    public:
        // the asset at a path (project-relative, as the engine reads files), read on first use
        static const EffectAsset& get(const std::string& path);
        // the file changed: read again now; its users rebind on their next update
        static void reload(const std::string& path);
        // the document an editor holds for the path (an edit not saved yet, or one just
        // written): users see it at once
        static void set(const std::string& path, const ParticleEffectDocument& document);
        // forgets every asset (a project closed)
        static void clear();
        static uint64_t version(const std::string& path);
        // moves on with any read, edit or clear: a user of many assets compares it first
        static uint64_t generation();
    };

}}

#endif // PARTICLES_PARTICLEEFFECTLIBRARY_H
