// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ModelImporterView.h"

#include "Backend.h"
#include "EditorHost.h"
#include "Project.h"
#include "command/CommandHandle.h"
#include "command/CommandHistory.h"
#include "command/type/SetAssetMetaCmd.h"
#include "command/type/ScenePropertyCmd.h"
#include "external/IconsFontAwesome6.h"
#include "model/ModelSettingsEdit.h"
#include "texture/TextureCookQueue.h"
#include "util/FbxImporter.h"
#include "util/FileUtils.h"
#include "window/dialog/EditorSettingsWindow.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

    constexpr int kPreviewSize = 320;
    // the reference view the "about" screen heights assume (Unity's percentages have none)
    constexpr float kReferenceViewHeight = 1080.0f;

    // a colour-blind-safe ramp (Okabe-Ito): LOD 0, 1, 2, 3
    const ImU32 kLodColors[4] = {IM_COL32(0, 158, 115, 255), IM_COL32(86, 180, 233, 255), IM_COL32(230, 159, 0, 255), IM_COL32(204, 121, 167, 255)};

    std::string bytesText(uint64_t bytes){
        return ModelAudit::formatBytes(bytes);
    }

    std::string countText(long long value){
        std::string digits = std::to_string(value < 0 ? -value : value);
        for (int i = (int)digits.size() - 3; i > 0; i -= 3) digits.insert((size_t)i, ",");
        return (value < 0 ? "-" : "") + digits;
    }

    std::string savedText(double before, double after){
        if (before <= 0.0 || after >= before) return "-";
        char text[16];
        std::snprintf(text, sizeof(text), "%.0f%%", (1.0 - after / before) * 100.0);
        return text;
    }


}

struct ModelImporterView::PreviewJob {
    jobs::Task<std::shared_ptr<ModelPreviewGeometry>> geometry;
    jobs::Task<std::vector<uint8_t>> image;
    std::string error;
};

ModelImporterView::ModelImporterView(Project* project): project(project){
    ImporterPrompt::add(this, {[this](){ return hasPendingEdits(); },
                               [this](){ return assetPaths.size() == 1 ? fs::path(assetPaths[0]).filename().string()
                                                                      : std::to_string(assetPaths.size()) + " models"; },
                               [this](){ apply(); }, [this](){ revert(); }});
}

ModelImporterView::~ModelImporterView(){
    ImporterPrompt::remove(this);
    previewTexture.destroy();
}

ModelFamily ModelImporterView::shownFamily() const{
    return tab > 0 && tab <= (int)kModelFamilyCount ? (ModelFamily)(tab - 1) : ModelFamily::Desktop;
}

ModelImportSettings ModelImporterView::workingSettings() const{
    ModelImportSettings settings;
    ModelImportSettings::parse(form.working, settings);
    return settings;
}

void ModelImporterView::select(const std::vector<fs::path>& selection){
    std::vector<std::string> assets;
    for (const fs::path& path : selection){
        const fs::path absolute = (project->getProjectPath() / path).lexically_normal();
        if (!project->isInsideAssetsPath(absolute))
            continue;
        const std::string assetPath = project->normalizeToAssetsRelative(absolute).generic_string();
        if (isModelSource(assetPath))
            assets.push_back(assetPath);
    }
    std::string key;
    for (const std::string& path : assets)
        key += path + "\n";
    // another selection with edits not applied: the prompt decides (S53 R17)
    if (key != selectionKey && hasPendingEdits()){
        if (key != promptedKey){
            promptedKey = key;
            ImporterPrompt::guard(nullptr);
        }
        return;
    }
    promptedKey.clear();
    // the stored settings changed under us (undo, an agent, a pull): read them again
    bool changed = key != selectionKey;
    if (!changed && !hasPendingEdits()){
        for (size_t i = 0; i < assetPaths.size() && !changed; i++){
            const YAML::Node now = SetAssetMetaCmd::read(project->getAssetsPath() / FileUtils::pathFromUtf8(assetPaths[i]), ModelImportSettings::metaKey);
            changed = importer::dump(now && !now.IsNull() ? now : YAML::Node()) != importer::dump(stored[i]);
        }
    }
    if (!changed)
        return;
    if (key != selectionKey){
        tab = 0;
        pageTab = 0;
        geometryKey.clear();
        geometry.reset();
    }
    selectionKey = key;
    assetPaths = assets;
    stored.clear();
    for (const std::string& path : assetPaths){
        const YAML::Node section = SetAssetMetaCmd::read(project->getAssetsPath() / FileUtils::pathFromUtf8(path), ModelImportSettings::metaKey);
        stored.push_back(section && !section.IsNull() ? YAML::Clone(section) : YAML::Node());
    }
    std::vector<YAML::Node> sections;
    for (const YAML::Node& section : stored)
        sections.push_back(section ? section : YAML::Node(YAML::NodeType::Map));
    form.reset(sections);
    // the numbers of every platform of the first one, the shown platform's of the others
    ModelCookQueue& queue = ModelCookQueue::get();
    if (queue.getProject() == project){
        for (size_t i = 0; i < assetPaths.size(); i++)
            for (size_t f = 0; f < (i == 0 ? kModelFamilyCount : 1); f++)
                queue.request(assetPaths[i], (ModelFamily)f, ModelCookQueue::Selected);
    }
    refreshRows(true);
}

