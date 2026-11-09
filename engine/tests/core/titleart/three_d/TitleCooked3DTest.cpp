// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S86 R26, R37, R38 (AC26's player side): what an export cooks for a Realtime 3D title and how a
// game draws it. The fixed-geometry rule decides which 3D layers ship meshes (and whether a build
// needs the mesher); a game reads je-titleart/index.json and hands the renderer the cooked meshes
// and environment cubes. On the headless device, a plate lit by the softbox (an HDRI the game does
// not have) drawn from the cooked files without the mesher equals the editor's render of the same
// tier with the mesher and the HDRI-built cube (High and Low), and the editor's Bake within AC9's
// mean (k and segments differ by tier); red controls: without the index the layer has no mesh and
// says so, and without the cube the studio stands in.

#include "titleart/render/TitleRenderTestSupport.h"
#include "titleart/support/TitleTestAssets.h"

#include "TestPaths.h"
#include "titleart/three_d/Title3DRenderer.h"
#include "titleart/three_d/TitleCooked3D.h"
#include "titleart/three_d/TitleEnvironment.h"
#include "titleart/three_d/TitleMeshFile.h"
#include "titleart/three_d/Title3DSource.h"
#include "titleart/three_d/mesher/TitleMesher.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>

using namespace jasperengine;
using namespace jasperengine::titleart;
using namespace jasperengine::titleart::test;

namespace {

    const char* const TITLE_PATH = "titles/plate.titleart";

    Json plateLayer() {
        Json layer = shapeLayer("00000001", "rect", 120, solidFace("#ff0000"), Json{{"aspect", 2.5}, {"dimension", "3d"}});
        layer["style_3d"] = Json{{"face", materialPreset("gold")},
                                 {"bevel", {{"width", 12}, {"depth", 6}, {"profile", "round"}}},
                                 {"bands", Json::array({{{"grow", 3}, {"material", materialPreset("chrome")}}})}};
        return layer;
    }

    std::shared_ptr<const TitleArtDoc> plateDoc() {
        return makeDoc({{"canvas", canvas(512, 256)},
                        {"layers", Json::array({plateLayer()})},
                        {"scene_3d", {{"supersample", 1}, {"environment", {{"preset", "softbox"}, {"rotation", 25}}}}}});
    }

    std::string bundledHdriDir() {
        return (std::filesystem::path(jasperengine::test::testPaths().sourceDir) / ".." / ".." / ".." / "editor" / "resources" / "titleart" / "hdri")
            .lexically_normal()
            .string();
    }

    void useBundledHdris(bool on) {
        if (!on) {
            setTitleHdriLoader(nullptr);
            return;
        }
        const std::string dir = bundledHdriDir();
        setTitleHdriLoader([dir](const std::string& slug) { return loadTitleHdriFromDirectory(slug, dir); });
    }

    bool hasProblem(const TitleRenderJob& job, const char* code) {
        for (const TitleProblem& p : job.problems())
            if (p.code == code) return true;
        return false;
    }

