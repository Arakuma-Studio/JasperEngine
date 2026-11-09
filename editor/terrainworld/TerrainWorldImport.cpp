// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "terrainworld/TerrainWorldImport.h"

#include "EditorHost.h"
#include "Project.h"
#include "Scene.h"
#include "command/CommandHandle.h"
#include "command/CommandHistory.h"
#include "command/type/TerrainWorldImportCmd.h"
#include "io/AtomicFile.h"
#include "jobs/JobSystem.h"
#include "subsystem/TerrainWorldRuntime.h"
#include "subsystem/TerrainWorldSystem.h"
#include "terrain/TerrainRawIO.h"
#include "terrainworld/TerrainWorldFiles.h"
#include "util/FileUtils.h"

#include "tinyexr.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iterator>
#include <set>
#include <string_view>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::terraincore;

namespace fs = std::filesystem;

namespace {

    std::string importExtension(const fs::path& file) {
        std::string extension = FileUtils::pathToUtf8(file.extension());
        if (!extension.empty() && extension[0] == '.') extension.erase(0, 1);
        for (char& c : extension) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return extension;
    }

    std::string importTileName(TileCoord tile) {
        return "(" + std::to_string(tile.x) + ", " + std::to_string(tile.z) + ")";
    }

    // A PNG's size from its IHDR chunk
    bool pngSize(const fs::path& file, int& width, int& height) {
        std::ifstream in(file, std::ios::binary);
        unsigned char head[24] = {};
        if (!in.read(reinterpret_cast<char*>(head), sizeof(head))) return false;
        static const unsigned char signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
        if (!std::equal(signature, signature + 8, head) || std::string(reinterpret_cast<char*>(head + 12), 4) != "IHDR") return false;
        auto be32 = [&](int at) { return (static_cast<uint32_t>(head[at]) << 24) | (static_cast<uint32_t>(head[at + 1]) << 16) | (static_cast<uint32_t>(head[at + 2]) << 8) | head[at + 3]; };
        width = static_cast<int>(std::min<uint32_t>(be32(16), 1u << 30));
        height = static_cast<int>(std::min<uint32_t>(be32(20), 1u << 30));
        return true;
    }

    // An EXR's size from its header (the first MiB holds it)
    bool exrSize(const fs::path& file, int& width, int& height) {
        std::ifstream in(file, std::ios::binary);
        std::vector<unsigned char> bytes(1u << 20);
        in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        bytes.resize(static_cast<size_t>(in.gcount()));
        EXRVersion version;
        if (bytes.empty() || ParseEXRVersionFromMemory(&version, bytes.data(), bytes.size()) != TINYEXR_SUCCESS || version.multipart) return false;
        EXRHeader header;
        InitEXRHeader(&header);
        const char* message = nullptr;
        const bool ok = ParseEXRHeaderFromMemory(&header, &version, bytes.data(), bytes.size(), &message) == TINYEXR_SUCCESS;
        if (ok) {
            width = header.data_window.max_x - header.data_window.min_x + 1;
            height = header.data_window.max_y - header.data_window.min_y + 1;
        }
        if (message) FreeEXRErrorMessage(message);
        FreeEXRHeader(&header);
        return ok;
    }

    // The sides a raw of these samples has: given, one given, else square
    bool rawSides(uintmax_t samples, int givenWidth, int givenHeight, int& width, int& height) {
        if (givenWidth > 0 && givenHeight > 0) {
            width = givenWidth;
            height = givenHeight;
            return static_cast<uintmax_t>(width) * static_cast<uintmax_t>(height) == samples;
        }
        if (givenWidth > 0) {
            if (samples % static_cast<uintmax_t>(givenWidth) != 0) return false;
            width = givenWidth;
            height = static_cast<int>(std::min<uintmax_t>(samples / static_cast<uintmax_t>(givenWidth), 1u << 30));
            return true;
        }
        const uintmax_t side = static_cast<uintmax_t>(std::llround(std::sqrt(static_cast<double>(samples))));
        for (uintmax_t s = side > 0 ? side - 1 : 0; s <= side + 1; s++) {
            if (s * s != samples || s == 0) continue;
            width = height = static_cast<int>(std::min<uintmax_t>(s, 1u << 30));
            return true;
        }
        return false;
    }

    // The import's own folder under the project's trash, which the step keeps for undo
    fs::path importStash(Project* project, const std::string& guid) {
        static std::atomic<uint32_t> counter{0};
        const long long stamp = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        return project->getProjectInternalPath() / "trash" / "terrain-world-import" /
               (guid + "-" + std::to_string(stamp) + "-" + std::to_string(counter.fetch_add(1)));
    }