void ModelImporterView::refreshRows(bool force){
    const auto now = std::chrono::steady_clock::now();
    if (!force && rowsFamily == (int)shownFamily() && now - rowsAt < std::chrono::milliseconds(500))
        return;
    rowsAt = now;
    rowsFamily = (int)shownFamily();
    ModelUsageIndex::get().refresh(*project);
    rows.clear();
    for (const std::string& path : assetPaths)
        rows.push_back(ModelAudit::row(*project, path, shownFamily()));
}

void ModelImporterView::revert(){
    form.touched.clear();
    selectionKey.clear();
}

void ModelImporterView::apply(){
    if (!hasPendingEdits())
        return;
    std::vector<YAML::Node> sections;
    for (const YAML::Node& section : stored){
        ModelImportSettings parsed;
        std::string error;
        if (!ModelImportSettings::parse(form.merged(section), parsed, &error)){
            editor::getEditorHost().registerAlert("Model Import Settings", "Not applied: " + error);
            return;
        }
        sections.push_back(parsed.isDefault() ? YAML::Node() : parsed.serialize());
    }
    std::vector<std::string> changed;
    std::string error;
    if (!writeModelSections(*project, assetPaths, sections, "Set Model Import Settings", changed, error)){
        editor::getEditorHost().registerAlert("Model Import Settings", "Nothing was applied: " + error + ".");
        return;
    }
    form.touched.clear();
    selectionKey.clear();
}