    void writeBytes(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()), (std::streamsize)bytes.size());
    }

    // what the export writes for the plate: its mesh at each Realtime tier and the softbox cube
    // for the build's family (the CPU twin, uncompressed: the cube a device decodes to)
    std::filesystem::path cookPlate(const TitleArtDoc& doc) {
        const std::filesystem::path root = std::filesystem::path(jasperengine::test::testPaths().binaryDir) / "title-cooked-3d";
        std::filesystem::remove_all(root);
        const std::filesystem::path dir = root / TITLE_COOKED_DIR;
        Json index = Json{{"version", TITLE_COOKED_INDEX_VERSION},
                          {"realtime_3d_quality", {{"linux", "high"}}},
                          {"titles", Json::object()},
                          {"environments", Json::object()}};
        auto content = makeGeometryTitleContentProvider();
        Json entry = Json{{"meshes", Json::object()}, {"environments", Json::array({"softbox"})}};
        for (TitleTier tier : {TitleTier::High, TitleTier::Medium, TitleTier::Low}) {
            three_d::TitleMeshRequest r;
            r.source = three_d::collectTitleSource(doc.layers()[0], doc, *content, nullptr, nullptr);
            r.style = decodeStyle3D(doc.layers()[0]);
            r.blocks = three_d::collectCharBlocks(doc.layers()[0], r.style.bands.size());
            r.tier = tier;
            r.outputScale = 1.0;
            three_d::TitleMesher mesher;
            const std::string file = titleCookedMeshFile(TITLE_PATH, "00000001", tier);
            writeBytes(dir / file, writeTitleMesh(*mesher.build(r)));
            entry["meshes"]["00000001"][titleTierName(tier)] = file;
        }
        Environment3D env;
        env.preset = "softbox";
        const TitleEnvScene* scene = titleEnvironmentPreset("softbox");
        const std::shared_ptr<const TitleHdri> hdri = loadTitleHdri(scene->hdri);
        EXPECT_TRUE(hdri) << "the bundled Studio Small 09 at " << bundledHdriDir();
        const TitleEnvCube cube = buildTitleEnvironmentCpu(*scene, hdri.get(), titleHdriYaw("softbox"), titleEnvironmentFace(env, 1.0, TitleTier::High));
        std::vector<uint8_t> ktx2;
        std::string error;
        EXPECT_TRUE(writeTitleEnvironmentKtx2(cube, {{"JE.titleart.environment", "softbox"}}, ktx2, error)) << error;
        const std::string family = "desktop";
        const std::string envFile = titleCookedEnvironmentFile(family, "softbox");
        writeBytes(dir / envFile, ktx2);
        index["environments"][family]["softbox"] = envFile;
        index["titles"][TITLE_PATH] = entry;
        const std::string text = index.dump(1);
        writeBytes(dir / TITLE_COOKED_INDEX_FILE, std::vector<uint8_t>(text.begin(), text.end()));
        return root;
    }

    class TitleCooked3DGpu : public ::testing::Test {
    protected:
        void SetUp() override { JASPER_REQUIRE_GPU(); }
        void TearDown() override {
            setTitleHdriLoader(nullptr);
            resetTitleCookedIndexForTesting();
        }
    };

}

TEST(TitleCooked3D, FixedGeometryRule) {
    Json layer = plateLayer();
    layer["chars"] = Json{{"1", {{"tilt_x", 10}}}};
    auto docWith = [&](const char* field) {
        Json root = Json::object();
        root["exposed"] = Json::array({{{"name", "p"}, {"type", "number"}, {"targets", Json::array({{{"layer", "00000001"}, {"field", field}}})}}});
        return root;
    };
    std::string why;
    EXPECT_TRUE(titleArt3DFixedGeometry(Json::object(), layer, &why)) << "nothing exposed";
    for (const char* safe : {"transform.rotation_y", "3d.face.color", "style_3d.lighting.environment.rotation", "style_3d.skew.x", "opacity",
                             "band#0.material.roughness", "style_3d.bands[0].material.color", "chars.1.tilt_x", "chars.1.face_3d"}) {
        EXPECT_TRUE(titleArt3DFixedGeometry(docWith(safe), layer, &why)) << safe << ": " << why;
    }
    for (const char* changes : {"text", "font", "size", "style_3d.depth", "3d.bevel.width", "band#0.grow", "style_3d.bands[0].depth",
                                "chars.2.tilt_x", "chars.1.text"}) {
        why.clear();
        EXPECT_FALSE(titleArt3DFixedGeometry(docWith(changes), layer, &why)) << changes;
        EXPECT_NE(why.find(changes), std::string::npos) << why;
    }
    // the 2D style is not drawn while 3D, unless the 3D style derives from it (null style_3d)
    EXPECT_TRUE(titleArt3DFixedGeometry(docWith("style.face.fill.color"), layer, &why));
    Json derived = layer;
    derived["style_3d"] = nullptr;
    EXPECT_FALSE(titleArt3DFixedGeometry(docWith("style.face.fill.color"), derived, &why));
    // another layer's target leaves this one fixed
    Json other = docWith("text");
    other["exposed"][0]["targets"][0]["layer"] = "00000002";
    EXPECT_TRUE(titleArt3DFixedGeometry(other, layer, &why));
}