    // What the job carries: copies, never the runtime
    struct ImportWork {
        TerrainWorldImportPlan plan;
        WorldLattice lattice;
        fs::path folder;
        fs::path stash;
        std::set<uint32_t> present;
        uint16_t baseHeight = 0;
        std::vector<float> layerColours;
        int layerCount = 0;
        TileCodec codec;
        std::shared_ptr<TerrainWorldImportTask> task;
        TerrainWorldImportResult result;
    };

    void runImport(ImportWork& work) {
        TerrainWorldImportTask& task = *work.task;
        const TerrainWorldImportPlan& plan = work.plan;
        const std::string path = FileUtils::pathToUtf8(plan.file);
        auto cancelled = [&task]() { return task.cancelRequested(); };
        HeightmapImport slicing;
        slicing.lattice = work.lattice;
        slicing.origin = plan.origin;
        HeightmapRows rows;
        std::shared_ptr<HeightImport> whole;
        std::string error;
        if (plan.banded) {
            // T1's Auto range and byte order, found in a pass a row at a time
            task.report(0.0, "Reading the file's range");
            RawHeightmapScan scan;
            if (!scanRawHeightmap(path, plan.width, plan.height, cancelled, scan, error)) {
                work.result.cancelled = error == "cancelled";
                work.result.error = error;
                return;
            }
            RawHeightmapFile raw;
            raw.path = path;
            raw.width = plan.width;
            raw.height = plan.height;
            raw.bigEndian = scan.bigEndian();
            raw.low = raw.bigEndian ? scan.minBig : scan.minLittle;
            raw.high = raw.bigEndian ? scan.maxBig : scan.maxLittle;
            rows = rawHeightmapRows(raw);
            slicing.flipZ = plan.flipVertically;
        } else {
            task.report(-1.0, "Decoding the file");
            std::ifstream file(plan.file, std::ios::binary);
            const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            HeightImportOptions options;
            options.width = plan.width;
            options.height = plan.height;
            options.flipVertically = plan.flipVertically;
            if (plan.bytesPerSample == 1) options.format = HeightFileFormat::R8;
            else if (plan.bytesPerSample == 4) options.format = HeightFileFormat::R32F;
            whole = std::make_shared<HeightImport>(TerrainRawIO::decode(path, bytes, options));
            if (!whole->ok) {
                work.result.error = whole->error;
                return;
            }
            if (whole->width != plan.width || whole->height != plan.height) {
                work.result.error = "the file changed while it was read: import it again";
                return;
            }
            rows.width = whole->width;
            rows.height = whole->height;
            rows.read = [whole](int row, std::vector<uint16_t>& out) {
                const float* source = whole->normalized.data() + static_cast<size_t>(row) * static_cast<size_t>(whole->width);
                for (int i = 0; i < whole->width; i++) out[static_cast<size_t>(i)] = static_cast<uint16_t>(std::lround(std::clamp(source[i], 0.0f, 1.0f) * 65535.0f));
                return true;
            };
        }
        if (!rows.read) {
            work.result.error = "the file could not be opened";
            return;
        }

        ImportTarget target;
        target.present = [&work](TileCoord tile) { return work.present.count(tile.key()) > 0; };
        target.read = [&work](TileCoord tile) -> std::shared_ptr<WorldTile> {
            std::ifstream file(work.folder / FileUtils::pathFromUtf8(terrainWorldTileName(tile)), std::ios::binary);
            const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            auto data = std::make_shared<WorldTile>();
            if (bytes.empty() || decodeTile(bytes, work.codec, *data) != TileError::None || data->coord != tile || data->height.empty()) return nullptr;
            return data;
        };
        target.write = [&work, &error](const WorldTile& tile) {
            std::vector<uint8_t> bytes;
            TileEncodeOptions options;
            options.level = 9;
            if (!encodeTile(tile, work.codec, options, bytes, &error)) return false;
            io::AtomicWriteOptions write;
            write.durability = io::Durability::Cache;
            const io::WriteResult result = io::writeFileAtomic(work.stash / "after" / FileUtils::pathFromUtf8(terrainWorldTileName(tile.coord)),
                                                               std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), write);
            if (!result.ok()) error = io::describeWriteError(result);
            return result.ok();
        };
        target.baseHeight = work.baseHeight;
        target.layerColours = work.layerColours;
        target.layerCount = work.layerCount;
        task.report(0.0, "Writing tiles");
        const bool ok = importHeightmap(rows, slicing, target, cancelled, [&task](int done, int total) {
            task.report(total > 0 ? static_cast<double>(done) / static_cast<double>(total) : 1.0, "Writing tiles " + std::to_string(done) + " of " + std::to_string(total));
        }, error);
        work.result.cancelled = !ok && error == "cancelled";
        work.result.error = ok ? std::string() : error;
        work.result.ok = ok;
    }

}