void ModelImporterView::drawPreview(){
    const ModelAuditRow* row = rows.empty() ? nullptr : &rows[0];
    const bool cooked = showCooked && row && row->hasState && row->state.status == ModelCookStatus::Cooked;
    const fs::path absolute = project->getAssetsPath() / FileUtils::pathFromUtf8(assetPaths[0]);
    const fs::path source = FbxImporter::isFbxFile(absolute) ? project->getImportedModelPath(absolute) : absolute;
    std::error_code ec;
    const auto written = fs::last_write_time(source, ec);
    const std::string gKey = cooked ? "cooked|" + row->state.record.key
                                    : "source|" + source.generic_string() + "|" + std::to_string(ec ? 0 : (long long)written.time_since_epoch().count());
    if (gKey != geometryKey){
        geometryKey = gKey;
        imageKey.clear();
        auto job = std::make_shared<PreviewJob>();
        const fs::path file = cooked ? ModelCookQueue::get().getCache().artifactPath(row->state.record.key) : source;
        const fs::path baseDir = absolute.parent_path();
        jobs::SubmitOptions options;
        options.lane = jobs::Lane::Background;
        options.name = "model-preview";
        job->geometry = jobs::submit([job, file, baseDir](){
            auto out = std::make_shared<ModelPreviewGeometry>();
            GltfDocument doc;
            std::vector<uint8_t> bytes;
            std::ifstream in(file, std::ios::binary);
            bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            if (!doc.loadMemory(bytes, baseDir, file.filename().string(), job->error) || !loadPreviewGeometry(doc, *out, job->error))
                return std::shared_ptr<ModelPreviewGeometry>();
            return out;
        }, options);
        geometryJob = job;
    }
    if (geometryJob && geometryJob->geometry.isReady()){
        try { geometry = geometryJob->geometry.get(); } catch (const std::exception& e) { geometry.reset(); geometryJob->error = e.what(); }
        previewError = geometry ? std::string() : geometryJob->error;
        geometryJob.reset();
        imageKey.clear();
    }
    char key[128];
    std::snprintf(key, sizeof(key), "%.3f|%.3f|%d|%d|%p", yaw, pitch, wireframe ? 1 : 0, forceLod, (void*)geometry.get());
    if (geometry && key != imageKey && !imageJob){
        imageKey = key;
        auto job = std::make_shared<PreviewJob>();
        const std::shared_ptr<ModelPreviewGeometry> shown = geometry;
        const float y = yaw, p = pitch;
        const bool wire = wireframe;
        const int level = forceLod;
        jobs::SubmitOptions options;
        options.lane = jobs::Lane::Background;
        options.name = "model-preview";
        job->image = jobs::submit([shown, y, p, wire, level](){
            return rasterizePreview(*shown, kPreviewSize, kPreviewSize, y, p, wire, level);
        }, options);
        imageJob = job;
    }
    if (imageJob && imageJob->image.isReady()){
        std::vector<uint8_t> rgba;
        try { rgba = imageJob->image.get(); } catch (const std::exception&) { rgba.clear(); }
        imageJob.reset();
        if (!rgba.empty()){
            unsigned char* pixels = static_cast<unsigned char*>(std::malloc(rgba.size()));
            if (pixels){
                std::memcpy(pixels, rgba.data(), rgba.size());
                previewTexture.destroy();
                TextureData data(kPreviewSize, kPreviewSize, (unsigned int)rgba.size(), ColorFormat::RGBA, 4, pixels);
                data.setDataOwned(false);
                previewTexture = Texture("editor-model-importer-preview-" + std::to_string(++previewSerial), data);
                previewTexture.getData().setDataOwned(true);
                previewTexture.setMinFilter(TextureFilter::LINEAR);
                previewTexture.setMagFilter(TextureFilter::LINEAR);
                previewTexture.load();
            }
        }
    }
    const float avail = ImGui::GetContentRegionAvail().x;
    const float size = std::min(avail, 256.0f);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail - size) * 0.5f);
    TextureRender* render = previewTexture.getRender();
    if (render && geometry){
        // an item that takes the drag, the image drawn over it
        const ImVec2 at = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##model_preview", ImVec2(size, size));
        ImGui::GetWindowDrawList()->AddImage(Backend::getImGuiTexture(render), at, ImVec2(at.x + size, at.y + size));
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)){
            const ImVec2 delta = ImGui::GetIO().MouseDelta;
            yaw += delta.x * 0.01f;
            pitch = std::clamp(pitch + delta.y * 0.01f, -1.5f, 1.5f);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Drag to orbit. Drawn on the CPU from the %s file.", cooked ? "cooked" : "source");
    }else if (!previewError.empty()){
        ImGui::TextDisabled("%s", previewError.c_str());
    }else{
        ImGui::TextDisabled("Loading preview...");
    }
    static const char* const modes[] = {"Shaded", "Wireframe"};
    int mode = wireframe ? 1 : 0;
    ImGui::SetNextItemWidth(110.0f);
    if (ImGui::Combo("##preview_mode", &mode, modes, 2))
        wireframe = mode == 1;
    ImGui::SameLine();
    if (ImGui::RadioButton("Source", !showCooked)) showCooked = false;
    ImGui::SameLine();
    if (ImGui::RadioButton("Cooked", showCooked)) showCooked = true;
    ImGui::SameLine();
    const int levels = geometry ? geometry->levelCount : 1;
    forceLod = std::min(forceLod, levels - 1);
    ImGui::SetNextItemWidth(-1);
    ImGui::SliderInt("##force_lod", &forceLod, 0, std::max(0, levels - 1), "Force LOD %d");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Draws the baked level (Cooked: the file's JE_mesh_lods levels).");
}