TEST(TitleCooked3D, FileNames) {
    EXPECT_EQ(titleCookedEnvironmentFile("web", "studio"), "environments/web/studio.ktx2");
    const std::string high = titleCookedMeshFile(TITLE_PATH, "00000001", TitleTier::High);
    EXPECT_EQ(high.rfind("meshes/", 0), 0u);
    EXPECT_NE(high.find("-high.titlemesh"), std::string::npos);
    EXPECT_NE(high, titleCookedMeshFile(TITLE_PATH, "00000002", TitleTier::High)) << "one file per layer";
    EXPECT_NE(high, titleCookedMeshFile("titles/other.titleart", "00000001", TitleTier::High)) << "and per title";
}

TEST_F(TitleCooked3DGpu, APlayerDrawsTheExportsDataAsTheEditorDoes) {
    const std::shared_ptr<const TitleArtDoc> doc = plateDoc();
    useBundledHdris(true);
    const std::filesystem::path root = cookPlate(*doc);

    // the editor: the mesher and the HDRI's cube built on the GPU, per tier and at Bake
    std::map<int, Pixels> editor;
    for (int tier : {0, 1, 3}) {
        auto renderer = std::make_unique<TitleArtRenderer>();
        renderer->setContentProvider(makeGeometryTitleContentProvider());
        RenderRequest r;
        r.doc = doc;
        r.tier = tier;
        std::unique_ptr<TitleRenderJob> job;
        editor[tier] = render(*renderer, r, &job);
        EXPECT_FALSE(hasProblem(*job, "environment_missing")) << "tier " << tier;
    }

    // the game: no HDRI, no mesher; the export's index and files
    useBundledHdris(false);
    resetTitleCookedIndexForTesting(root.string());
    TitleTier project = TitleTier::Bake;
    EXPECT_TRUE(titleCookedProjectTier(4, project)) << "Linux's Realtime 3D Quality from the index";
    EXPECT_EQ(project, TitleTier::High);
    const std::shared_ptr<TitleCooked3DSource> cooked = titleCooked3DFor(TITLE_PATH);
    ASSERT_TRUE(cooked);
    EXPECT_FALSE(titleCooked3DFor("titles/other.titleart")) << "a title the export cooked nothing for";
    for (int tier : {1, 3}) {
        SCOPED_TRACE(tier);
        TitleJobSpec spec;
        cooked->fill(tier, spec);
        ASSERT_EQ(spec.cookedMeshes3D.count("00000001"), 1u);
        EXPECT_EQ((int)spec.cookedMeshes3D["00000001"]->tier, tier);
        ASSERT_EQ(spec.cookedEnvironments3D.count("softbox"), 1u);
        auto renderer = std::make_unique<TitleArtRenderer>();
        renderer->setContentProvider(makeGeometryTitleContentProvider());
        RenderRequest r;
        r.doc = doc;
        r.tier = tier;
        r.testFlags = TITLE_ART_TEST_NO_MESHER;
        r.cookedMeshes3D = spec.cookedMeshes3D;
        r.cookedEnvironments3D = spec.cookedEnvironments3D;
        std::unique_ptr<TitleRenderJob> job;
        const Pixels player = render(*renderer, r, &job);
        for (const TitleProblem& p : job->problems()) ADD_FAILURE() << p.code << ": " << p.message;
        ASSERT_EQ(job->meshReports().size(), 1u);
        EXPECT_TRUE(job->meshReports()[0].cooked);
        // S86's tolerance for a cooked cube against the built one (AC13's test): mean 3/255, 2 % over 24/255
        const Difference d = compare(editor[tier], player, 24.0);
        std::printf("[cooked3d] tier %d: player vs editor mean %.2f/255, %.2f %% over 24/255\n", tier, d.mean, d.overFraction * 100.0);
        EXPECT_LE(d.mean, 3.0);
        EXPECT_LE(d.overFraction, 0.02);
        // against the editor's bake: the tier's k (2 and 1 against 3) and its segments (bevel <= 8 and <= 3 against
        // 24) move the edges' antialiasing; AC9's mean holds, its 1 % of pixels does not at High (S86 deviation 31)
        const Difference bake = compare(editor[0], player, 24.0);
        std::printf("[cooked3d] tier %d: player vs editor Bake mean %.2f/255, %.2f %% over 24/255\n", tier, bake.mean, bake.overFraction * 100.0);
        EXPECT_LE(bake.mean, 2.0);
        EXPECT_LE(bake.overFraction, 0.03);
    }

    // red controls: no cooked mesh in a build without the mesher draws nothing and says why; no
    // cooked cube lights the softbox look with the studio
    {
        auto renderer = std::make_unique<TitleArtRenderer>();
        renderer->setContentProvider(makeGeometryTitleContentProvider());
        RenderRequest r;
        r.doc = doc;
        r.tier = 1;
        r.testFlags = TITLE_ART_TEST_NO_MESHER;
        std::unique_ptr<TitleRenderJob> job;
        const Pixels nothing = render(*renderer, r, &job);
        EXPECT_TRUE(hasProblem(*job, "mesher_unavailable"));
        EXPECT_LT(nothing.at(256, 128)[3], 1.0f / 255.0f);
        TitleJobSpec spec;
        cooked->fill(1, spec);
        r.cookedMeshes3D = spec.cookedMeshes3D;
        const Pixels studio = render(*renderer, r, &job);
        EXPECT_TRUE(hasProblem(*job, "environment_missing"));
        EXPECT_GT(compare(editor[1], studio, 24.0).mean, 3.0) << "the studio stand-in is not the softbox";
    }
}

