// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef PROCEDURALBAKE_H
#define PROCEDURALBAKE_H

// The bake of a Procedural Material (docs/specs/S90-procedural-material.md R14-R16): ordinary PNGs and
// an ordinary .material, so a game never knows a graph existed. Shared by bake_procedural_material,
// check --materials, the Project window's badge, the Inspector's line and, later, the window's Bake
// button, the way Texture Synthesis shares SynthesisSession:
//
// - planBake(): what a bake draws (materialBakeRoots, the outputs asked for);
// - bakeInputHash(): what the result depends on. It hashes the plan, which is the graph's meaning with
//   every node's source resolved: positions and unconnected nodes do not count, a library update that
//   changes a node's code does. Tier and device are never in it, so a CPU bake is not stale against a
//   GPU bake;
// - encodeBake() + writeBakeUndoable(): the PNGs with their jasper.procmat recipe chunk, explicit
//   import settings in each .meta, and the .material, as ONE step of the project history. A re-bake
//   changes only the slots it owns (and a slot's factor when it newly takes the slot);
// - bakeStatus(): a baked PNG's recipe against the graph as it is now.

#include "PassPlanner.h"
#include "ProceduralChecks.h"

#include "texsynth/SynthesisSession.h"
#include "util/ProjectChecks.h"

#include "json.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace jasperengine {
    struct Material;
}

namespace jasperengine::editor {
    class Project;
}

namespace jasperengine::editor::procmat {

    inline constexpr const char* BAKE_RECIPE_KEYWORD = "jasper.procmat";
    inline constexpr int BAKE_RECIPE_FORMAT = 1;
    // what a bake's pixels depend on besides the plan (its encoding, the passes it adds): a change to
    // either makes every earlier bake stale
    inline constexpr int BAKE_VERSION = 1;
    inline constexpr int BAKE_SIZE_MIN = 64;
    inline constexpr int BAKE_SIZE_MAX = 8192;
    inline constexpr int BAKE_SIZE_DEFAULT = 1024;

    // base_color, normal, orm, emission, height: R14's order
    const std::vector<std::string>& bakeOutputNames();

    struct BakeOptions {
        int size = BAKE_SIZE_DEFAULT;
        uint32_t seed = 0;
        // the outputs to bake; empty: every one the graph has
        std::vector<std::string> outputs;
    };

    // The seed a graph keeps in its metadata.seed (the Procedural Material window's lock, reroll and
    // Variations write it): what a bake and a preview use when not told another; 0 when it has none
    uint32_t graphSeed(const graph::Graph& graph);

    // A .procmat read for an evaluation: of its kind and without error issues (each listed with its fix)
    bool readGraphForBake(const std::filesystem::path& file, const std::string& rel, graph::Graph& graph, std::string& error);

    // The names of materialBakeRoots, in R14's order: what a bake of the whole graph writes
    std::vector<std::string> graphBakeOutputs(const graph::Graph& graph);

    // The plan of a bake (read back, Buffers at the sizes the graph asks) and the outputs it draws, in
    // plan order. False with why: an invalid size, an output the graph does not have, a graph that does
    // not plan.
    bool planBake(const graph::Graph& graph, const std::shared_ptr<const NodeLibrary>& library, const BakeOptions& options, PassPlan& plan,
                  std::vector<std::string>& outputs, std::string& error);
    // 64 hex digits
    std::string bakeInputHash(const PassPlan& plan);

    // How one PNG of a bake was made: its jasper.procmat chunk
    struct BakeRecipe {
        std::string graphGuid;
        std::string graphPath;      // project-relative, when it was baked
        std::string inputHash;
        uint32_t seed = 0;
        int size = 0;
        std::string output;         // which output this file is
        std::vector<std::string> outputs;   // every output baked with it
        // every output the graph had (materialBakeRoots): a bake of some of them is not stale for the others
        std::vector<std::string> graphOutputs;
        // the seed was the graph's metadata.seed, not one the call gave: a reroll makes the bake stale
        bool seedFromGraph = false;
        std::string tier;
        std::string device;
    };
    nlohmann::json recipeToJson(const BakeRecipe& recipe);
    // false with why on a garbled recipe (another writer, a hand edit)
    bool recipeFromJson(const nlohmann::json& json, BakeRecipe& recipe, std::string& error);
    // the recipe of a PNG, nullopt when it has none (error says why when it has a garbled one)
    std::optional<BakeRecipe> readBakeRecipe(const std::filesystem::path& png, std::string* error = nullptr);

    // Where a bake goes: a sibling folder named after the graph (assets/mats/stone.procmat ->
    // assets/mats/stone), files named after it too (stone_base_color.png, stone.material)
    std::string defaultBakeFolder(const std::string& graphPath);
    std::string bakeStem(const std::string& graphPath);
    std::string bakedImageName(const std::string& stem, const std::string& output);
    std::string bakedMaterialName(const std::string& stem);