void ModelImporterView::drawMeshes(const ResolvedModelSettings& r){
    const std::string prefix = tab > 0 ? std::string("platforms.") + modelFamilyName(shownFamily()) + "." : std::string();
    const std::vector<std::string>& optimize = modelEnumValues("optimizeMesh");
    form.comboField("Optimize Mesh", prefix + "optimizeMesh", optimize, {"Everything", "Polygon Order", "Vertex Order", "Nothing"},
                    optimizeMeshName(r.optimizeMesh), "Reorders triangles for the vertex cache and overdraw, and vertices for the "
                    "fetch; Everything also welds exact duplicates. Blended submeshes keep their triangle order.");
    form.comboField("Mesh Compression", prefix + "meshCompression", modelEnumValues("meshCompression"), {"Off", "Lossless", "Low", "Medium", "High"},
                    meshCompressionName(r.meshCompression), "Lossless stores the same values in less space. Low, Medium and High keep fewer "
                    "bits: smaller files, a small change the table below measures.");
    form.boolField("Weld Vertices", prefix + "weldVertices", r.weldVertices, "Merges vertices whose every attribute is the same.");
    form.comboField("Index Format", prefix + "indexFormat", modelEnumValues("indexFormat"), {"Auto", "16 bit", "32 bit"},
                    indexFormatName(r.indexFormat), "Auto takes 16 bits when a mesh's vertices fit (65,535 or fewer).");
    form.comboField("Read/Write", prefix + "readWrite", modelEnumValues("readWrite"), {"Automatic", "On"}, modelReadWriteName(r.readWrite),
                    "Automatic keeps the mesh data on the CPU in games only when something reads it there; On always keeps it.");
    if (!rows.empty() && assetPaths.size() == 1){
        const ModelReadWriteDecision& decision = rows[0].readWrite;
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(1);
        ImGui::PushTextWrapPos(0.0f);
        if (decision.keep)
            ImGui::TextDisabled("Kept: %s", decision.reasons.empty() ? "" : decision.reasons.front().c_str());
        else
            ImGui::TextDisabled("Nothing at runtime reads it");
        ImGui::PopTextWrapPos();
        if (decision.reasons.size() > 1 && ImGui::IsItemHovered()){
            ImGui::BeginTooltip();
            for (const std::string& reason : decision.reasons) ImGui::TextUnformatted(reason.c_str());
            ImGui::EndTooltip();
        }
    }
    form.comboField("Normals", prefix + "normals", modelEnumValues("normals"), {"Import", "Calculate"}, modelNormalsName(r.normals),
                    "Calculate makes area-weighted normals that crease at the Smoothing Angle.");
    if (r.normals == ModelNormals::Calculate)
        form.floatSlider("Smoothing Angle", prefix + "smoothingAngle", r.smoothingAngle, 0.0f, 180.0f, "%.0f\xC2\xB0",
                         "Edges sharper than this stay hard.");
    form.boolField("Ship Source File", prefix + "shipSource", r.shipSource,
                   "Exports keep the source model next to its cook (code that reads the file itself).");
}

void ModelImporterView::drawLevels(const ResolvedModelSettings& r){
    const std::string prefix = tab > 0 ? std::string("platforms.") + modelFamilyName(shownFamily()) + "." : std::string();
    form.comboField("Generate LODs", prefix + "generateLods", modelEnumValues("generateLods"), {"Automatic", "On", "Off"},
                    generateLodsName(r.generateLods), "Automatic bakes the levels the runtime would build; On bakes LOD Count levels of "
                    "the Reduction; Off bakes none.");
    ImGui::BeginDisabled(r.generateLods != GenerateLods::On);
    form.comboField("LOD Count", prefix + "lodCount", {"1", "2", "3"}, {"1", "2", "3"}, std::to_string(r.lodCount), "Levels after LOD 0.");
    form.comboField("Reduction", prefix + "lodReduction", modelEnumValues("lodReduction"), {"Preserve Detail", "Balanced", "Smallest"},
                    lodReductionName(r.lodReduction), "Preserve Detail keeps 60% a level with a tight error, Balanced 50%, Smallest 35%.");
    ImGui::EndDisabled();
}

