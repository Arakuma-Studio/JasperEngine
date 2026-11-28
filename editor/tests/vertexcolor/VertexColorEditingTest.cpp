// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// docs/specs/V1-vertex-colors.md R8, R10, R26, R31, R50, R106, R107, R120, R132, D7: the editor's
// vertex color edits: the component and file a first edit makes, one undo step per edit, dry runs,
// all-or-nothing over targets, copy-on-write, the stroke's cap and cancel, the delta undo's memory
// and its refusal, the atomic file written at the scene's save, the remap of a changed mesh, the
// element selection's undo and vertex_blend's black start.

#include "VertexColorTestScene.h"

#include "command/type/PropertyCmd.h"
#include "texture/Material.h"
#include "vertexcolor/VertexColorEditorInternal.h"
#include "vertexcolor/VertexColorFile.h"
#include "jobs/JobSystem.h"

#include <atomic>
#include <chrono>
#include <fstream>
#include <iterator>
#include <thread>

namespace VertexColorEditingTest_cpp {

using namespace jasperengine;
using namespace jasperengine::editor;

// (a named namespace, tests included: a unity build merges the test sources)
namespace vertex_color_editing_test {
    using namespace jasperengine::editor::vertexcolortest;

    const Vector4 kRed(1.0f, 0.0f, 0.0f, 1.0f);
    const Vector4 kBlue(0.0f, 0.0f, 1.0f, 1.0f);

    float centroidX(const vc::MeshView& mesh, uint32_t t) {
        return (mesh.positions[mesh.indices[t * 3]].x + mesh.positions[mesh.indices[t * 3 + 1]].x + mesh.positions[mesh.indices[t * 3 + 2]].x) / 3.0f;
    }

    class VertexColorEditing : public VertexColorSceneTest {
    protected:
        // the faces left of x = 0 set to `value`: a hard edge along x = 0
        vc::Op leftFaces(Entity entity, const Vector4& value) {
            const std::shared_ptr<const vc::MeshView> mesh = vce::meshView(project.get(), sceneId, entity, firstKey(entity));
            vc::Op op;
            op.type = vc::OpType::Set;
            op.target.kind = vc::TargetKind::Faces;
            op.value = value;
            for (uint32_t t = 0; mesh && t < mesh->triangleCount(); t++) {
                if (centroidX(*mesh, t) < 0.0f) op.target.ids.push_back(t);
            }
            EXPECT_FALSE(op.target.ids.empty());
            return op;
        }

        // every corner at a vertex of the 4-segment grid (0.5 apart) is red left of x = 0, blue right
        void expectSides(Entity entity) {
            const vc::TargetData* data = vce::targetData(project.get(), sceneId, entity, firstKey(entity));
            const std::shared_ptr<const vc::MeshView> mesh = vce::meshView(project.get(), sceneId, entity, firstKey(entity));
            ASSERT_NE(data, nullptr);
            ASSERT_NE(mesh, nullptr);
            ASSERT_EQ(data->fingerprint, mesh->fingerprint());
            uint32_t checked = 0;
            for (uint32_t c = 0; c < (uint32_t)mesh->indices.size(); c++) {
                const Vector3& p = mesh->positions[mesh->indices[c]];
                bool old = true;
                for (unsigned a = 0; a < 3; a++) old = old && std::fabs(p[a] * 2.0f - std::round(p[a] * 2.0f)) < 1e-4f;
                if (!old) continue;
                EXPECT_EQ(data->cornerValue(c, mesh->indices), centroidX(*mesh, c / 3) < 0.0f ? kRed : kBlue) << "corner " << c;
                checked++;
            }
            EXPECT_GT(checked, 0u);
            EXPECT_TRUE(data->hasOverrides()) << "the vertices on the edge split";
        }
    };

    std::vector<uint8_t> fileBytes(const fs::path& path) {
        std::ifstream in(path, std::ios::binary);
        return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }

    vc::Op strokeThrough(const Vector3& from, const Vector3& to, int passes, float strength, bool buildup) {
        vc::Op op;
        op.type = vc::OpType::Stroke;
        op.value = Vector4(0.0f, 0.0f, 0.0f, 1.0f);
        op.channels = vc::CHANNELS_RGB;
        op.strength = strength;
        op.radius = 0.3f;
        op.falloff = vc::FalloffPreset::Constant;
        op.buildup = buildup;
        for (int pass = 0; pass < passes; pass++) {
            op.points.push_back({pass % 2 == 0 ? from : to, 1.0f});
            op.points.push_back({pass % 2 == 0 ? to : from, 1.0f});
        }
        return op;
    }

TEST_F(VertexColorEditing, FirstEditMakesComponentAndFileInOneUndoStep) {
    const Entity box = addBox();
    ASSERT_EQ(colorsOf(box), nullptr);
    const size_t before = steps();

    const vce::EditResult result = edit(box, {fillAll(kRed)}, "Apply Color to 12 Faces");
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_TRUE(result.createdComponent);
    EXPECT_TRUE(result.createdFile);
    ASSERT_NE(colorsOf(box), nullptr);
    const std::string path = colorsOf(box)->data.path;
    EXPECT_EQ(path, "VertexColors/Box.vcolors") << "a scene outside the assets keeps its files in VertexColors/";
    EXPECT_TRUE(fs::exists(project->getAssetsPath() / path));
    EXPECT_FALSE(colorsOf(box)->data.guid.isNull()) << "the new file has its GUID (S04)";
    EXPECT_TRUE(std::any_of(result.warnings.begin(), result.warnings.end(), [](const vc::Warning& w) { return w.code == "created"; }));

    // one step, with its name
    EXPECT_EQ(steps(), before + 1);
    ASSERT_NE(history->top(), nullptr);
    EXPECT_EQ(history->top()->name(), "Apply Color to 12 Faces");
    const uint64_t painted = hashOf(box);
    ASSERT_NE(painted, 0u);

    ASSERT_TRUE(history->undo());
    EXPECT_EQ(colorsOf(box), nullptr) << "the undo takes the component back with the paint";
    ASSERT_TRUE(history->redo());
    ASSERT_NE(colorsOf(box), nullptr);
    EXPECT_EQ(colorsOf(box)->data.path, path);
    EXPECT_EQ(hashOf(box), painted) << "redo puts the same values back";

    // a second edit is one more step, and its undo gives the first edit's hash back
    ASSERT_TRUE(edit(box, {fillAll(kBlue)}).ok);
    EXPECT_EQ(steps(), before + 2);
    EXPECT_NE(hashOf(box), painted);
    ASSERT_TRUE(history->undo());
    EXPECT_EQ(hashOf(box), painted);
    ASSERT_TRUE(history->redo());
    EXPECT_NE(hashOf(box), painted);
}

TEST_F(VertexColorEditing, NewFilesGoBesideTheScene) {
    SceneProject inside;
    inside.filepath = project->getAssetsPath() / "levels" / "Level 1.scene";
    EXPECT_EQ(vce::detail::newFilePath(*project, inside, "Rock/01"), "levels/Level 1_VertexColors/Rock_01.vcolors");
    SceneProject unsaved;
    EXPECT_EQ(vce::detail::newFilePath(*project, unsaved, "Rock"), "VertexColors/Rock.vcolors");
}

TEST_F(VertexColorEditing, DryRunWritesNothing) {
    const Entity box = addBox();
    const size_t before = steps();
    const vce::EditResult result = edit(box, {fillAll(kRed)}, "Apply Color", true);
    ASSERT_TRUE(result.ok) << result.error;
    ASSERT_EQ(result.targets.size(), 1u);
    EXPECT_NE(result.targets[0].result.hash, 0u) << "the dry run reports what the edit would make";
    EXPECT_GT(result.targets[0].result.ops[0].changedVertices, 0u);
    EXPECT_EQ(colorsOf(box), nullptr);
    EXPECT_FALSE(fs::exists(project->getAssetsPath() / "VertexColors"));
    EXPECT_EQ(steps(), before);

    // the same edit for real gives the hash the dry run said
    const vce::EditResult real = edit(box, {fillAll(kRed)});
    ASSERT_TRUE(real.ok);
    EXPECT_EQ(real.targets[0].result.hash, result.targets[0].result.hash);
    EXPECT_EQ(hashOf(box), result.targets[0].result.hash);
}

TEST_F(VertexColorEditing, FailingTargetLeavesEveryTargetAsItWas) {
    const Entity ball = addSphere();
    const Entity box = addBox();
    ASSERT_TRUE(edit(box, {fillAll(kBlue)}).ok);
    const uint64_t boxHash = hashOf(box);
    const std::vector<uint8_t> boxFile = (vce::detail::flushWrites(), fileBytes(project->getAssetsPath() / colorsOf(box)->data.path));
    const size_t before = steps();

    // vertex 300 is the sphere's, past the box's 8 logical vertices
    vc::Op op;
    op.type = vc::OpType::Set;
    op.target.kind = vc::TargetKind::Vertices;
    op.target.ids = {300};
    op.value = kRed;
    vce::EditRequest request;
    request.targets = {{ball, firstKey(ball)}, {box, firstKey(box)}};
    request.ops = {op};
    const vce::EditResult result = vce::applyEdit(project.get(), sceneId, request);
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("targets[1]"), std::string::npos) << result.error;
    EXPECT_NE(result.error.find("ops[0].target"), std::string::npos) << result.error;
    EXPECT_EQ(colorsOf(ball), nullptr) << "the sphere's edit went with the box's";
    EXPECT_EQ(hashOf(box), boxHash);
    EXPECT_EQ(steps(), before);
    vce::detail::flushWrites();
    EXPECT_EQ(fileBytes(project->getAssetsPath() / colorsOf(box)->data.path), boxFile);
}

TEST_F(VertexColorEditing, DuplicateSharesTheFileUntilItsFirstEdit) {
    const Entity original = addBox("Rock");
    ASSERT_TRUE(edit(original, {fillAll(kRed)}).ok);
    const uint64_t red = hashOf(original);
    const std::string path = colorsOf(original)->data.path;

    // what Duplicate leaves: the same asset and the same decoded values (EntityCloner)
    const Entity copy = addBox("Rock Copy");
    scene()->addComponent<VertexColorsComponent>(copy, *colorsOf(original));
    EXPECT_EQ(vce::fileUserCount(project.get(), path), 2);
    EXPECT_EQ(hashOf(copy), red);

    const vce::EditResult result = edit(copy, {fillAll(kBlue)});
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_NE(colorsOf(copy)->data.path, path) << "the edited copy got its own file";
    EXPECT_NE(colorsOf(copy)->loaded, colorsOf(original)->loaded);
    EXPECT_EQ(hashOf(original), red) << "the original's colors stay";
    EXPECT_NE(hashOf(copy), red);
    EXPECT_EQ(vce::fileUserCount(project.get(), path), 1);

    // one step: the copy and the paint go back together
    ASSERT_TRUE(history->undo());
    EXPECT_EQ(colorsOf(copy)->data.path, path);
    EXPECT_EQ(hashOf(copy), red);
    EXPECT_EQ(hashOf(original), red);
}

TEST_F(VertexColorEditing, StrokeCapsAtItsStrengthAndCancels) {
    const Entity grid = addGrid("Floor", 8);
    const vc::TargetKey key = firstKey(grid);
    const std::shared_ptr<const vc::MeshView> mesh = vce::meshView(project.get(), sceneId, grid, key);
    ASSERT_NE(mesh, nullptr);
    const Vector3 from(-1.0f, 0.0f, 0.0f), to(1.0f, 0.0f, 0.0f);

    // R31 (AC8): crossing a vertex ten times at 30 % moves it 30 % of the way, in one undo step
    const size_t before = steps();
    vce::StrokeSession stroke;
    std::string error;
    ASSERT_TRUE(stroke.begin(project.get(), sceneId, {{grid, key}}, "Paint Vertex Colors", &error)) << error;
    for (int call = 0; call < 10; call++) {
        ASSERT_TRUE(stroke.apply(strokeThrough(from, to, 1, 0.3f, false)));
    }
    stroke.end();
    EXPECT_EQ(steps(), before + 1);
    EXPECT_EQ(history->top()->name(), "Paint Vertex Colors");
    const vc::TargetData* data = vce::targetData(project.get(), sceneId, grid, key);
    ASSERT_NE(data, nullptr);
    // the centre vertex lies on the stroke: white to black, 30 %
    uint32_t centre = 0;
    for (uint32_t v = 0; v < mesh->vertexCount(); v++) {
        if (std::fabs(mesh->positions[v].x) < 1e-4f && std::fabs(mesh->positions[v].z) < 1e-4f) centre = v;
    }
    EXPECT_NEAR(data->base[centre].x, 0.7f, 1.0f / 255.0f);

    // Build-up: past the cap
    ASSERT_TRUE(history->undo());
    ASSERT_TRUE(stroke.begin(project.get(), sceneId, {{grid, key}}, "Paint Vertex Colors", &error)) << error;
    for (int call = 0; call < 10; call++) ASSERT_TRUE(stroke.apply(strokeThrough(from, to, 1, 0.3f, true)));
    stroke.end();
    data = vce::targetData(project.get(), sceneId, grid, key);
    ASSERT_NE(data, nullptr);
    EXPECT_LT(data->base[centre].x, 0.69f);

    // R106: cancel puts the values back and takes back nothing more than the stroke
    const uint64_t kept = hashOf(grid);
    const size_t kept_steps = steps();
    ASSERT_TRUE(stroke.begin(project.get(), sceneId, {{grid, key}}, "Paint Vertex Colors", &error)) << error;
    vc::Op white = strokeThrough(from, to, 3, 1.0f, false);
    white.value = Vector4(1.0f, 1.0f, 1.0f, 1.0f);
    ASSERT_TRUE(stroke.apply(white));
    EXPECT_NE(hashOf(grid), kept) << "the stroke shows as it goes";
    stroke.cancel();
    EXPECT_EQ(hashOf(grid), kept);
    EXPECT_EQ(steps(), kept_steps);

    // a cancelled first stroke leaves no component behind
    const Entity other = addGrid("Other", 4);
    ASSERT_TRUE(stroke.begin(project.get(), sceneId, {{other, firstKey(other)}}, "Paint Vertex Colors", &error)) << error;
    ASSERT_TRUE(stroke.apply(strokeThrough(from, to, 1, 1.0f, false)));
    EXPECT_NE(colorsOf(other), nullptr);
    stroke.cancel();
    EXPECT_EQ(colorsOf(other), nullptr);
}

TEST_F(VertexColorEditing, UndoKeepsDeltasAndRefusesAChangedMesh) {
    const Entity ball = addSphere();
    ASSERT_TRUE(edit(ball, {fillAll(kBlue)}).ok);
    const vc::TargetKey key = firstKey(ball);
    const size_t vertices = vce::targetData(project.get(), sceneId, ball, key)->base.size();
    ASSERT_GT(vertices, 1000u);

    // a small sphere at the pole: the step holds the XOR of the chunks it touched, packed
    vc::Op dab = fillAll(kRed);
    dab.target.kind = vc::TargetKind::Sphere;
    dab.target.center = Vector3(0.0f, 1.0f, 0.0f);
    dab.target.radius = 0.2f;
    ASSERT_TRUE(edit(ball, {dab}, "Paint Vertex Colors").ok);
    const UndoEntry* top = history->top();
    ASSERT_NE(top, nullptr);
    const size_t copyBytes = vertices * sizeof(Vector4);
    EXPECT_LT(top->info.bytes, copyBytes / 4) << "a delta, packed, not a copy of the values";
    EXPECT_GT(top->info.bytes, 0u);

    // the target remapped meanwhile (its fingerprint is another): the step is refused, the values stay
    vc::TargetData* live = const_cast<vc::TargetData*>(vce::targetData(project.get(), sceneId, ball, key));
    const uint64_t now = vc::hashTarget(*live);
    live->fingerprint.topologyHash ^= 1;
    EXPECT_FALSE(history->undo());
    EXPECT_NE(history->lastRefusal().find("changed"), std::string::npos) << history->lastRefusal();
    EXPECT_EQ(vc::hashTarget(*live), now);
    live->fingerprint.topologyHash ^= 1;
    EXPECT_TRUE(history->undo());
}

TEST_F(VertexColorEditing, SceneSaveWritesTheFileWithItsSnapshot) {
    const Entity box = addBox();
    ASSERT_TRUE(edit(box, {fillAll(kRed)}).ok);
    const std::string path = colorsOf(box)->data.path;
    ASSERT_TRUE(project->saveSceneNow(sceneId));
    EXPECT_FALSE(vertexColorFileDirty(*project, path));

    vc::Data onDisk;
    std::string error;
    ASSERT_TRUE(vc::readFile(project->getAssetsPath() / path, onDisk, error)) << error;
    ASSERT_EQ(onDisk.targets.size(), 1u);
    EXPECT_EQ(vc::hashTarget(onDisk.targets[0]), hashOf(box));
    EXPECT_EQ(onDisk.targets[0].snapshotPositions.size(), onDisk.targets[0].base.size()) << "the editor-only snapshot (R9)";
    // nothing beside it but its .meta: the write went through a renamed temporary (AtomicFile)
    for (const fs::directory_entry& entry : fs::directory_iterator((project->getAssetsPath() / path).parent_path())) {
        const std::string name = entry.path().filename().string();
        EXPECT_TRUE(name == "Box.vcolors" || name == "Box.vcolors.meta") << name;
    }

    // an edit after the save makes the file dirty until it is written
    ASSERT_TRUE(edit(box, {fillAll(kBlue)}).ok);
    vce::detail::flushWrites();
    ASSERT_TRUE(vc::readFile(project->getAssetsPath() / path, onDisk, error)) << error;
    EXPECT_EQ(vc::hashTarget(onDisk.targets[0]), hashOf(box)) << "the debounced write landed";
}

// The editor's quit flushes the queue on the main thread: the drain waits on the one-at-a-time
// io-write channel, and the job system's shutdown cancels a drain still pending there (V1 R8)
TEST_F(VertexColorEditing, QuitFlushWritesWhatWaitsBehindABusyWriteChannel) {
    const Entity box = addBox();
    ASSERT_TRUE(edit(box, {fillAll(kRed)}).ok);
    const std::string path = colorsOf(box)->data.path;
    ASSERT_TRUE(project->saveSceneNow(sceneId));

    std::atomic<bool> release{false};
    jobs::SubmitOptions options;
    options.name = "vertex-colors-busy-channel";
    jobs::Task<void> busy = jobs::system().ioWriteChannel().submit([&release]() {
        while (!release.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }, options);
    ASSERT_TRUE(edit(box, {fillAll(kBlue)}).ok);
    const bool flushed = flushVertexColorFiles();
    vc::Data onDisk;
    std::string error;
    const bool read = vc::readFile(project->getAssetsPath() / path, onDisk, error);
    release = true;
    busy.wait();

    EXPECT_TRUE(flushed);
    ASSERT_TRUE(read) << error;
    ASSERT_EQ(onDisk.targets.size(), 1u);
    EXPECT_EQ(vc::hashTarget(onDisk.targets[0]), hashOf(box)) << "the blue fill reached the file while the channel was busy";
}

TEST_F(VertexColorEditing, ChangedMeshIsTransferredAndTheFileWaitsForTheSave) {
    const Entity grid = addGrid("Floor", 4);
    ASSERT_TRUE(edit(grid, {fillAll(kRed)}).ok);
    ASSERT_TRUE(project->saveSceneNow(sceneId));
    const std::string path = colorsOf(grid)->data.path;
    const std::vector<uint8_t> saved = fileBytes(project->getAssetsPath() / path);
    sceneProject->isModified = false;

    // a reimport with another topology: the same surface in more segments
    Primitive(scene(), grid).createGrid(2.0f, 2.0f, 6);
    tickVertexColors(project.get());
    const std::vector<vce::RemapIssue> issues = vce::remapIssues(project.get(), sceneId);
    ASSERT_EQ(issues.size(), 1u);
    EXPECT_EQ(issues[0].entity, grid);
    EXPECT_FALSE(issues[0].unmatched);
    EXPECT_TRUE(sceneProject->isModified);
    const vc::TargetData* data = vce::targetData(project.get(), sceneId, grid, firstKey(grid));
    ASSERT_NE(data, nullptr);
    const std::shared_ptr<const vc::MeshView> mesh = vce::meshView(project.get(), sceneId, grid, firstKey(grid));
    ASSERT_NE(mesh, nullptr);
    EXPECT_EQ(data->fingerprint, mesh->fingerprint());
    for (const Vector4& value : data->base) EXPECT_NEAR(value.x, 1.0f, 1e-3f);
    vce::detail::flushWrites();
    EXPECT_EQ(fileBytes(project->getAssetsPath() / path), saved) << "the old file stays until the save (R10)";
    ASSERT_TRUE(project->saveSceneNow(sceneId));
    EXPECT_NE(fileBytes(project->getAssetsPath() / path), saved);
}

// R10, R60: the hard edges go with their faces through a reimport. The file keeps the triangles of a
// target with overrides; each new corner at an old vertex takes the old corner on its side
TEST_F(VertexColorEditing, ChangedMeshKeepsItsHardEdges) {
    const Entity grid = addGrid("Floor", 4);
    ASSERT_TRUE(edit(grid, {fillAll(kBlue), leftFaces(grid, kRed)}).ok);
    ASSERT_TRUE(vce::targetData(project.get(), sceneId, grid, firstKey(grid))->hasOverrides());
    // the file as a scene save writes it (the snapshot of the mesh, then the editor section), and the
    // values as the next open reads them: from that file
    vc::Data& live = *colorsOf(grid)->loaded;
    vce::detail::refreshSnapshots(project.get(), sceneId, grid, live);
    std::string error;
    ASSERT_TRUE(vc::writeFile(project->getAssetsPath() / colorsOf(grid)->data.path, live, true, error)) << error;
    vce::detail::resetFiles();
    colorsOf(grid)->loaded.reset();

    // a reimport: the same surface in 8 segments, every old vertex among the new ones
    Primitive(scene(), grid).createGrid(2.0f, 2.0f, 8);
    tickVertexColors(project.get());
    const std::vector<vce::RemapIssue> issues = vce::remapIssues(project.get(), sceneId);
    ASSERT_EQ(issues.size(), 1u);
    EXPECT_FALSE(issues[0].unmatched);
    expectSides(grid);
}

// R60: transfer from another GameObject carries its hard edges too (vce::transferSource)
TEST_F(VertexColorEditing, TransferFromAnotherObjectKeepsItsHardEdges) {
    const Entity painted = addGrid("Painted", 4);
    ASSERT_TRUE(edit(painted, {fillAll(kBlue), leftFaces(painted, kRed)}).ok);
    const Entity copy = addGrid("Copy", 8);
    std::string error;
    vc::Op transfer;
    transfer.type = vc::OpType::Transfer;
    transfer.source = vce::transferSource(project.get(), sceneId, painted, &error);
    ASSERT_NE(transfer.source, nullptr) << error;
    transfer.maxDistance = 0.01f;
    ASSERT_TRUE(edit(copy, {transfer}).ok);
    expectSides(copy);
}

TEST_F(VertexColorEditing, ElementSelectionChangesAreUndoSteps) {
    const Entity box = addBox();
    const vc::TargetKey key = firstKey(box);
    vce::ElementSelection selected;
    selected.entity = box;
    selected.key = key;
    selected.selection.kind = vc::ElementKind::Face;
    selected.selection.ids = {0, 1};
    vce::setElementSelection(project.get(), sceneId, {selected}, "Select 2 Faces");
    vc::Target target;
    ASSERT_TRUE(vce::selectionTarget(sceneId, box, key, target));
    EXPECT_EQ(target.kind, vc::TargetKind::Faces);
    EXPECT_EQ(target.ids, (std::vector<uint32_t>{0, 1}));
    EXPECT_EQ(history->top()->name(), "Select 2 Faces");
    EXPECT_FALSE(sceneProject->isModified) << "a selection is no scene change";
    ASSERT_TRUE(history->undo());
    EXPECT_FALSE(vce::selectionTarget(sceneId, box, key, target));
    ASSERT_TRUE(history->redo());
    EXPECT_TRUE(vce::selectionTarget(sceneId, box, key, target));
}

TEST_F(VertexColorEditing, VertexBlendMaterialStartsBlack) {
    const Entity box = addBox();
    MeshComponent& mesh = scene()->getComponent<MeshComponent>(box);
    mesh.submeshes[0].material.shader = "library:vertex_blend";
    std::string error;
    ASSERT_TRUE(vce::ensureComponent(project.get(), sceneId, box, &error)) << error;
    EXPECT_EQ(colorsOf(box)->initialValue, vc::InitialValue::Black) << "white would mix every layer at once";
    const vc::TargetData data = vce::targetDataOrInitial(project.get(), sceneId, box, firstKey(box));
    ASSERT_FALSE(data.base.empty());
    EXPECT_EQ(data.base[0], Vector4(0.0f, 0.0f, 0.0f, 1.0f));
    ASSERT_TRUE(history->undo());
    ASSERT_EQ(colorsOf(box), nullptr) << "one step";

    // five layers: A weighs the fifth, so the start is transparent black
    mesh.submeshes[0].material.shaderFeatures = {"LAYERS_FIVE"};
    ASSERT_TRUE(vce::ensureComponent(project.get(), sceneId, box, &error)) << error;
    EXPECT_EQ(colorsOf(box)->initialValue, vc::InitialValue::Custom);
    EXPECT_EQ(colorsOf(box)->customInitial, Vector4(0.0f, 0.0f, 0.0f, 0.0f));

    // a Lit box keeps the asset's colors
    const Entity lit = addBox("Lit");
    ASSERT_TRUE(vce::ensureComponent(project.get(), sceneId, lit, &error)) << error;
    EXPECT_EQ(colorsOf(lit)->initialValue, vc::InitialValue::Asset);
}

TEST_F(VertexColorEditing, PerTargetOpsRunEachTargetsOwnListInOneStep) {
    const Entity red = addBox("Red");
    const Entity blue = addBox("Blue");
    vce::EditRequest request;
    request.targets = {{red, firstKey(red)}, {blue, firstKey(blue)}};
    request.perTargetOps = {{fillAll(kRed)}};
    vce::EditResult result = vce::applyEdit(project.get(), sceneId, request);
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("perTargetOps"), std::string::npos) << result.error;

    const size_t before = steps();
    request.perTargetOps = {{fillAll(kRed)}, {fillAll(kBlue)}};
    request.undoName = "Apply Color to 2 Objects";
    result = vce::applyEdit(project.get(), sceneId, request);
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(steps(), before + 1);
    EXPECT_EQ(vce::targetData(project.get(), sceneId, red, firstKey(red))->base[0], kRed);
    EXPECT_EQ(vce::targetData(project.get(), sceneId, blue, firstKey(blue))->base[0], kBlue);
    ASSERT_TRUE(history->undo());
    EXPECT_EQ(colorsOf(red), nullptr);
    EXPECT_EQ(colorsOf(blue), nullptr);
}

TEST_F(VertexColorEditing, LiftedStrokeSweepsNoCapsuleAcrossTheGap) {
    const Entity grid = addGrid("Floor", 8);
    const vc::TargetKey key = firstKey(grid);
    const std::shared_ptr<const vc::MeshView> mesh = vce::meshView(project.get(), sceneId, grid, key);
    ASSERT_NE(mesh, nullptr);
    auto dab = [](const Vector3& at) {
        vc::Op op = strokeThrough(at, at, 0, 1.0f, false);
        op.points = {{at, 1.0f}};
        return op;
    };
    uint32_t centre = 0;
    for (uint32_t v = 0; v < mesh->vertexCount(); v++) {
        if (std::fabs(mesh->positions[v].x) < 1e-4f && std::fabs(mesh->positions[v].z) < 1e-4f) centre = v;
    }
    for (const bool lifted : {false, true}) {
        vce::StrokeSession stroke;
        std::string error;
        ASSERT_TRUE(stroke.begin(project.get(), sceneId, {{grid, key}}, "Paint Vertex Colors", &error)) << error;
        ASSERT_TRUE(stroke.apply(dab(Vector3(-0.9f, 0.0f, 0.0f))));
        if (lifted) stroke.lift();
        ASSERT_TRUE(stroke.apply(dab(Vector3(0.9f, 0.0f, 0.0f))));
        const vc::TargetData* data = vce::targetData(project.get(), sceneId, grid, key);
        ASSERT_NE(data, nullptr);
        // swept, the capsule from one dab to the other paints the centre; lifted, nothing does
        if (lifted) EXPECT_NEAR(data->base[centre].x, 1.0f, 1e-3f);
        else EXPECT_NEAR(data->base[centre].x, 0.0f, 1e-3f);
        stroke.cancel();
    }
}

} // namespace vertex_color_editing_test

}  // namespace VertexColorEditingTest_cpp