std::string TerrainWorldImportTask::phase() const {
    std::lock_guard<std::mutex> guard(mutex);
    return phaseText;
}

void TerrainWorldImportTask::report(double value, const std::string& text) {
    progressValue.store(value);
    std::lock_guard<std::mutex> guard(mutex);
    phaseText = text;
}

bool jasperengine::editor::planTerrainWorldImport(Project* project, TerrainWorldRuntime& runtime, const TerrainWorldImportRequest& request,
                                                  TerrainWorldImportPlan& plan, std::string& error) {
    plan = TerrainWorldImportPlan();
    if (request.path.empty()) {
        error = "pick a heightmap file (.r16, .raw, .png or .exr)";
        return false;
    }
    std::error_code ec;
    plan.file = project->resolveAssetPath(FileUtils::pathFromUtf8(request.path));
    if (!fs::is_regular_file(plan.file, ec)) {
        error = "\"" + request.path + "\" is not a file of the project";
        return false;
    }
    plan.flipVertically = request.flipVertically;
    plan.origin = request.origin;
    const std::string extension = importExtension(plan.file);
    if (extension == "png" || extension == "exr") {
        if (!(extension == "png" ? pngSize(plan.file, plan.width, plan.height) : exrSize(plan.file, plan.width, plan.height))) {
            error = "\"" + request.path + "\" is not a readable " + (extension == "png" ? std::string("PNG") : std::string("EXR"));
            return false;
        }
        plan.bytesPerSample = 0;
    } else {
        // T1's depths: the extension's, a .raw 16-bit unless only 8-bit fits its length
        const uintmax_t length = fs::file_size(plan.file, ec);
        plan.bytesPerSample = extension == "r8" ? 1 : (extension == "r32" || extension == "f32") ? 4 : 2;
        auto fits = [&](int bytes) {
            return length % static_cast<uintmax_t>(bytes) == 0 && rawSides(length / static_cast<uintmax_t>(bytes), request.width, request.height, plan.width, plan.height);
        };
        bool sized = fits(plan.bytesPerSample);
        if (!sized && extension == "raw" && fits(1)) {
            plan.bytesPerSample = 1;
            sized = true;
        }
        if (!sized) {
            error = "the file's " + std::to_string(length) + " bytes are no " +
                    (request.width > 0 ? std::to_string(request.width) + " x " + (request.height > 0 ? std::to_string(request.height) : std::string("n")) : std::string("square")) +
                    " map of " + std::to_string(plan.bytesPerSample * 8) + "-bit samples: give width and height";
            return false;
        }
        plan.sizeGuessed = request.width <= 0 || request.height <= 0;
        plan.banded = plan.bytesPerSample == 2;
    }
    if (plan.width < 2 || plan.height < 2) {
        error = "the file holds fewer than 2 x 2 samples";
        return false;
    }
    if (!plan.banded && (plan.width > TERRAIN_WORLD_IMPORT_WHOLE_SIDE || plan.height > TERRAIN_WORLD_IMPORT_WHOLE_SIDE)) {
        error = "the file's " + std::to_string(plan.width) + " x " + std::to_string(plan.height) + " samples are past the " +
                std::to_string(TERRAIN_WORLD_IMPORT_WHOLE_SIDE) + " a side a decoded file may have: save it as a 16-bit .r16 or .raw, which is read in bands";
        return false;
    }
    HeightmapImport slicing;
    slicing.lattice = runtime.lattice();
    slicing.origin = plan.origin;
    plan.covered = importCoveredTiles(slicing, plan.width, plan.height);
    TileCoord low, high;
    bool box = runtime.bounds(low, high);
    for (const TileCoord& tile : plan.covered) {
        std::string why;
        if (!tile.addressable() || !tileWithinLimit(runtime.lattice(), tile)) why = "is past the world's limit of +-16384 m";
        if (why.empty() && !runtime.present(tile)) {
            plan.made++;
            low = box ? TileCoord(std::min<int>(low.x, tile.x), std::min<int>(low.z, tile.z)) : tile;
            high = box ? TileCoord(std::max<int>(high.x, tile.x), std::max<int>(high.z, tile.z)) : tile;
            box = true;
            const int most = runtime.store().options().maxBoxTiles;
            if (high.x - low.x + 1 > most || high.z - low.z + 1 > most) why = "would make the world span more than " + std::to_string(most) + " tiles a side";
        }
        if (!why.empty()) {
            error = "the file's " + std::to_string(plan.width) + " x " + std::to_string(plan.height) + " samples from tile " + importTileName(plan.origin) +
                    " reach tile " + importTileName(tile) + ", which " + why + ": move the origin or crop the file";
            return false;
        }
    }
    plan.changed = importChangedTiles(slicing, plan.width, plan.height, [&runtime](TileCoord tile) { return runtime.present(tile); });
    return true;
}