void ModelImporterView::drawBeforeAfter(){
    if (rows.empty())
        return;
    const ModelAuditRow& row = rows[0];
    const bool cooked = row.hasState && row.state.status == ModelCookStatus::Cooked && !row.state.stale;
    const char* pending = row.hasState && row.state.status == ModelCookStatus::Failed ? "failed"
                          : row.hasState && (row.state.status == ModelCookStatus::Queued || row.state.status == ModelCookStatus::Cooking || row.state.stale)
                              ? "cooking..." : "not cooked yet";
    const ModelCookRecord& record = row.state.record;
    // the levels: a read-only bar and the table
    if (cooked && !record.lods.empty()){
        const float width = ImGui::GetContentRegionAvail().x;
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const float height = ImGui::GetFrameHeight();
        ImDrawList* draw = ImGui::GetWindowDrawList();
        // each level's share of the bar: where it is drawn as the model shrinks on screen
        std::vector<float> switches = {1.0f};
        for (const ModelLodLevelStats& level : record.lods){
            const double screen = level.error > 0.0 ? record.modelSize / (level.error * kReferenceViewHeight) : 1.0;
            switches.push_back((float)std::clamp(screen, 0.0, (double)switches.back()));
        }
        float x = at.x;
        for (size_t l = 0; l < switches.size(); l++){
            const float next = l + 1 < switches.size() ? switches[l + 1] : 0.0f;
            const float w = std::max(2.0f, (switches[l] - next) * width);
            draw->AddRectFilled(ImVec2(x, at.y), ImVec2(std::min(x + w, at.x + width), at.y + height), kLodColors[std::min<size_t>(l, 3)]);
            char label[48];
            std::snprintf(label, sizeof(label), "LOD %zu  %.0f%%", l, switches[l] * 100.0f);
            // a level too narrow for its label: the table below has its numbers
            if (ImGui::CalcTextSize(label).x + 8.0f <= w)
                draw->AddText(ImVec2(x + 4.0f, at.y + 2.0f), IM_COL32(20, 20, 20, 255), label);
            x += w;
        }
        ImGui::InvisibleButton("##lod_bar", ImVec2(width, height));
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("About the screen height each level draws below, in a 1080-pixel view at the scene's default Threshold "
                              "and LOD Bias 1 (read only: selection follows the levels' errors).");
        // short cells and headers: the Inspector is about 350 pixels wide (the tooltips say the rest)
        if (ImGui::BeginTable("model_lods", 5, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingFixedFit)){
            ImGui::TableSetupColumn("Level");
            ImGui::TableSetupColumn("Tris");
            ImGui::TableSetupColumn("Share");
            ImGui::TableSetupColumn("Differs by at most");
            ImGui::TableSetupColumn("Below");
            ImGui::TableHeadersRow();
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted("LOD 0");
            ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(countText(record.after.triangles).c_str());
            ImGui::TableSetColumnIndex(2); ImGui::TextUnformatted("100%");
            ImGui::TableSetColumnIndex(3); ImGui::TextUnformatted("exact");
            ImGui::TableSetColumnIndex(4); ImGui::TextUnformatted("-");
            for (size_t l = 0; l < record.lods.size(); l++){
                const ModelLodLevelStats& level = record.lods[l];
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::Text("LOD %d", level.level);
                ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(countText(level.triangles).c_str());
                ImGui::TableSetColumnIndex(2); ImGui::Text("%.0f%%", level.share * 100.0);
                ImGui::TableSetColumnIndex(3); ImGui::Text("%.2g%%", level.deviation * 100.0);
                ImGui::SetItemTooltip("The largest distance measured from the source's surface, as a share of the model's size.");
                ImGui::TableSetColumnIndex(4); ImGui::Text("~%.0f%%", switches[l + 1] * 100.0f);
                ImGui::SetItemTooltip("About the screen height this level draws below (the bar above says how it is measured).");
            }
            ImGui::EndTable();
        }
    }
    ImGui::TextDisabled("Before and after (%s)", modelFamilyLabel(row.family));
    if (!ImGui::BeginTable("model_before_after", 4, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp))
        return;
    ImGui::TableSetupColumn("");
    ImGui::TableSetupColumn("Source");
    ImGui::TableSetupColumn("Cooked");
    ImGui::TableSetupColumn("Saved");
    ImGui::TableHeadersRow();
    auto line = [&](const char* name, const std::string& before, const std::string& after, const std::string& saved){
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(name);
        ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(before.c_str());
        ImGui::TableSetColumnIndex(2);
        if (cooked) ImGui::TextUnformatted(after.c_str());
        else ImGui::TextDisabled("%s", pending);
        ImGui::TableSetColumnIndex(3); ImGui::TextUnformatted(cooked ? saved.c_str() : "");
    };
    const std::string sourceTriangles = cooked ? countText(record.before.triangles) : countText(row.header.triangles);
    const std::string sourceVertices = cooked ? countText(record.before.vertices) : countText(row.header.vertices);
    line("Triangles", sourceTriangles, countText(record.after.triangles), "-");
    line("Vertices", sourceVertices, countText(record.after.vertices), savedText((double)record.before.vertices, (double)record.after.vertices));
    line("Mesh Memory", cooked ? bytesText(record.before.memoryBytes()) : bytesText(row.header.vertexBytes + row.header.indexBytes),
         bytesText(record.after.memoryBytes()), savedText((double)record.before.memoryBytes(), (double)record.after.memoryBytes()));
    line("Download", cooked ? bytesText(record.sourceBytes) : bytesText(row.header.fileBytes), bytesText(record.diskBytes),
         savedText((double)record.sourceBytes, (double)record.diskBytes));
    if (cooked)
        line("Visual Change", "", record.visualChange(), "");
    ImGui::EndTable();
}

