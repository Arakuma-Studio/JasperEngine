// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S91 AC15: the Particle Effect window's Checks rows (ParticleChecksPanel) and preview_particle_effect's
// checks match on one effect: the panel's request and the tool's (ParticlePreviewSource with the
// tool's arguments) give the same rows; the sheet bake's pieces (R12). Runs a headless project, as
// SpriteNormalsApplyTest does.

#include "AppSettings.h"
#include "JasperEngine.h"
#include "Platform.h"
#include "Project.h"
#include "particles/ParticleEffectCodec.h"
#include "particles/ParticlePreviewSource.h"
#include "particles/ParticleSheetBake.h"
#include "window/widget/ParticleChecksPanel.h"
#include "ScriptDataTestSupport.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <unistd.h>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

    struct Headless {
        fs::path dir;
        bool ready = false;
        std::unique_ptr<Project> project;
        std::unique_ptr<editor::Platform> platform;
    };

    Headless& headless(){
        static Headless h = [](){
            Headless out;
            const fs::path root = fs::temp_directory_path() / ("jasper-particle-checks-test-" + std::to_string(::getpid()));
            fs::remove_all(root);
            fs::create_directories(root / "config");
            setenv("XDG_CONFIG_HOME", (root / "config").c_str(), 1);
            out.dir = root / "project";
            fs::create_directories(out.dir / ".jasperengine");
            fs::create_directories(out.dir / "assets" / "fx");
            std::ofstream(out.dir / "project.yaml") << "name: particlechecks\n";
            // a burst over Max Particles: capacity_hit fails, and has a fix
            std::ofstream(out.dir / "assets" / "fx" / "burst.particles") << R"({"version": 1, "duration": 1, "looping": false, "seed": 5,
  "maxParticles": 400, "modules": [{"type": "main", "startLifetime": 1, "startSpeed": 2, "startSize": 0.2},
  {"type": "emission", "rateOverTime": 0, "bursts": [{"time": 0, "count": 600}]}]})";
            AppSettings::initialize();
            out.project.reset(new Project());
            out.platform.reset(new editor::Platform(out.project.get()));
            headlesssystem::use(out.platform.get());
            Engine::pauseGameEvents(true);
            Engine::setAsyncLoading(false);
            out.ready = out.project->loadProject(out.dir, false);
            return out;
        }();
        headlesssystem::use(h.platform.get());
        return h;
    }

}

TEST(ParticleChecksPanel, RowsMatchTheToolsChecks){
    Headless& h = headless();
    ASSERT_TRUE(h.ready);
    ParticleQaSettings qa;
    qa.overdrawMobile = 6.0f;   // the project's thresholds reach both
    h.project->setParticleQaSettings(qa);

    // the window's path
    ParticleChecksPanel panel;
    panel.setAsset("fx/burst.particles");
    ParticlePreviewRequest windowRequest;
    std::string error;
    ASSERT_TRUE(panel.buildRequest(h.project.get(), windowRequest, error)) << error;
    qa::Json windowReport;
    ASSERT_TRUE(ParticleEffectPreview::runCpu(windowRequest, windowReport, error)) << error;

    // the tool's path: its source and arguments (frames 0, the default platform)
    ParticlePreviewRequest toolRequest;
    ASSERT_TRUE(ParticlePreviewSource::fromAsset("fx/burst.particles", toolRequest.systems, error)) << error;
    ASSERT_TRUE(ParticlePreviewSource::applyArgs(qa::Json{{"frames", 0}}, toolRequest, error)) << error;
    toolRequest.qa = h.project->getParticleQaSettings();
    qa::Json toolReport;
    ASSERT_TRUE(ParticleEffectPreview::runCpu(toolRequest, toolReport, error)) << error;

    EXPECT_EQ(windowReport["checks"].dump(), toolReport["checks"].dump());
    const std::vector<ParticleChecksPanel::Row> rows = ParticleChecksPanel::rowsOf(windowReport);
    ASSERT_EQ(rows.size(), toolReport["checks"].size());
    for (size_t k = 0; k < rows.size(); k++){
        EXPECT_EQ(rows[k].id, toolReport["checks"][k]["id"]);
        EXPECT_EQ(rows[k].status, toolReport["checks"][k]["status"]);
    }
    EXPECT_EQ(rows[0].id, "capacity_hit");
    EXPECT_EQ(rows[0].status, "FAIL");
    EXPECT_EQ(rows[0].fix, 0) << "a single edit: the Fix button";
    EXPECT_EQ(toolReport["fixes"][0]["path"], "fx/burst.particles");
    EXPECT_EQ(toolReport["thresholds"]["changed"][0]["key"], "particles.qa.overdraw_mobile");
    h.project->setParticleQaSettings(ParticleQaSettings());
}

TEST(ParticleSheetBake, SheetIsTheCellsRowByRowAndItsEffectPlaysIt){
    std::vector<PoseImage> cells;
    for (int k = 0; k < 6; k++) cells.emplace_back(8, 8, PoseColor((uint8_t)(k * 40), 0, 0, (uint8_t)(100 + k)));
    const PoseImage sheet = ParticleSheetBake::composeSheet(cells, 3, 2);
    ASSERT_EQ(sheet.getWidth(), 24);
    ASSERT_EQ(sheet.getHeight(), 16);
    EXPECT_EQ(sheet.getPixel(17, 2).r, 80) << "the third cell, top right";
    EXPECT_EQ(sheet.getPixel(1, 9).a, 103) << "the fourth, bottom left";

    const particles::ParticleEffectDocument effect = ParticleSheetBake::sheetEffect("fx/s.png", 3, 2, 1.5, 2.0f, false, true);
    std::vector<particles::Diagnostic> diagnostics;
    EXPECT_TRUE(particles::ParticleEffectCodec::validate(effect, diagnostics)) << particles::ParticleEffectCodec::format(diagnostics);
    EXPECT_EQ(effect.textureSheetAnimation.tilesX, 3);
    EXPECT_EQ(effect.textureSheetAnimation.tilesY, 2);
    EXPECT_EQ(effect.renderer.blend, particles::ParticleBlend::Additive);
    EXPECT_FLOAT_EQ(effect.duration, 1.5f);

    EXPECT_FALSE(ParticleSheetBake::validate(5, 5, 128).empty()) << "25 frames: over 24";
    EXPECT_FALSE(ParticleSheetBake::validate(4, 4, 2048).empty());
    EXPECT_TRUE(ParticleSheetBake::validate(4, 4, 128).empty());
}