TEST_F(TitleCooked3DGpu, TheJobNamesEveryProgramAnExportPrecompiles) {
    // S86 §6.4, AC22: a build without the runtime shader compiler draws only what the export
    // precompiled from the job's keys. Low holds compressed light only; High both encodings
    auto renderer = std::make_unique<TitleArtRenderer>();
    renderer->setContentProvider(makeGeometryTitleContentProvider());
    Json layer = plateLayer();
    layer["style_3d"]["glint"] = Json{{"enabled", true}};
    auto doc = makeDoc({{"canvas", canvas(512, 256)}, {"layers", Json::array({layer})}, {"scene_3d", {{"supersample", 1}}}});
    auto keysAt = [&](int tier) {
        RenderRequest r;
        r.doc = doc;
        r.tier = tier;
        return renderer->begin(r)->shaderKeys();
    };
    auto count = [](const std::vector<ShaderKey>& keys, ShaderType type, bool compressed) {
        int n = 0;
        for (const ShaderKey& k : keys) {
            if (k.type == (uint16_t)type && ((k.properties & TitleLitBits::HDR_COMPRESS) != 0) == compressed) n++;
        }
        return n;
    };
    const std::vector<ShaderKey> low = keysAt(3), high = keysAt(1);
    EXPECT_GT(count(low, ShaderType::TITLE_LIT, true), 0);
    EXPECT_EQ(count(low, ShaderType::TITLE_LIT, false), 0) << "Low always draws compressed light";
    EXPECT_GT(count(high, ShaderType::TITLE_LIT, true), 0) << "a device without float targets";
    EXPECT_GT(count(high, ShaderType::TITLE_LIT, false), 0);
    auto has = [](const std::vector<ShaderKey>& keys, ShaderType type) {
        return std::any_of(keys.begin(), keys.end(), [type](const ShaderKey& k) { return k.type == (uint16_t)type; });
    };
    EXPECT_TRUE(has(low, ShaderType::TITLE_3D_POST));
    EXPECT_TRUE(has(low, ShaderType::TITLE_GLINT_SPRITE)) << "Low's glint is the sprites";
    EXPECT_FALSE(has(high, ShaderType::TITLE_GLINT_SPRITE));
    EXPECT_TRUE(has(high, ShaderType::TITLE_GLINT)) << "High's is the full chain";
}