void ModelImporterView::drawPlatformTable(){
    if (!ImGui::BeginTable("model_platforms", 5, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp))
        return;
    ImGui::TableSetupColumn("Platform");
    ImGui::TableSetupColumn("Memory");
    ImGui::TableSetupColumn("Download");
    ImGui::TableSetupColumn("LODs");
    ImGui::TableSetupColumn("Status");
    ImGui::TableHeadersRow();
    ModelCookQueue& queue = ModelCookQueue::get();
    for (size_t f = 0; f < kModelFamilyCount; f++){
        ModelCookState state;
        const bool known = queue.getState(assetPaths[0], (ModelFamily)f, state);
        const bool cooked = known && state.status == ModelCookStatus::Cooked;
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(modelFamilyLabel((ModelFamily)f));
        ImGui::TableSetColumnIndex(1); if (cooked) ImGui::TextUnformatted(bytesText(state.record.after.memoryBytes()).c_str());
        ImGui::TableSetColumnIndex(2); if (cooked) ImGui::TextUnformatted(bytesText(state.record.diskBytes).c_str());
        ImGui::TableSetColumnIndex(3); if (cooked) ImGui::Text("%zu", state.record.lods.size());
        ImGui::TableSetColumnIndex(4);
        if (known && state.status == ModelCookStatus::Failed){
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.35f, 1.0f), ICON_FA_TRIANGLE_EXCLAMATION " failed");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", state.error.c_str());
        }else{
            ImGui::TextUnformatted(!known ? "not cooked" : state.stale ? "cooking..." : modelCookStatusName(state.status));
        }
    }
    ImGui::EndTable();
}

void ModelImporterView::drawIssues(){
    if (rows.empty())
        return;
    const ModelAuditRow& row = rows[0];
    int index = 0;
    for (const ModelIssue& issue : row.issues){
        ImGui::PushID(index++);
        const bool error = issue.severity == "error";
        const ImVec4 color = error ? ImVec4(1.0f, 0.4f, 0.35f, 1.0f) : issue.severity == "warning" ? ImVec4(1.0f, 0.75f, 0.3f, 1.0f)
                                                                                                   : ImVec4(0.7f, 0.8f, 0.95f, 1.0f);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(color, "%s %s", error ? ICON_FA_CIRCLE_XMARK : ICON_FA_TRIANGLE_EXCLAMATION, issue.message.c_str());
        ImGui::PopTextWrapPos();
        // the failure row's actions (R18) and each warning's fix
        // the row's buttons on the line under its message, wrapping at the Inspector's edge (a
        // SameLine right after wrapped text would go on from the end of the text)
        bool anyAction = false;
        auto actionButton = [&anyAction](const char* label){
            if (anyAction){
                const ImGuiStyle& style = ImGui::GetStyle();
                const float width = ImGui::CalcTextSize(label).x + style.FramePadding.x * 2.0f;
                const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
                if (ImGui::GetItemRectMax().x + style.ItemSpacing.x + width <= right)
                    ImGui::SameLine();
            }
            anyAction = true;
            return ImGui::SmallButton(label);
        };
        if (issue.fix == ModelIssue::Fix::Settings && actionButton(issue.fixLabel.c_str())){
            ModelSettingsEdit edit;
            for (const auto& [key, value] : issue.settings.items())
                edit.set.push_back({ModelAudit::metaKeyOf(key), value.is_boolean() ? YAML::Node(value.get<bool>()) : YAML::Node(value.get<std::string>())});
            YAML::Node section;
            std::string message;
            std::vector<std::string> changed;
            if (editModelSection(stored[0], edit, section, message))
                writeModelSections(*project, {assetPaths[0]}, {section}, "Set Model Import Settings", changed, message);
        }
        // a recook fixes a failed cook, and turns a Draco or WebP source into what games read
        // (R18 "Cook anyway"); an extension the engine cannot read needs the file exported again
        const bool cooked = row.hasState && row.state.status == ModelCookStatus::Cooked;
        const bool transcode = issue.code == "gltf-cook-transcodes" && !cooked;
        const bool recook = issue.fix == ModelIssue::Fix::Reimport || transcode || (error && issue.code != "gltf-required-extension");
        if (recook && actionButton(transcode ? "Cook anyway" : "Reimport")){
            ModelCookQueue::get().invalidate(assetPaths[0]);
            ModelCookQueue::get().request(assetPaths[0], shownFamily(), ModelCookQueue::Selected);
        }
        if (issue.fix == ModelIssue::Fix::SceneLod && actionButton(issue.fixLabel.c_str())){
            for (SceneProject& scene : project->getScenes()){
                if (project->normalizeToProjectRelative(scene.filepath).generic_string() != issue.scene || !scene.scene)
                    continue;
                CommandHistory* history = CommandHandle::get(scene.id);
                if (history) history->addCommand(new ScenePropertyCmd<bool>(project, scene.id, "mesh_lod_enabled", true));
            }
        }
        if (error){
            if (actionButton("Open Console")) editor::getEditorHost().openConsole();
            if (actionButton("Show in Explorer"))
                EditorSettingsWindow::openInFileManager((project->getAssetsPath() / FileUtils::pathFromUtf8(assetPaths[0])).parent_path());
        }
        ImGui::PopID();
    }
    if (row.hasState && row.state.status == ModelCookStatus::Cooked)
        for (const std::string& kept : row.state.record.keptOrder)
            ImGui::TextDisabled(ICON_FA_CIRCLE_INFO " Triangle order kept on %s", kept.c_str());
}

