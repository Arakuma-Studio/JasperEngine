// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S80-title-art-core.md §6.3, §6.5)
// - Purpose: the renderer's TitlePassRunner (TitlePasses.h): what an effect's `apply` and a
//   generator's `render` draw through inside one step of a render job. Targets come from the
//   renderer's pool (and go back with release() or when the runner ends), lookups are uploaded
//   once per content into the renderer's cache, and every pass draws at once through the
//   renderer's TitleGpu, the effects' programs (titleart.frag) and the renderer's own alike.
// - Programs: a pass whose program is still being compiled draws nothing and is noted in
//   compiling(); the job step that ran the definition then waits for those programs and runs it
//   again from the start (a definition writes all of its output and reads only its inputs), so
//   what the step leaves is never half drawn.
// - Thread-safety: main thread, for the duration of one step.

#pragma once

#include "Export.h"
#include "titleart/render/TitlePasses.h"

#include <memory>
#include <string>
#include <vector>

namespace jasperengine::titleart {

    class TitleRenderServices;
    struct TitleCacheEntry;

    class JASPERENGINE_API TitleJobRunner : public TitlePassRunner {
    public:
        explicit TitleJobRunner(TitleRenderServices& services);
        ~TitleJobRunner() override;
        TitleJobRunner(const TitleJobRunner&) = delete;
        TitleJobRunner& operator=(const TitleJobRunner&) = delete;

        const TitleArtCaps& caps() const override;
        TargetRef* acquire(const TargetKey& key) override;
        void release(TargetRef* target) override;
        const TitleLut* lut(const TitleLutData& data) override;
        bool run(const TitlePass& pass) override;
        bool clear(TargetRef& target) override;
        const std::string& error() const override;
        void clearError() override;

        // the colour texture a pass samples for a source (null when it has none)
        static TextureRender* textureOf(const TitleSource& source);
        // the renderer's GPU layer, pool and cache (its own passes and the tests' definitions)
        TitleRenderServices& renderServices() { return services; }
        // the programs (with their variant bits) passes skipped because they were not built yet
        const std::vector<uint32_t>& compiling() const { return pending; }

    private:
        struct Lut;
        TitleRenderServices& services;
        std::vector<std::unique_ptr<TargetRef>> lent;
        std::vector<std::unique_ptr<Lut>> luts;
        std::vector<uint32_t> pending;
        std::string failure;
    };

}