std::shared_ptr<TerrainWorldImportTask> jasperengine::editor::startTerrainWorldImport(Project* project, uint32_t sceneId, Entity world,
                                                                                    const TerrainWorldImportPlan& plan,
                                                                                    std::function<void(const TerrainWorldImportResult&)> done) {
    auto task = std::make_shared<TerrainWorldImportTask>();
    TerrainWorldImportResult failed;
    SceneProject* sceneProject = project ? project->getScene(sceneId) : nullptr;
    TerrainWorldSystem* system = sceneProject && sceneProject->scene ? sceneProject->scene->findSystem<TerrainWorldSystem>().get() : nullptr;
    TerrainWorldRuntime* runtime = system ? system->ensureRuntime(world, &failed.error) : nullptr;
    auto* source = runtime ? dynamic_cast<EditorWorldSource*>(&runtime->source()) : nullptr;
    if (!runtime || !source || runtime->playing()) {
        if (failed.error.empty()) failed.error = runtime && runtime->playing() ? "a Terrain World's tiles change only outside Play: stop Play first"
                                                                               : "the Terrain World is not running";
        task->finish();
        done(failed);
        return task;
    }
    // the job reads the tile files: edits still in memory go to them first, and a stamp of each lets
    // the swap see an edit made while the job ran
    TerrainWorldFiles::flushWorld(*runtime);
    auto work = std::make_shared<ImportWork>();
    work->plan = plan;
    work->lattice = runtime->lattice();
    work->folder = source->folder();
    work->stash = importStash(project, runtime->manifest().guid);
    work->baseHeight = runtime->manifest().maps.baseHeight;
    work->layerColours = runtime->getLayerColours();
    work->layerCount = runtime->getLayerCount();
    work->codec = terrainWorldEditorCodec();
    work->task = task;
    auto stamps = std::make_shared<std::vector<TerrainWorldImportCmd::Stamp>>();
    // a changed tile's gutter may hold a sample of the tile beyond it: two rings round what it covers
    for (const TileCoord& tile : plan.covered) {
        for (int dz = -2; dz <= 2; dz++) {
            for (int dx = -2; dx <= 2; dx++) {
                const TileCoord near(tile.x + dx, tile.z + dz);
                if (near.addressable() && runtime->present(near)) work->present.insert(near.key());
            }
        }
    }
    for (const TileCoord& tile : plan.changed) stamps->push_back(TerrainWorldImportCmd::stampOf(work->folder / FileUtils::pathFromUtf8(terrainWorldTileName(tile))));
    auto finish = [project, sceneId, world, work, stamps, done]() {
        TerrainWorldImportResult result = work->result;
        result.tiles = static_cast<int>(work->plan.covered.size());
        std::error_code ec;
        if (result.ok && work->task->cancelRequested()) {
            result.ok = false;
            result.cancelled = true;
        }
        if (!result.ok) {
            fs::remove_all(work->stash, ec);
            if (result.cancelled) result.error = "the import was cancelled; nothing was changed";
            work->task->finish();
            done(result);
            return;
        }
        CommandHistory* history = project->getScene(sceneId) ? CommandHandle::get(sceneId) : nullptr;
        auto reason = std::make_shared<std::string>();
        auto* command = new TerrainWorldImportCmd(project, sceneId, world, work->stash, work->plan.changed, *stamps, reason);
        if (!history || !history->addCommandNoMerge(command)) {
            if (!history) delete command;
            result.ok = false;
            result.error = reason->empty() ? std::string("the import could not be applied; nothing was changed") : *reason;
        } else {
            result.changed = static_cast<int>(work->plan.changed.size());
        }
        work->task->finish();
        done(result);
    };
    auto job = [work, finish]() {
        runImport(*work);
        getEditorHost().enqueueMainThreadTask(finish);
    };
    if (jobs::hasSystem()) {
        jobs::enqueueDetached(job, jobs::Lane::Background, "terrain-world-import");
    } else {
        job();
    }
    return task;
}