void ModelImporterView::drawMultiple(){
    long long trisAfter = 0;
    uint64_t memoryBefore = 0, memoryAfter = 0, downloadBefore = 0, downloadAfter = 0;
    int cooked = 0, cooking = 0, failed = 0;
    for (const ModelAuditRow& row : rows){
        const std::string status = row.status();
        cooked += status == "cooked" ? 1 : 0;
        cooking += status == "cooking" ? 1 : 0;
        failed += status == "failed" ? 1 : 0;
        if (status == "cooked"){
            trisAfter += row.state.record.after.triangles;
            memoryBefore += row.state.record.before.memoryBytes();
            memoryAfter += row.state.record.after.memoryBytes();
            downloadBefore += row.state.record.sourceBytes;
            downloadAfter += row.state.record.diskBytes;
        }
    }
    ImGui::TextWrapped("Totals (%s, %d cooked): %s tris | memory %s -> %s | download %s -> %s | %d/%zu cooked, %d cooking, %d failed",
                       modelFamilyLabel(shownFamily()), cooked, countText(trisAfter).c_str(), bytesText(memoryBefore).c_str(), bytesText(memoryAfter).c_str(),
                       bytesText(downloadBefore).c_str(), bytesText(downloadAfter).c_str(), cooked, rows.size(), cooking, failed);
    // the largest: a click inspects that one
    std::vector<const ModelAuditRow*> largest;
    for (const ModelAuditRow& row : rows) largest.push_back(&row);
    std::sort(largest.begin(), largest.end(), [](const ModelAuditRow* a, const ModelAuditRow* b){ return a->memoryBytes() > b->memoryBytes(); });
    ImGui::TextDisabled("Largest:");
    for (size_t i = 0; i < largest.size() && i < 5; i++){
        const ModelAuditRow& row = *largest[i];
        const std::string label = fs::path(row.assetPath).filename().string() + "  " + bytesText(row.memoryBytes()) + "  " + countText(row.triangles()) +
                                  " tris##largest" + std::to_string(i);
        if (ImGui::Selectable(label.c_str()))
            project->setInspectedAssets({fs::path(row.projectPath)});
    }
}