    // A material after a bake into it: every baked output's slot points at its PNG (asset path and
    // GUID), and a slot's factor goes to 1 when the slot newly takes the bake (a factor someone tuned
    // on a baked slot stays). `dropped`: outputs the graph no longer has; a slot holding one is emptied,
    // and an emptied emission slot's factor goes to 0, since without its texture the factor alone glows.
    // Every other field is left as it was.
    struct BakedSlot {
        std::string output;
        std::string assetPath;      // assets-relative, as a .material stores it
        std::string guid;
    };
    void applyBakeToMaterial(Material& material, const std::vector<BakedSlot>& baked, const std::vector<BakedSlot>& dropped);

    // The .meta of a baked PNG: its GUID (kept when it has one) and explicit texture import settings
    // (R15), every other section and import key as they were. `existing` is the meta's text, or empty.
    std::string bakedMetaText(const std::string& existing, const std::string& guid, const std::string& output);
    // the GUID of an existing meta's text, empty when it has none
    std::string metaGuid(const std::string& existing);

    // Everything one bake writes, ready for writeBakeUndoable
    struct BakeWrite {
        std::string graphPath;      // project-relative
        std::string folder;         // project-relative, under the assets directory
        BakeRecipe recipe;          // the bake's (output and outputs filled per file)
        std::vector<OutputImage> images;
        // every output the graph has (graphBakeOutputs): a slot of one of them that this bake leaves out
        // keeps its earlier bake
        std::vector<std::string> graphOutputs;
        // replace a PNG at an output's path that is not an earlier bake of this graph
        bool overwrite = false;
    };
    struct BakeFiles {
        std::vector<texsynth::OutputFile> files;
        std::vector<std::string> images;    // project-relative, in output order
        std::string material;               // project-relative
    };
    bool encodeBake(Project* project, const BakeWrite& bake, BakeFiles& out, std::string& error);
    // One step of the project history named "Bake Procedural Material <stem>" (undo removes every file
    // it created and puts back what it replaced); every write stays beneath `beneath` when it is given
    bool writeBakeUndoable(Project* project, const BakeFiles& files, const std::string& stem, std::string& error,
                           const std::filesystem::path& beneath = {});

    enum class BakeState { Fresh, Stale, GraphMissing, GraphInvalid };
    const char* bakeStateName(BakeState state);   // fresh, stale, graph_missing, graph_invalid

    struct BakeStatus {
        BakeState state = BakeState::Fresh;
        std::string graphPath;      // project-relative, where the graph is now
        std::string reason;         // not fresh: why, in a sentence
        BakeRecipe recipe;
    };
    // The graph a recipe names: by GUID among the project's .procmat files, else at its recorded path;
    // empty when neither exists
    std::filesystem::path findBakeGraph(const std::filesystem::path& projectDir, const BakeRecipe& recipe);
    // A recipe against the graph as it is now (R16): only an input change makes it stale
    BakeStatus bakeStatus(const std::filesystem::path& projectDir, const BakeRecipe& recipe);
    // For the Project window and the Inspector: the status of a PNG, nullopt when it is no bake. Kept
    // until the PNG or its graph change on disk. Main thread.
    std::optional<BakeStatus> cachedBakeStatus(const std::filesystem::path& projectDir, const std::filesystem::path& png);

    // preview_procedural_material's report on a graph's images, the rows the Procedural Material window's
    // Problems strip shows (its Services::checks): ProceduralChecks::report with the project's thresholds,
    // this editor session's FAIL rounds by path (read, not recorded: the tools record them), and height_8bit's
    // reader from the bake's own .material. `only`: the check ids, empty for all.
    qa::Json graphCheckReport(Project* project, const std::string& path, const graph::Graph& graph, const EvaluationResult& images,
                              const std::vector<std::string>& only = {});
    // The single edits of the graph the rows point at (R27's Fix: `fixes` [{check, node, op}], and the row's
    // limited_by and fix_hint name the node): a tileability FAIL on an output fed through a repeat count with a
    // fraction (a scale the descriptor steps by 1 from 1, as perlin's Scale X; setParam to the nearest whole one); a normal row that fails or warns while
    // Normal is connected and Height is too (disconnect: the bake then makes the normal from the height)
    void attachGraphFixes(qa::Json& report, const graph::Graph& graph, const std::shared_ptr<const NodeLibrary>& library);
    // the consecutive FAILs of each check, by the material's path, this editor session. Main thread.
    qa::FailRounds& checkRounds();
    // "the shader texture <uniform>" of the bake's .material beside the graph that samples a baked height;
    // empty when none does
    std::string bakedHeightReader(Project* project, const std::string& graphPath);

    // check --materials (R39), compiler style and without the engine: each graph's problems (its issues,
    // and a graph that does not plan) and each baked PNG that is stale or whose graph is gone. `files`:
    // only these (a .procmat, and the PNGs baked from it; a PNG, its recipe); empty: the whole project.
    struct MaterialsCheck {
        std::vector<ProjectChecks::Finding> findings;
        size_t graphs = 0;
        size_t bakes = 0;
    };
    MaterialsCheck checkMaterials(const std::filesystem::path& projectDir, const std::vector<std::filesystem::path>& files);

}

#endif //PROCEDURALBAKE_H