bool ModelImporterView::draw(const std::vector<fs::path>& selection, const std::function<void(const std::string&)>& drawClips){
    // a selection of models and textures: one importer at a time
    int models = 0, textures = 0;
    for (const fs::path& path : selection){
        const std::string name = path.filename().string();
        models += isModelSource(name) ? 1 : 0;
        textures += isTextureSource(name) ? 1 : 0;
    }
    if (models > 0 && textures > 0){
        ImGui::TextWrapped("Narrow the selection: %d models, %d textures", models, textures);
        if (ImGui::Button("Select Models")){
            std::vector<fs::path> keep;
            for (const fs::path& path : selection) if (isModelSource(path.filename().string())) keep.push_back(path);
            project->setInspectedAssets(keep);
        }
        ImGui::SameLine();
        if (ImGui::Button("Select Textures")){
            std::vector<fs::path> keep;
            for (const fs::path& path : selection) if (isTextureSource(path.filename().string())) keep.push_back(path);
            project->setInspectedAssets(keep);
        }
        return true;
    }
    select(selection);
    if (assetPaths.empty())
        return false;
    refreshRows(false);

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(ICON_FA_CUBE);
    ImGui::SameLine();
    if (assetPaths.size() == 1) ImGui::TextUnformatted(fs::path(assetPaths[0]).filename().string().c_str());
    else ImGui::Text("%zu models", assetPaths.size());
    ImGui::TextDisabled("Model Import Settings");
    ImGui::Separator();

    if (assetPaths.size() == 1){
        drawPreview();
        if (!rows.empty()){
            const ModelAuditRow& row = rows[0];
            if (row.hasState && row.state.status == ModelCookStatus::Cooked && !row.state.stale){
                const ModelCookRecord& r = row.state.record;
                ImGui::TextWrapped("%s cook: %s tris, %s verts, %s memory, %s download (source %s)", modelFamilyLabel(row.family),
                                   countText(r.after.triangles).c_str(), countText(r.after.vertices).c_str(), bytesText(r.after.memoryBytes()).c_str(),
                                   bytesText(r.diskBytes).c_str(), bytesText(r.sourceBytes).c_str());
            }else{
                ImGui::TextDisabled("%s", row.hasState && row.state.status == ModelCookStatus::Failed ? "The cook failed" : "Cooking...");
            }
        }
        if (ImGui::BeginTabBar("model_importer_pages")){
            if (ImGui::BeginTabItem("Model")){ pageTab = 0; ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Animation")){ pageTab = 1; ImGui::EndTabItem(); }
            ImGui::EndTabBar();
        }
        if (pageTab == 1){
            if (drawClips) drawClips(assetPaths[0]);
            return true;
        }
    }

    if (ImGui::BeginTabBar("model_importer_tabs")){
        if (ImGui::BeginTabItem("Default")){ tab = 0; ImGui::EndTabItem(); }
        for (size_t f = 0; f < kModelFamilyCount; f++){
            if (ImGui::BeginTabItem(modelFamilyLabel((ModelFamily)f))){ tab = (int)f + 1; ImGui::EndTabItem(); }
        }
        ImGui::EndTabBar();
    }
    const ModelImportSettings settings = workingSettings();
    const ResolvedModelSettings resolved = settings.resolve(shownFamily(), project->getModelSettings());
    auto section = [&](const char* title, auto&& body){
        if (ImGui::CollapsingHeader(title, ImGuiTreeNodeFlags_DefaultOpen)){
            if (ImGui::BeginTable(title, 2, ImGuiTableFlags_SizingStretchProp)){
                ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthStretch, 0.45f);
                ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 0.55f);
                body();
                ImGui::EndTable();
            }
        }
    };
    if (tab > 0){
        const std::string key = std::string("platforms.") + modelFamilyName(shownFamily()) + ".override";
        const YAML::Node value = form.value(key);
        section("Platform", [&]{
            form.boolField((std::string("Override for ") + modelFamilyLabel(shownFamily())).c_str(), key, value && value.as<bool>(false),
                           "This platform's own values of the settings below.");
        });
        ImGui::BeginDisabled(!(value && value.as<bool>(false)));
    }
    section("Meshes", [&]{ drawMeshes(resolved); });
    section("Level of Detail", [&]{ drawLevels(resolved); });
    if (tab > 0)
        ImGui::EndDisabled();

    ImGui::Separator();
    if (assetPaths.size() == 1){
        drawBeforeAfter();
        ImGui::Separator();
        drawPlatformTable();
        drawIssues();
    }else{
        drawMultiple();
    }

    ImGui::Separator();
    const bool playing = project->isAnyScenePlaying();
    ImGui::BeginDisabled(!hasPendingEdits() || playing);
    if (ImGui::Button("Revert")) revert();
    ImGui::SameLine();
    if (ImGui::Button("Apply")) apply();
    ImGui::EndDisabled();
    if (playing && hasPendingEdits() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Stop Play to apply import settings.");
    return true;
}
