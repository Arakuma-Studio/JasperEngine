// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Inspector's Vertex Animation parts (vertex animation spec 3.3, 3.6, 4.5, 5.2, 5.4):
// - the Vertex Animator section: Animation field (drop a .vat, pick one), Clip dropdown with
//   Preview (PreviewOwner; stops on play, save, a selection or scene change), the settings,
//   Attachments, the time scrubber while previewing, the load status (loadError,
//   mesh_mismatch with its one-click fix, the device's texture limit);
// - the .vat asset view of a Project window click: header, staleness banner, Rebake / Open in
//   Baker (VatAppHooks: disabled until the baker sets them), Create Crowd..., the clips table
//   (name, wrap, speed, interpolate editable; events in a list per clip), sockets (rename), the
//   removed-clips note, load warnings. Edits go through VatDocument (project history);
// - the Create Crowd dialog (VatCrowd: one scene step).
// Every component edit is a PropertyCmd / MultiPropertyCmd (scene undo). Owner: W5.

#include "InspectorWindow.h"
#include "InspectorWindowVat.h"

#include "animation/PreviewOwner.h"
#include "animation/VertexAnimatorRuntime.h"
#include "command/CommandHandle.h"
#include "command/type/ModelLoadCmd.h"
#include "command/type/MultiPropertyCmd.h"
#include "command/type/PropertyCmd.h"
#include "component/MeshComponent.h"
#include "component/ModelComponent.h"
#include "component/Transform.h"
#include "component/VertexAnimatorComponent.h"
#include "external/IconsFontAwesome6.h"
#include "pool/VertexAnimationPool.h"
#include "render/SystemRender.h"
#include "subsystem/VertexAnimatorSystem.h"
#include "util/FileDialogs.h"
#include "util/UIProbe.h"
#include "util/Util.h"
#include "vat/VatAppHooks.h"
#include "vat/VatAssetFiles.h"
#include "vat/VatBakeSession.h"
#include "vat/VatCrowd.h"
#include "vat/VatDocument.h"
#include "Out.h"
#include "Scene.h"
#include "Theme.h"

#include "imgui.h"
#include "window/EditorModal.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;
namespace jv = jasperengine::vat;
namespace ev = jasperengine::editor::vat;

// ---- pure helpers ----

std::string inspectorvat::formatBytes(uint64_t bytes){
    char text[32];
    if (bytes < 1024){
        snprintf(text, sizeof(text), "%llu B", (unsigned long long)bytes);
    }else if (bytes < 1024ull * 1024ull){
        snprintf(text, sizeof(text), "%.1f KB", (double)bytes / 1024.0);
    }else{
        snprintf(text, sizeof(text), "%.2f MB", (double)bytes / (1024.0 * 1024.0));
    }
    return text;
}

const char* inspectorvat::modeLabel(jv::VatMode mode){
    return mode == jv::VatMode::Bones ? "Bones" : "Vertices";
}

const char* inspectorvat::formatLabel(jv::VatPrecision precision){
    return precision == jv::VatPrecision::Float ? "RGBA32F" : "RGBA16F";
}

namespace{

    // 5120 -> "5,120"
    std::string grouped(int64_t value){
        std::string digits = std::to_string(value < 0 ? -value : value);
        std::string out;
        for (size_t i = 0; i < digits.size(); i++){
            if (i > 0 && (digits.size() - i) % 3 == 0) out += ',';
            out += digits[i];
        }
        return value < 0 ? "-" + out : out;
    }

    std::string trimmed(const std::string& text){
        size_t begin = 0, end = text.size();
        while (begin < end && std::isspace((unsigned char)text[begin])) begin++;
        while (end > begin && std::isspace((unsigned char)text[end - 1])) end--;
        return text.substr(begin, end - begin);
    }

}

std::string inspectorvat::modeLine(const jv::VatData& data){
    if (data.mode == jv::VatMode::Bones){
        return std::string(modeLabel(data.mode)) + " \xC2\xB7 " + grouped(data.layout.bones) + (data.layout.bones == 1 ? " bone" : " bones");
    }
    std::string line = std::string(modeLabel(data.mode)) + " \xC2\xB7 " + grouped(data.layout.vertices) +
                       (data.layout.vertices == 1 ? " vertex" : " vertices");
    if (data.layout.normals) line += ", normals";
    if (data.layout.colors) line += ", colors";
    return line;
}

std::string inspectorvat::textureLine(const jv::VatData& data){
    return std::to_string(data.layout.width) + " \xC3\x97 " + std::to_string(data.layout.height) + " " +
           formatLabel(data.layout.precision) + " \xC2\xB7 " + formatBytes(data.stats.bytes);
}

std::string inspectorvat::errorLine(const jv::VatData& data){
    char text[96];
    snprintf(text, sizeof(text), "Error: max %.2f mm, mean %.2f mm", data.stats.maxErrorMm, data.stats.meanErrorMm);
    return text;
}

uint64_t inspectorvat::clipBytes(const jv::VatData& data, const jv::VatClip& clip){
    const uint64_t elements = (uint64_t)std::max(data.mode == jv::VatMode::Bones ? data.layout.bones : data.layout.vertices, 0);
    const uint64_t texelBytes = data.layout.precision == jv::VatPrecision::Float ? 16 : 8;
    return (uint64_t)std::max(clip.frames, 0) * elements * (uint64_t)data.layout.texelsPerSample(data.mode) * texelBytes;
}

std::string inspectorvat::meshMismatchFix(const std::string& loadError, const std::string& animation, const std::string& meshName){
    if (loadError.compare(0, 14, "mesh_mismatch:") != 0 || animation.empty()) return std::string();
    const std::string mesh = meshName.empty() ? ev::pairPathOf(fs::path(animation)).filename().generic_string() : meshName;
    if (mesh.empty()) return std::string();
    return (fs::path(animation).parent_path() / mesh).lexically_normal().generic_string();
}

std::string inspectorvat::meshFixLabel(const std::string& meshPath){
    return "Use " + fs::path(meshPath).filename().generic_string();
}

std::string inspectorvat::deviceLimitWarning(int height, int maxTextureSize2D, const std::string& asset){
    if (maxTextureSize2D <= 0 || height <= maxTextureSize2D) return std::string();
    return fs::path(asset).filename().generic_string() + " needs a texture " + std::to_string(height) +
           " rows tall; this device allows " + std::to_string(maxTextureSize2D) +
           ": the mesh draws at rest here. Rebake with fewer frames, fewer clips or the Mobile & Web preset.";
}

float inspectorvat::clipTimeAt(const jv::VatClip& clip, double tau){
    const jv::VatFrameSample sample = jv::vatSample(clip, (float)tau);
    return std::clamp(sample.normalized, 0.0f, 1.0f) * clip.length();
}

double inspectorvat::scrubTarget(const jv::VatClip& clip, double tau, float time){
    if (clip.wrap == ClipWrapMode::Loop || clip.wrap == ClipWrapMode::PingPong){
        const double period = jv::vatClipPeriod(clip);
        const double base = period > 0.0 && std::isfinite(tau) ? std::floor(tau / period) * period : 0.0;
        return base + (double)time;
    }
    return (double)time;
}

std::vector<double> inspectorvat::shiftedTimes(const std::vector<double>& instanceTaus, double from, double to){
    std::vector<double> out;
    out.reserve(instanceTaus.size());
    for (double tau : instanceTaus) out.push_back(tau + (to - from));
    return out;
}

const char* inspectorvat::wrapLabel(ClipWrapMode wrap){
    switch (wrap){
        case ClipWrapMode::Once: return "Once";
        case ClipWrapMode::PingPong: return "Ping Pong";
        case ClipWrapMode::ClampForever: return "Clamp Forever";
        default: return "Loop";
    }
}

std::string inspectorvat::eventArgsText(const std::vector<AnimationEventArg>& args){
    std::string text;
    for (const AnimationEventArg& arg : args){
        if (!text.empty()) text += ", ";
        if (const double* number = std::get_if<double>(&arg)){
            char buffer[32];
            snprintf(buffer, sizeof(buffer), "%.9g", *number);
            text += buffer;
        }else if (const bool* flag = std::get_if<bool>(&arg)){
            text += *flag ? "true" : "false";
        }else{
            const std::string& value = std::get<std::string>(arg);
            // quoted when it would read back as something else
            const std::string bare = trimmed(value);
            char* end = nullptr;
            const bool looksNumber = !bare.empty() && (std::strtod(bare.c_str(), &end), end && *end == '\0');
            const bool needsQuotes = value.empty() || bare != value || looksNumber || value == "true" || value == "false" ||
                                     value.find(',') != std::string::npos || value.find('"') != std::string::npos;
            if (needsQuotes){
                std::string escaped;
                for (char c : value){
                    if (c == '"' || c == '\\') escaped += '\\';
                    escaped += c;
                }
                text += "\"" + escaped + "\"";
            }else{
                text += value;
            }
        }
    }
    return text;
}

bool inspectorvat::parseEventArgs(const std::string& text, std::vector<AnimationEventArg>& out, std::string* error){
    out.clear();
    size_t i = 0;
    auto skip = [&](){ while (i < text.size() && std::isspace((unsigned char)text[i])) i++; };
    skip();
    if (i == text.size()) return true;
    while (true){
        skip();
        if (i < text.size() && text[i] == '"'){
            std::string value;
            i++;
            bool closed = false;
            while (i < text.size()){
                if (text[i] == '\\' && i + 1 < text.size()){
                    value += text[i + 1];
                    i += 2;
                    continue;
                }
                if (text[i] == '"'){
                    closed = true;
                    i++;
                    break;
                }
                value += text[i++];
            }
            if (!closed){
                if (error) *error = "an argument's quote is not closed";
                return false;
            }
            out.push_back(AnimationEvent::stringArg(value));
            skip();
        }else{
            const size_t comma = text.find(',', i);
            const std::string token = trimmed(text.substr(i, comma == std::string::npos ? std::string::npos : comma - i));
            if (token.empty()){
                if (error) *error = "an argument is empty (two commas, or one at the end)";
                return false;
            }
            char* end = nullptr;
            const double number = std::strtod(token.c_str(), &end);
            if (end && *end == '\0' && std::isfinite(number)){
                out.push_back(AnimationEvent::numberArg(number));
            }else if (token == "true" || token == "false"){
                out.push_back(AnimationEvent::boolArg(token == "true"));
            }else{
                out.push_back(AnimationEvent::stringArg(token));
            }
            i = comma == std::string::npos ? text.size() : comma;
        }
        if (i >= text.size()) return true;
        if (text[i] != ','){
            if (error) *error = "expected a comma between arguments";
            return false;
        }
        i++;
    }
}

bool inspectorvat::isLuaName(const std::string& name){
    if (name.empty() || std::isdigit((unsigned char)name[0])) return false;
    return std::all_of(name.begin(), name.end(), [](unsigned char c){ return std::isalnum(c) || c == '_'; });
}

std::string inspectorvat::stalenessText(const std::string& kind, const std::string& message, const std::string& fix){
    std::string text = kind == "vat-source-changed" || kind == "vat-source-missing" ? "Source changed since baking: " + message : message;
    if (!fix.empty()){
        std::string sentence = fix;
        sentence[0] = (char)std::toupper((unsigned char)sentence[0]);
        text += ". " + sentence;
    }
    if (!text.empty() && text.back() != '.') text += '.';
    return text;
}

std::string inspectorvat::removedClipsNote(const jv::VatData& data){
    const int unused = VatDocument::unusedSamples(data);
    if (unused <= 0) return std::string();
    jv::VatClip probe;
    probe.frames = unused;
    return "Removed clips keep their rows until the next Rebake (" + grouped(unused) + (unused == 1 ? " unused frame, " : " unused frames, ") +
           formatBytes(clipBytes(data, probe)) + ")";
}

// ---- state ----

namespace jasperengine::editor{

    struct InspectorVatState{
        struct DocumentEntry{
            std::weak_ptr<VatDocument> document;
            std::string path;
            std::string error;
            double retryAt = 0.0;
        };
        struct StalenessEntry{
            uint64_t revision = 0;
            double checkedAt = -1.0;
            std::vector<ev::VatIssue> issues;
        };

        fs::path project;
        std::map<std::string, DocumentEntry> documents;
        std::map<std::string, StalenessEntry> staleness;

        // the Vertex Animator preview (one at a time)
        PreviewOwner::Token previewToken = 0;
        uint32_t previewScene = 0;
        Entity previewEntity = NULL_ENTITY;
        bool scrubbing = false;

        // text fields over std::string: the one being typed in keeps its buffer
        std::string activeEdit;
        std::map<std::string, std::array<char, 256>> buffers;

        // drags merge into one step: a serial per drag
        int dragSerial = 0;

        std::string sectionError;
        std::string assetError;
        fs::path assetShown;
        int eventsClipStart = -1;   // the clip (by first frame) whose events list is open

        // Create Crowd
        bool crowdRequested = false;
        ev::CrowdSettings crowd;
        std::vector<float> crowdWeights;
        std::string crowdVat;
        std::string crowdResult;
        bool crowdResultOk = false;
        bool crowdOnSurface = false;

        void forgetProject(const fs::path& path){
            if (project == path) return;
            documents.clear();
            staleness.clear();
            buffers.clear();
            activeEdit.clear();
            assetShown.clear();
            project = path;
        }
    };

    void InspectorVatStateDeleter::operator()(InspectorVatState* state) const{
        delete state;
    }

}

InspectorVatState& editor::InspectorWindow::vatState(){
    if (!vatStatePtr){
        vatStatePtr.reset(new InspectorVatState());
    }
    vatStatePtr->forgetProject(project->getProjectPath());
    return *vatStatePtr;
}

namespace{

    constexpr double DOCUMENT_RETRY_SECONDS = 1.0;
    constexpr double STALENESS_SECONDS = 5.0;
    constexpr size_t MAX_ROWS = 64;

    float textWidth(const std::string& text){
        return ImGui::CalcTextSize(text.c_str()).x;
    }

    float iconButtonWidth(const char* icon){
        return ImGui::CalcTextSize(icon).x + ImGui::GetStyle().FramePadding.x * 2;
    }

    void warningText(const std::string& text){
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::WarningText);
        ImGui::TextWrapped(ICON_FA_TRIANGLE_EXCLAMATION " %s", text.c_str());
        ImGui::PopStyleColor();
    }

    void errorText(const std::string& text){
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::ErrorText);
        ImGui::TextWrapped("%s", text.c_str());
        ImGui::PopStyleColor();
    }

    // the asset-relative form of a path inside the assets folder, "" outside
    std::string assetRelative(Project* project, const fs::path& path){
        const fs::path absolute = fs::absolute(project->resolveAssetPath(path)).lexically_normal();
        if (!project->isInsideAssetsPath(absolute)) return std::string();
        return project->normalizeToAssetsRelative(absolute).generic_string();
    }

    std::shared_ptr<VatDocument> findDocument(Project* project, InspectorVatState& state, const std::string& path, std::string* error){
        if (path.empty()){
            if (error) *error = "no Vertex Animation";
            return nullptr;
        }
        const double now = ImGui::GetTime();
        auto it = state.documents.find(path);
        if (it != state.documents.end()){
            std::shared_ptr<VatDocument> document = it->second.document.lock();
            // still the file the path named (a rename takes the document along)
            if (document && document->exists() && document->path() == it->second.path) return document;
            if (!document && now < it->second.retryAt){
                if (error) *error = it->second.error;
                return nullptr;
            }
        }
        if (state.documents.size() > 64) state.documents.clear();
        InspectorVatState::DocumentEntry& entry = state.documents[path];
        std::string openError;
        std::shared_ptr<VatDocument> document = VatDocument::open(project, path, &openError);
        entry.document = document;
        entry.path = document ? document->path() : std::string();
        entry.error = openError;
        entry.retryAt = now + DOCUMENT_RETRY_SECONDS;
        if (!document && error) *error = openError;
        return document;
    }

    // A text field over a std::string: true (with `out`) when an edit ended (Enter or focus left)
    bool textField(InspectorVatState& state, const std::string& id, const std::string& value, std::string& out, float width){
        std::array<char, 256>& buffer = state.buffers[id];
        if (state.activeEdit != id){
            std::snprintf(buffer.data(), buffer.size(), "%s", value.c_str());
        }
        ImGui::SetNextItemWidth(width);
        ImGui::InputText(("##" + id).c_str(), buffer.data(), buffer.size());
        const bool done = ImGui::IsItemDeactivatedAfterEdit();
        if (ImGui::IsItemActive()){
            state.activeEdit = id;
        }else if (state.activeEdit == id){
            state.activeEdit.clear();
        }
        if (state.buffers.size() > 512) state.buffers.clear();
        if (done){
            out = buffer.data();
            return out != value;
        }
        return false;
    }

    // what a .vat path names as a dropped / picked file (a .vat.glb stands for its .vat)
    std::string vatFileOf(const std::string& file){
        if (ev::isVatMeshFile(file)) return ev::pairPathOf(fs::path(file)).generic_string();
        return Util::isVertexAnimationFile(file) ? file : std::string();
    }

    bool draggingVat(){
        const ImGuiPayload* payload = ImGui::GetDragDropPayload();
        if (!payload || !payload->IsDataType("resource_files")) return false;
        for (const std::string& file : Util::getStringsFromPayload(payload)){
            if (!vatFileOf(file).empty()) return true;
        }
        return false;
    }

}

// ---- preview ----

void editor::InspectorWindow::stopVertexAnimatorPreview(){
    if (!vatStatePtr) return;
    InspectorVatState& state = *vatStatePtr;
    if (state.previewToken == 0 && state.previewEntity == NULL_ENTITY) return;
    const PreviewOwner::Token token = state.previewToken;
    const uint32_t sceneId = state.previewScene;
    const Entity entity = state.previewEntity;
    state.previewToken = 0;
    state.previewScene = 0;
    state.previewEntity = NULL_ENTITY;
    state.scrubbing = false;
    PreviewOwner::release(token);
    SceneProject* sceneProject = project->getScene(sceneId);
    if (sceneProject && sceneProject->scene && sceneProject->scene->isEntityCreated(entity)){
        // the system puts it back where it started at its next update (deterministic Scene view)
        if (VertexAnimatorComponent* animator = sceneProject->scene->findComponent<VertexAnimatorComponent>(entity)){
            animator->preview = false;
        }
        sceneProject->needUpdateRender = true;
    }
}

void editor::InspectorWindow::startVertexAnimatorPreview(SceneProject* sceneProject, Entity entity){
    stopVertexAnimatorPreview();
    VertexAnimatorComponent* animator = sceneProject->scene->findComponent<VertexAnimatorComponent>(entity);
    if (!animator) return;
    InspectorVatState& state = vatState();
    // another preview of this GameObject (the Animation window's, a pose) stops first
    state.previewToken = PreviewOwner::acquire(sceneProject->id, entity, sceneProject->scene, "Vertex Animator preview", [this](){
        stopVertexAnimatorPreview();
    });
    state.previewScene = sceneProject->id;
    state.previewEntity = entity;
    // it writes the GameObject's mesh state and its attachments' transforms
    std::vector<Entity> written = {entity};
    for (const VertexAnimatorAttachment& attachment : animator->attachments){
        if (attachment.entity != NULL_ENTITY) written.push_back(attachment.entity);
    }
    PreviewOwner::setWrites(state.previewToken, [written](Entity e){
        return std::find(written.begin(), written.end(), e) != written.end();
    });
    animator->preview = true;
    sceneProject->needUpdateRender = true;
}

void editor::InspectorWindow::clearVertexAnimatorPreviewScene(uint32_t sceneId){
    if (!vatStatePtr || vatStatePtr->previewScene != sceneId || vatStatePtr->previewEntity == NULL_ENTITY) return;
    InspectorVatState& state = *vatStatePtr;
    PreviewOwner::release(state.previewToken);
    state.previewToken = 0;
    state.previewScene = 0;
    state.previewEntity = NULL_ENTITY;
    state.scrubbing = false;
}

// Stops a preview that is no longer the shown selection's (another selection or scene, the
// asset view, play), or that something else stopped (a save, Play: Project::stopVertexAnimatorPreviews)
void editor::InspectorWindow::syncVertexAnimatorPreview(SceneProject* shown, const std::vector<Entity>& selection){
    if (!vatStatePtr || vatStatePtr->previewEntity == NULL_ENTITY) return;
    InspectorVatState& state = *vatStatePtr;
    SceneProject* sceneProject = project->getScene(state.previewScene);
    VertexAnimatorComponent* animator = sceneProject && sceneProject->scene && sceneProject->scene->isEntityCreated(state.previewEntity)
        ? sceneProject->scene->findComponent<VertexAnimatorComponent>(state.previewEntity) : nullptr;
    const bool matches = shown && shown == sceneProject && shown->playState == ScenePlayState::STOPPED &&
                         selection.size() == 1 && selection[0] == state.previewEntity;
    if (!animator || !animator->preview || !matches || !PreviewOwner::owns(state.previewToken)){
        stopVertexAnimatorPreview();
    }
}

// ---- Vertex Animator section ----

MultiPropertyCmd* inspectorvat::vertexAnimatorFileCmd(Project* project, uint32_t sceneId, const std::vector<Entity>& entities,
                                                     const std::string& file, std::string& error){
    const std::string vatFile = file.empty() ? std::string() : vatFileOf(file);
    const std::string relative = vatFile.empty() ? std::string() : assetRelative(project, fs::path(vatFile));
    if (!file.empty() && relative.empty()){
        error = "Not a Vertex Animation: " + fs::path(file).filename().string() +
                ". The Animation is a .vat file inside the assets folder (bake one with the Vertex Animation Baker).";
        return nullptr;
    }
    if (entities.empty()){
        error = "no GameObject";
        return nullptr;
    }
    std::string guid;
    if (!relative.empty()){
        const fs::path full = project->resolveAssetPath(relative);
        guid = project->getAssetDatabase().getGuid(full);
        if (guid.empty()){
            std::string openError;
            if (std::shared_ptr<VatDocument> document = VatDocument::open(project, full.string(), &openError)){
                guid = document->data().guid;
            }
        }
    }
    // the GUID follows the path (the old one would win on the next load), on every GameObject
    MultiPropertyCmd* multiCmd = new MultiPropertyCmd();
    for (Entity entity : entities){
        multiCmd->addPropertyCmd<std::string>(project, sceneId, entity, ComponentType::VertexAnimatorComponent, "animation", relative);
        multiCmd->addPropertyCmd<std::string>(project, sceneId, entity, ComponentType::VertexAnimatorComponent, "guid", guid);
    }
    multiCmd->setNoMerge();
    return multiCmd;
}

void editor::InspectorWindow::setVertexAnimatorFile(SceneProject* sceneProject, const std::vector<Entity>& entities, const std::string& file){
    InspectorVatState& state = vatState();
    std::string error;
    MultiPropertyCmd* command = inspectorvat::vertexAnimatorFileCmd(project, sceneProject->id, entities, file, error);
    if (!command){
        state.sectionError = error;
        return;
    }
    state.sectionError.clear();
    CommandHandle::get(sceneProject->id)->addCommand(command);
}

// The Animation field: the file name (or "—" when the selection differs), a pick button and a
// .vat drop target; every change goes through setVertexAnimatorFile (the path and its GUID)
void editor::InspectorWindow::drawVertexAnimatorFileField(SceneProject* sceneProject, const std::vector<Entity>& entities,
                                                          const std::string& animation, bool mixed, bool stopped){
    const float pickWidth = iconButtonWidth(ICON_FA_FOLDER_OPEN);
    const float fieldWidth = std::max(ImGui::GetContentRegionAvail().x - pickWidth - ImGui::GetStyle().ItemSpacing.x, 1.0f);
    ImGui::BeginGroup();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, Theme::Colors::filenameLabel);
    ImGui::BeginChild("vertexanimatorfile", ImVec2(fieldWidth, ImGui::GetFrameHeight()), false,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::SetCursorPos(ImVec2(ImGui::GetStyle().FramePadding.x, ImGui::GetStyle().FramePadding.y));
    if (mixed){
        ImGui::TextDisabled("\xE2\x80\x94");
    }else if (animation.empty()){
        ImGui::TextDisabled("< Not set >");
    }else{
        ImGui::TextUnformatted(fs::path(animation).filename().string().c_str());
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    UIProbe::item("vat.animation");
    if (mixed) ImGui::SetItemTooltip("The selected GameObjects play different Vertex Animations");
    else if (!animation.empty()) ImGui::SetItemTooltip("%s", animation.c_str());
    ImGui::SameLine();
    ImGui::BeginDisabled(!stopped);
    if (ImGui::Button(ICON_FA_FOLDER_OPEN "##vertexanimator_pick")){
        const std::string path = FileDialogs::openFileDialog(project->getAssetsPath().string(), FILE_DIALOG_VERTEX_ANIMATION);
        if (!path.empty()) setVertexAnimatorFile(sceneProject, entities, path);
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip(entities.size() > 1 ? "Pick a Vertex Animation (.vat) for every selected GameObject" : "Pick a Vertex Animation (.vat)");
    ImGui::EndGroup();
    if (stopped && draggingVat() && ImGui::BeginDragDropTarget()){
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("resource_files")){
            for (const std::string& file : Util::getStringsFromPayload(payload)){
                if (!vatFileOf(file).empty()){
                    setVertexAnimatorFile(sceneProject, entities, file);
                    break;
                }
            }
        }
        ImGui::EndDragDropTarget();
    }
}

void editor::InspectorWindow::drawVertexAnimatorComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities){
    InspectorVatState& state = vatState();
    Scene* scene = sceneProject->scene;
    const bool stopped = sceneProject->playState == ScenePlayState::STOPPED;

    RowSettings speedSettings;
    speedSettings.secondColSize = 6 * ImGui::GetFontSize();
    speedSettings.help = "Multiplies the speed of every clip; negative plays backwards.";
    RowSettings randomSettings;
    randomSettings.help = "Each instance (and the GameObject) starts at a random time of its first clip, seeded by the "
                          "GameObject and the instance: a crowd never marches in step.";
    RowSettings autoSettings;
    autoSettings.help = "Plays the clip when the game starts. Off: a script calls play(name).";
    RowSettings fadeSettings;
    fadeSettings.secondColSize = 6 * ImGui::GetFontSize();
    fadeSettings.help = "Seconds of the crossfade when play(name, fade) or crossFade(name) give none.";
    RowSettings eventsSettings;
    eventsSettings.help = "Fire clip events and finishes for every instance (onInstanceEvent / onInstanceFinish). "
                          "Costs work per instance each frame: off for large crowds that don't need them.";

    const float labelSize = getLabelSize("Play Automatically");

    if (entities.size() != 1){
        beginTable(cpType, labelSize, "vertexanimator_multi");
        // Animation: the same field as one GameObject's (the path and its GUID on every one)
        {
            std::string first;
            bool mixed = false;
            bool any = false;
            for (size_t i = 0; i < entities.size(); i++){
                const VertexAnimatorComponent* animator = scene->findComponent<VertexAnimatorComponent>(entities[i]);
                const std::string value = animator ? animator->animation : std::string();
                any = any || !value.empty();
                if (i == 0) first = value;
                else if (value != first) mixed = true;
            }
            prefabRowProperty = "animation";
            if (propertyHeader("Animation", -1, any)){
                setVertexAnimatorFile(sceneProject, entities, "");
            }
            drawVertexAnimatorFileField(sceneProject, entities, first, mixed, stopped);
        }
        propertyRow(RowPropertyType::Float, cpType, "speed", "Speed", sceneProject, entities, speedSettings);
        propertyRow(RowPropertyType::Bool, cpType, "randomStart", "Random Start", sceneProject, entities, randomSettings);
        propertyRow(RowPropertyType::Bool, cpType, "playAutomatically", "Play Automatically", sceneProject, entities, autoSettings);
        propertyRow(RowPropertyType::FloatPositive, cpType, "defaultFadeTime", "Default Fade", sceneProject, entities, fadeSettings);
        propertyRow(RowPropertyType::Bool, cpType, "instanceEvents", "Instance Events", sceneProject, entities, eventsSettings);
        endTable();
        if (!state.sectionError.empty()) errorText(state.sectionError);
        return;
    }

    const Entity entity = entities[0];
    VertexAnimatorComponent* animatorPtr = scene->findComponent<VertexAnimatorComponent>(entity);
    if (!animatorPtr) return;
    const VertexAnimatorComponent& animator = *animatorPtr;
    std::shared_ptr<const jv::VertexAnimation> asset = animator.asset;
    std::string documentError;
    std::shared_ptr<VatDocument> document = animator.animation.empty() ? nullptr :
        findDocument(project, state, project->resolveAssetPath(animator.animation).generic_string(), &documentError);
    // the clip names: the loaded asset's, else the file's
    std::vector<std::string> clipNames;
    if (asset){
        for (const jv::VatClip& clip : asset->getData().clips) clipNames.push_back(clip.name);
    }else if (document){
        for (const jv::VatClip& clip : document->data().clips) clipNames.push_back(clip.name);
    }

    beginTable(cpType, labelSize, "vertexanimator_main");

    // Animation: the .vat (drop one, pick one, clear)
    prefabRowProperty = "animation";
    if (propertyHeader("Animation", -1, !animator.animation.empty())){
        setVertexAnimatorFile(sceneProject, {entity}, "");
    }
    drawVertexAnimatorFileField(sceneProject, {entity}, animator.animation, false, stopped);

    // Clip: the default clip, with Preview
    prefabRowProperty = "defaultClip";
    if (propertyHeader("Clip", -1, !animator.defaultClip.empty())){
        CommandHandle::get(project->getSelectedSceneId())->addCommandNoMerge(
            new PropertyCmd<std::string>(project, sceneProject->id, entity, cpType, "defaultClip", std::string()));
    }
    {
        const bool previewing = state.previewEntity == entity && state.previewScene == sceneProject->id && animator.preview;
        const char* previewLabel = previewing ? ICON_FA_STOP "##vertexanimator_preview" : ICON_FA_PLAY "##vertexanimator_preview";
        const float previewWidth = iconButtonWidth(ICON_FA_STOP);
        ImGui::SetNextItemWidth(std::max(ImGui::GetContentRegionAvail().x - previewWidth - ImGui::GetStyle().ItemSpacing.x, 1.0f));
        const std::string current = animator.defaultClip.empty()
            ? (clipNames.empty() ? std::string("(first clip)") : clipNames[0] + " (first clip)") : animator.defaultClip;
        if (ImGui::BeginCombo("##vertexanimator_clip", current.c_str())){
            auto choose = [&](const std::string& clip){
                CommandHandle::get(project->getSelectedSceneId())->addCommandNoMerge(
                    new PropertyCmd<std::string>(project, sceneProject->id, entity, cpType, "defaultClip", clip));
                VertexAnimatorComponent* edited = scene->findComponent<VertexAnimatorComponent>(entity);
                if (edited && previewing){
                    // the preview shows the chosen clip now
                    VertexAnimatorRequest request;
                    request.kind = VertexAnimatorRequest::Kind::Play;
                    request.all = true;
                    request.clip = clip.empty() && !clipNames.empty() ? clipNames[0] : clip;
                    edited->requests.push_back(request);
                }else if (edited && edited->runtime){
                    // edit mode shows the new default clip from its start (a new path starts over:
                    // VertexAnimatorSystem compares runtime.path)
                    edited->runtime->path.clear();
                }
            };
            if (ImGui::Selectable("(first clip)", animator.defaultClip.empty())) choose(std::string());
            for (const std::string& name : clipNames){
                if (ImGui::Selectable(name.c_str(), name == animator.defaultClip) && name != animator.defaultClip) choose(name);
                UIProbe::item("vat.clip.option." + name);
            }
            if (!animator.defaultClip.empty() && std::find(clipNames.begin(), clipNames.end(), animator.defaultClip) == clipNames.end()){
                ImGui::TextDisabled("%s (not in the animation)", animator.defaultClip.c_str());
            }
            ImGui::EndCombo();
        }
        UIProbe::item("vat.clip");
        ImGui::SetItemTooltip("The clip Play Automatically starts (the first one when none is chosen)");
        ImGui::SameLine();
        ImGui::BeginDisabled(!stopped || animator.animation.empty());
        if (ImGui::Button(previewLabel)){
            if (previewing){
                stopVertexAnimatorPreview();
            }else{
                startVertexAnimatorPreview(sceneProject, entity);
            }
        }
        ImGui::EndDisabled();
        UIProbe::item("vat.preview");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)){
            ImGui::SetTooltip("%s", previewing ? "Stop the preview: the GameObject goes back to where it started"
                                               : "Preview: play the animation in the Scene view (edit mode only; stops on Play, a save or another selection)");
        }
    }

    endTable();

    // the time scrubber while previewing
    VertexAnimatorComponent* live = scene->findComponent<VertexAnimatorComponent>(entity);
    if (live && live->preview && state.previewEntity == entity && live->asset && live->runtime && live->state.a.clip >= 0){
        const jv::VatData& data = live->asset->getData();
        const int clipIndex = live->state.a.clip;
        if (clipIndex >= 0 && clipIndex < (int)data.clips.size()){
            const jv::VatClip& clip = data.clips[(size_t)clipIndex];
            const double tau = jv::vatTrackTau(live->state.a, live->runtime->entity.holdA, live->gpuNow);
            float time = inspectorvat::clipTimeAt(clip, tau);
            // a loop's end is its start: the slider stops just before it
            const float length = std::max(clip.wrap == ClipWrapMode::Loop ? clip.length() * 0.999f : clip.length(), 0.0001f);
            ImGui::SetNextItemWidth(-FLT_MIN);
            // the clip's name in the slider's format ('%' doubled)
            std::string format;
            for (char c : clip.name) format += c == '%' ? std::string("%%") : std::string(1, c);
            format += "  %.2f s";
            const bool scrubbed = ImGui::SliderFloat("##vertexanimator_scrub", &time, 0.0f, length, format.c_str());
            if (scrubbed){
                // every state moves by the same step: instances keep their offsets
                std::vector<double> taus;
                for (size_t i = 0; i < live->instanceStates.size() && i < live->runtime->instances.size(); i++){
                    taus.push_back(jv::vatTrackTau(live->instanceStates[i].a, live->runtime->instances[i].holdA, live->gpuNow));
                }
                const double target = inspectorvat::scrubTarget(clip, tau, time);
                const std::vector<double> shifted = inspectorvat::shiftedTimes(taus, tau, target);
                VertexAnimatorRequest request;
                request.kind = VertexAnimatorRequest::Kind::SetTime;
                request.value = (float)target;
                live->requests.push_back(request);
                for (size_t i = 0; i < shifted.size(); i++){
                    request.instance = (int)i;
                    request.value = (float)shifted[i];
                    live->requests.push_back(request);
                }
                scene->getSystem<VertexAnimatorSystem>()->applyRequests(entity);
                sceneProject->needUpdateRender = true;
            }
            UIProbe::item("vat.scrub");
            ImGui::SetItemTooltip("The preview's time in the current clip (drag to scrub; instances keep their offsets)");
        }
    }

    beginTable(cpType, labelSize, "vertexanimator_settings");
    propertyRow(RowPropertyType::Float, cpType, "speed", "Speed", sceneProject, entities, speedSettings);
    propertyRow(RowPropertyType::Bool, cpType, "randomStart", "Random Start", sceneProject, entities, randomSettings);
    propertyRow(RowPropertyType::Bool, cpType, "playAutomatically", "Play Automatically", sceneProject, entities, autoSettings);
    propertyRow(RowPropertyType::FloatPositive, cpType, "defaultFadeTime", "Default Fade", sceneProject, entities, fadeSettings);
    propertyRow(RowPropertyType::Bool, cpType, "instanceEvents", "Instance Events", sceneProject, entities, eventsSettings);
    endTable();

    // Attachments: GameObjects that follow a socket
    std::vector<std::string> sockets;
    const jv::VatData* socketData = asset ? &asset->getData() : (document ? &document->data() : nullptr);
    if (socketData){
        for (const jv::VatSocket& socket : socketData->sockets) sockets.push_back(socket.name);
    }
    ImGui::SeparatorText(("Attachments (" + std::to_string(animator.attachments.size()) + ")").c_str());
    if (animator.attachments.empty()){
        ImGui::TextDisabled(sockets.empty() ? "No sockets in this animation (bake sockets in the baker)." : "No attachments: + Add follows a socket.");
    }
    beginTable(cpType, getLabelSize("Socket 00"), "vertexanimator_attachments");
    for (size_t i = 0; i < animator.attachments.size() && i < MAX_ROWS; i++){
        const std::string prefix = "attachments[" + std::to_string(i) + "]";
        ImGui::PushID((int)i);
        propertyHeader("Socket " + std::to_string(i + 1));
        const float removeWidth = iconButtonWidth(ICON_FA_XMARK);
        ImGui::SetNextItemWidth(std::max(ImGui::GetContentRegionAvail().x - removeWidth - ImGui::GetStyle().ItemSpacing.x, 1.0f));
        const std::string socket = animator.attachments[i].socket;
        if (ImGui::BeginCombo("##attachment_socket", socket.empty() ? "(none)" : socket.c_str())){
            for (const std::string& name : sockets){
                if (ImGui::Selectable(name.c_str(), name == socket) && name != socket){
                    CommandHandle::get(project->getSelectedSceneId())->addCommandNoMerge(
                        new PropertyCmd<std::string>(project, sceneProject->id, entity, cpType, prefix + ".socket", name));
                }
            }
            ImGui::EndCombo();
        }
        UIProbe::item("vat.attachment." + std::to_string(i) + ".socket");
        if (!socket.empty() && std::find(sockets.begin(), sockets.end(), socket) == sockets.end()){
            ImGui::SetItemTooltip("'%s' is not a socket of this animation", socket.c_str());
        }
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_XMARK "##attachment_remove")){
            std::vector<VertexAnimatorAttachment> list = animator.attachments;
            list.erase(list.begin() + (std::ptrdiff_t)i);
            CommandHandle::get(project->getSelectedSceneId())->addCommandNoMerge(
                new PropertyCmd<std::vector<VertexAnimatorAttachment>>(project, sceneProject->id, entity, cpType, "attachments", list));
            ImGui::PopID();
            break;
        }
        ImGui::SetItemTooltip("Remove the attachment");
        ImGui::PopID();
        RowSettings entitySettings;
        entitySettings.child = true;
        entitySettings.excludeEntities = {entity};
        entitySettings.help = "The GameObject that follows the socket (a child of this one).";
        propertyRow(RowPropertyType::LocalEntity, cpType, prefix + ".entity", "GameObject", sceneProject, entities, entitySettings);
    }
    endTable();
    ImGui::BeginDisabled(!stopped);
    if (ImGui::Button(ICON_FA_PLUS " Add Attachment##vertexanimator_attach")){
        std::vector<VertexAnimatorAttachment> list = animator.attachments;
        VertexAnimatorAttachment added;
        added.socket = sockets.empty() ? std::string() : sockets[0];
        list.push_back(added);
        CommandHandle::get(project->getSelectedSceneId())->addCommandNoMerge(
            new PropertyCmd<std::vector<VertexAnimatorAttachment>>(project, sceneProject->id, entity, cpType, "attachments", list));
    }
    ImGui::EndDisabled();
    UIProbe::item("vat.addAttachment");
    ImGui::SetItemTooltip("A GameObject that follows one of the animation's sockets (a weapon in a hand)");

    // the load status
    if (!state.sectionError.empty()) errorText(state.sectionError);
    const VertexAnimatorComponent& status = *scene->findComponent<VertexAnimatorComponent>(entity);
    if (!status.loadError.empty()){
        warningText(status.loadError);
        UIProbe::item("vat.loadError");
        const std::string meshName = asset ? asset->getData().mesh : (document ? document->data().mesh : std::string());
        const std::string fix = inspectorvat::meshMismatchFix(status.loadError, status.animation, meshName);
        if (!fix.empty() && scene->findComponent<ModelComponent>(entity) && scene->findComponent<MeshComponent>(entity)){
            ImGui::BeginDisabled(!stopped);
            if (ImGui::Button((ICON_FA_WRENCH " " + inspectorvat::meshFixLabel(fix) + "##vertexanimator_meshfix").c_str())){
                // the Model field's own command: the mesh loads in place of the old model (undo puts it back)
                CommandHandle::get(project->getSelectedSceneId())->addCommandNoMerge(new ModelLoadCmd(project, sceneProject->id, entity, fix));
            }
            ImGui::EndDisabled();
            UIProbe::item("vat.meshFix");
            ImGui::SetItemTooltip("Set the Model to %s, the mesh this animation was baked for (one undo step)", fix.c_str());
        }
    }else if (!animator.animation.empty() && !asset && !documentError.empty() && !status.needsLoad){
        warningText(documentError);
    }
    if (asset){
        const std::string limit = inspectorvat::deviceLimitWarning(asset->getPayload().height, SystemRender::getFeatures().maxTextureSize2D,
                                                                   status.animation);
        if (!limit.empty()){
            warningText(limit);
            UIProbe::item("vat.deviceLimit");
        }
    }
}

// ---- the .vat asset view ----

void editor::InspectorWindow::drawVatAsset(const fs::path& asset){
    InspectorVatState& state = vatState();
    if (state.assetShown != asset){
        state.assetShown = asset;
        state.assetError.clear();
        state.eventsClipStart = -1;
        state.crowdResult.clear();
    }
    drawAssetHeader(ICON_FA_PEOPLE_GROUP, asset, "Vertex Animation");
    std::string error;
    std::shared_ptr<VatDocument> document = findDocument(project, state, asset.generic_string(), &error);
    if (!document){
        errorText(error);
        return;
    }
    const bool playing = project->isAnyScenePlaying();
    const jv::VatData& data = document->data();
    const VatAppHooks& hooks = vatAppHooks();

    auto commit = [&](const jv::VatData& newData, const std::string& undoName, const std::string& mergeKey){
        std::string commitError;
        if (!document->commit(newData, undoName, mergeKey, &commitError)){
            state.assetError = commitError.empty() ? std::string("The Vertex Animation could not be written") : commitError;
            Out::error("%s: %s", document->path().c_str(), state.assetError.c_str());
        }else{
            state.assetError.clear();
        }
    };

    // header: mode, size, texture, memory, error
    ImGui::TextUnformatted(inspectorvat::modeLine(data).c_str());
    UIProbe::item("vatAsset.mode");
    ImGui::TextUnformatted(inspectorvat::textureLine(data).c_str());
    UIProbe::item("vatAsset.texture");
    ImGui::TextDisabled("%s", inspectorvat::errorLine(data).c_str());
    const std::string limit = inspectorvat::deviceLimitWarning(data.layout.height, SystemRender::getFeatures().maxTextureSize2D,
                                                               document->path());
    if (!limit.empty()) warningText(limit);

    // staleness (the baker's checkVatStaleness: source hashes, the .vat.glb), on every new
    // revision and every few seconds (it hashes the sources)
    {
        InspectorVatState::StalenessEntry& entry = state.staleness[document->path()];
        const double now = ImGui::GetTime();
        if (entry.checkedAt < 0.0 || entry.revision != document->revision() || now - entry.checkedAt >= STALENESS_SECONDS){
            entry.issues = ev::checkVatStaleness(project, document->absolutePath().generic_string());
            entry.revision = document->revision();
            entry.checkedAt = now;
        }
        for (const ev::VatIssue& issue : entry.issues){
            warningText(inspectorvat::stalenessText(issue.kind, issue.message, issue.fix));
            UIProbe::item("vatAsset.stale");
        }
    }

    // actions
    const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    ImGui::BeginDisabled(playing || !hooks.rebake);
    if (ImGui::Button(ICON_FA_ROTATE " Rebake##vat_asset_rebake", ImVec2(half, 0))){
        hooks.rebake(project, document->absolutePath());
    }
    ImGui::EndDisabled();
    UIProbe::item("vatAsset.rebake");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)){
        ImGui::SetTooltip("%s", !hooks.rebake ? "Rebake needs the Vertex Animation Baker, which is not available in this build"
                               : playing ? "Stop the game to rebake"
                               : "Bake again from the recorded sources and settings (keeps the GUID, clip names, events, speeds and wraps)");
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(playing || !hooks.openBaker);
    if (ImGui::Button(ICON_FA_GEARS " Open in Baker##vat_asset_baker", ImVec2(half, 0))){
        hooks.openBaker(project, document->absolutePath(), VatBakerSource::VatAsset);
    }
    ImGui::EndDisabled();
    UIProbe::item("vatAsset.openBaker");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)){
        ImGui::SetTooltip("%s", !hooks.openBaker ? "The Vertex Animation Baker is not available in this build"
                                                 : "Open the Vertex Animation Baker with this animation's recipe");
    }
    ImGui::BeginDisabled(playing);
    if (ImGui::Button(ICON_FA_PEOPLE_GROUP " Create Crowd...##vat_asset_crowd", ImVec2(-FLT_MIN, 0))){
        state.crowdRequested = true;
        state.crowdVat = document->path();
    }
    ImGui::EndDisabled();
    UIProbe::item("vatAsset.createCrowd");
    ImGui::SetItemTooltip("Scatter instances of this animation over an area of the open scene, out of step (one GameObject, one undo step)");
    if (!state.crowdResult.empty()){
        if (state.crowdResultOk){
            ImGui::TextDisabled("%s", state.crowdResult.c_str());
        }else{
            errorText(state.crowdResult);
        }
    }
    drawCreateCrowdDialog();

    ImGui::BeginDisabled(playing);

    // clips: name, frames, fps, length, wrap, speed, interpolate, events, bytes
    ImGui::SeparatorText(("Clips (" + std::to_string(data.clips.size()) + ")").c_str());
    const ImGuiTableFlags tableFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollX |
                                       ImGuiTableFlags_SizingFixedFit;
    const float rowHeight = ImGui::GetFrameHeightWithSpacing();
    const float tableHeight = (std::min((float)data.clips.size(), 10.0f) + 1.0f) * rowHeight + ImGui::GetStyle().ScrollbarSize + 4.0f;
    if (!data.clips.empty() && ImGui::BeginTable("vat_asset_clips", 9, tableFlags, ImVec2(0, tableHeight))){
        ImGui::TableSetupScrollFreeze(1, 1);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, textWidth("MMMMMMM"));
        ImGui::TableSetupColumn("Wrap", ImGuiTableColumnFlags_WidthFixed, textWidth("Ping Pong") + ImGui::GetFrameHeight());
        ImGui::TableSetupColumn("Speed", ImGuiTableColumnFlags_WidthFixed, textWidth("00.00") + 8.0f);
        ImGui::TableSetupColumn("Interp.");
        ImGui::TableSetupColumn("Events");
        ImGui::TableSetupColumn("Frames");
        ImGui::TableSetupColumn("FPS");
        ImGui::TableSetupColumn("Length");
        ImGui::TableSetupColumn("Bytes");
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < data.clips.size() && i < MAX_ROWS; i++){
            const jv::VatClip& clip = data.clips[i];
            const std::string id = "vatAsset.clip." + std::to_string(i);
            ImGui::PushID((int)i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            std::string newName;
            if (textField(state, id + ".name", clip.name, newName, -FLT_MIN)){
                newName = trimmed(newName);
                jv::VatData edited = data;
                edited.clips[i].name = newName;
                commit(edited, "Rename Clip", "");
            }
            UIProbe::item(id + ".name");
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::BeginCombo("##wrap", inspectorvat::wrapLabel(clip.wrap))){
                for (ClipWrapMode wrap : {ClipWrapMode::Loop, ClipWrapMode::Once, ClipWrapMode::PingPong, ClipWrapMode::ClampForever}){
                    if (ImGui::Selectable(inspectorvat::wrapLabel(wrap), wrap == clip.wrap) && wrap != clip.wrap){
                        jv::VatData edited = data;
                        edited.clips[i].wrap = wrap;
                        commit(edited, "Clip Wrap", "");
                    }
                }
                ImGui::EndCombo();
            }
            UIProbe::item(id + ".wrap");
            ImGui::TableNextColumn();
            float speed = clip.speed;
            ImGui::SetNextItemWidth(-FLT_MIN);
            const bool speedChanged = ImGui::DragFloat("##speed", &speed, 0.01f, -10.0f, 10.0f, "%.3g");
            if (ImGui::IsItemActivated()) state.dragSerial++;
            if (speedChanged && std::isfinite(speed) && speed != clip.speed){
                jv::VatData edited = data;
                edited.clips[i].speed = speed;
                // one undo step per drag (or typed value)
                commit(edited, "Clip Speed", "speed#" + std::to_string(clip.start) + "#" + std::to_string(state.dragSerial));
            }
            UIProbe::item(id + ".speed");
            ImGui::SetItemTooltip("Multiplies the player's speed for this clip");
            ImGui::TableNextColumn();
            bool interpolate = clip.interpolate;
            if (ImGui::Checkbox("##interpolate", &interpolate)){
                jv::VatData edited = data;
                edited.clips[i].interpolate = interpolate;
                commit(edited, interpolate ? "Interpolate Clip" : "Step Clip", "");
            }
            UIProbe::item(id + ".interpolate");
            ImGui::SetItemTooltip("Blend between baked frames (off: each frame holds until the next, a stepped look)");
            ImGui::TableNextColumn();
            const bool open = state.eventsClipStart == clip.start;
            if (ImGui::Selectable((std::to_string(clip.events.size()) + (open ? " " ICON_FA_CARET_UP : " " ICON_FA_CARET_DOWN) + "##events").c_str(), open)){
                state.eventsClipStart = open ? -1 : clip.start;
            }
            UIProbe::item(id + ".events");
            ImGui::SetItemTooltip("The clip's events (functions called on the GameObject's scripts)");
            // the baked (read-only) columns after the editable ones
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%d", clip.frames);
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%.4g", clip.fps);
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%.2f s", clip.length());
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", inspectorvat::formatBytes(inspectorvat::clipBytes(data, clip)).c_str());
            ImGui::PopID();
        }
        ImGui::EndTable();
    }else if (data.clips.empty()){
        ImGui::TextDisabled("No clips: rebake to bake some.");
    }
    const std::string note = inspectorvat::removedClipsNote(data);
    if (!note.empty()){
        ImGui::TextDisabled(ICON_FA_CIRCLE_INFO " %s", note.c_str());
        UIProbe::item("vatAsset.removedNote");
    }

    // the events of the chosen clip
    const auto openClip = std::find_if(data.clips.begin(), data.clips.end(), [&](const jv::VatClip& c){ return c.start == state.eventsClipStart; });
    if (openClip != data.clips.end()){
        const size_t clipIndex = (size_t)(openClip - data.clips.begin());
        const jv::VatClip& clip = *openClip;
        ImGui::SeparatorText(("Events of " + clip.name).c_str());
        if (clip.events.empty()) ImGui::TextDisabled("No events: + Add Event calls a script function at a time of the clip.");
        if (!clip.events.empty() && ImGui::BeginTable("vat_asset_events", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)){
            ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, textWidth("000.000 s") + 8.0f);
            ImGui::TableSetupColumn("Function", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("Args", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, iconButtonWidth(ICON_FA_XMARK));
            ImGui::TableHeadersRow();
            for (size_t e = 0; e < clip.events.size() && e < MAX_ROWS; e++){
                const ClipEventData& event = clip.events[e];
                const std::string id = "vatAsset.event." + std::to_string(e);
                ImGui::PushID((int)e);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                float time = event.time;
                ImGui::SetNextItemWidth(-FLT_MIN);
                const bool timeChanged = ImGui::DragFloat("##time", &time, 0.01f, 0.0f, std::max(clip.length(), 0.0f), "%.3f s",
                                                          ImGuiSliderFlags_AlwaysClamp);
                if (ImGui::IsItemActivated()) state.dragSerial++;
                if (timeChanged && std::isfinite(time) && time != event.time){
                    jv::VatData edited = data;
                    edited.clips[clipIndex].events[e].time = time;
                    commit(edited, "Event Time", "event#" + std::to_string(clip.start) + "#" + std::to_string(state.dragSerial));
                }
                UIProbe::item(id + ".time");
                ImGui::TableNextColumn();
                std::string function;
                if (textField(state, id + ".function", event.function, function, -FLT_MIN)){
                    function = trimmed(function);
                    if (!inspectorvat::isLuaName(function)){
                        state.assetError = "'" + function + "' is not a function name (letters, digits and _, not starting with a digit)";
                    }else{
                        jv::VatData edited = data;
                        edited.clips[clipIndex].events[e].function = function;
                        commit(edited, "Event Function", "");
                    }
                }
                UIProbe::item(id + ".function");
                ImGui::TableNextColumn();
                std::string argsText;
                if (textField(state, id + ".args", inspectorvat::eventArgsText(event.args), argsText, -FLT_MIN)){
                    std::vector<AnimationEventArg> args;
                    std::string argsError;
                    if (!inspectorvat::parseEventArgs(argsText, args, &argsError)){
                        state.assetError = "Event args: " + argsError;
                    }else{
                        jv::VatData edited = data;
                        edited.clips[clipIndex].events[e].args = args;
                        commit(edited, "Event Args", "");
                    }
                }
                UIProbe::item(id + ".args");
                ImGui::SetItemTooltip("Arguments, comma separated: numbers, true/false, text (\"quoted\" when it has a comma)");
                ImGui::TableNextColumn();
                if (ImGui::Button(ICON_FA_XMARK "##remove")){
                    jv::VatData edited = data;
                    edited.clips[clipIndex].events.erase(edited.clips[clipIndex].events.begin() + (std::ptrdiff_t)e);
                    commit(edited, "Remove Event", "");
                    ImGui::PopID();
                    break;
                }
                UIProbe::item(id + ".remove");
                ImGui::SetItemTooltip("Remove the event");
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (ImGui::Button(ICON_FA_PLUS " Add Event##vat_asset_add_event")){
            jv::VatData edited = data;
            ClipEventData event;
            event.time = 0.0f;
            event.function = "onEvent";
            edited.clips[clipIndex].events.push_back(event);
            commit(edited, "Add Event", "");
        }
        UIProbe::item("vatAsset.addEvent");
        ImGui::SetItemTooltip("An event at 0 s calling onEvent(): set its time, function and arguments in the row");
    }

    // sockets: renamed here (the order and paths are the bake's)
    if (!data.sockets.empty()){
        ImGui::SeparatorText(("Sockets (" + std::to_string(data.sockets.size()) + ")").c_str());
        for (size_t s = 0; s < data.sockets.size() && s < MAX_ROWS; s++){
            ImGui::PushID((int)s);
            const std::string id = "vatAsset.socket." + std::to_string(s);
            std::string name;
            const float width = ImGui::GetContentRegionAvail().x * 0.45f;
            if (textField(state, id, data.sockets[s].name, name, width)){
                jv::VatData edited = data;
                edited.sockets[s].name = trimmed(name);
                commit(edited, "Rename Socket", "");
            }
            UIProbe::item(id);
            ImGui::SameLine();
            ImGui::TextDisabled("%s", data.sockets[s].path.c_str());
            ImGui::PopID();
        }
    }

    ImGui::EndDisabled();

    if (!state.assetError.empty()){
        errorText(state.assetError);
        UIProbe::item("vatAsset.error");
    }

    // warnings of the last load
    const std::vector<ClipWarning>& warnings = document->loadWarnings();
    if (!warnings.empty()){
        ImGui::SeparatorText(("Warnings (" + std::to_string(warnings.size()) + ")").c_str());
        for (size_t w = 0; w < warnings.size() && w < MAX_ROWS; w++){
            warningText(warnings[w].path + (warnings[w].path.empty() ? "" : ": ") + warnings[w].message);
        }
    }
}

// ---- Create Crowd ----

void editor::InspectorWindow::drawCreateCrowdDialog(){
    InspectorVatState& state = vatState();
    const char* const popup = "Create Crowd##vat_crowd";
    std::string error;
    std::shared_ptr<VatDocument> document = state.crowdVat.empty() ? nullptr : findDocument(project, state, state.crowdVat, &error);
    SceneProject* sceneProject = project->getSelectedScene();
    // the selected GameObject a crowd can stand on
    Entity surface = NULL_ENTITY;
    std::string surfaceName;
    if (sceneProject && sceneProject->scene){
        const std::vector<Entity> selected = project->getSelectedEntities(sceneProject->id);
        if (selected.size() == 1 && sceneProject->scene->findComponent<Transform>(selected[0])){
            surface = selected[0];
            surfaceName = sceneProject->scene->getEntityName(surface);
        }
    }

    if (state.crowdRequested){
        state.crowdRequested = false;
        if (document){
            const jv::VatData& data = document->data();
            state.crowd = ev::CrowdSettings();
            state.crowd.count = 100;
            state.crowd.minSpacing = 0.6f;
            state.crowdWeights.assign(data.clips.size(), 0.0f);
            if (!state.crowdWeights.empty()) state.crowdWeights[0] = 1.0f;
            state.crowdOnSurface = surface != NULL_ENTITY;
            if (surface != NULL_ENTITY){
                const Transform& transform = sceneProject->scene->getComponent<Transform>(surface);
                state.crowd.center = transform.worldPosition;
            }
            state.crowdResult.clear();
            ImGui::OpenPopup(popup);
        }
    }

    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 26.0f, 0.0f), ImGuiCond_Appearing);
    if (!EditorModal::begin(popup, nullptr, ImGuiWindowFlags_AlwaysAutoResize)){
        return;
    }
    if (!document){
        errorText(error.empty() ? std::string("The Vertex Animation is gone") : error);
        EditorModal::message(error.empty() ? std::string("The Vertex Animation is gone") : error);
        if (EditorModal::button("Close", "ok", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    const jv::VatData& data = document->data();
    ev::CrowdSettings& crowd = state.crowd;
    if (state.crowdWeights.size() != data.clips.size()) state.crowdWeights.resize(data.clips.size(), 0.0f);
    const float field = ImGui::GetFontSize() * 12.0f;

    ImGui::TextDisabled("%s into %s", fs::path(document->path()).filename().string().c_str(),
                        sceneProject ? sceneProject->name.c_str() : "(no scene)");
    EditorModal::message("Create a crowd of " + fs::path(document->path()).filename().string() + " in " +
                         (sceneProject ? sceneProject->name : std::string("(no scene)")) + ".");
    ImGui::SetNextItemWidth(field);
    EditorModal::inputInt("Count##crowd_count", "count", &crowd.count, 10, 100);
    crowd.count = std::clamp(crowd.count, 1, ev::MAX_CROWD);
    UIProbe::item("crowd.count");

    int area = crowd.area == ev::CrowdArea::Disc ? 1 : 0;
    if (EditorModal::enumField("area", area, {"Box", "Disc"})) crowd.area = area == 1 ? ev::CrowdArea::Disc : ev::CrowdArea::Box;
    ImGui::SetNextItemWidth(field);
    if (ImGui::Combo("Area##crowd_area", &area, "Box\0Disc\0")){
        crowd.area = area == 1 ? ev::CrowdArea::Disc : ev::CrowdArea::Box;
    }
    UIProbe::item("crowd.area");
    ImGui::SetNextItemWidth(field);
    if (crowd.area == ev::CrowdArea::Box){
        EditorModal::field("width", crowd.width);
        EditorModal::field("depth", crowd.depth);
        float size[2] = {crowd.width, crowd.depth};
        if (ImGui::DragFloat2("Size (X, Z)##crowd_size", size, 0.1f, 0.0f, 10000.0f, "%.1f m")){
            crowd.width = std::max(size[0], 0.0f);
            crowd.depth = std::max(size[1], 0.0f);
        }
    }else{
        EditorModal::field("diameter", crowd.width);
        ImGui::DragFloat("Diameter##crowd_size", &crowd.width, 0.1f, 0.0f, 10000.0f, "%.1f m");
        crowd.width = std::max(crowd.width, 0.0f);
    }
    UIProbe::item("crowd.size");
    ImGui::SetNextItemWidth(field);
    float center[3] = {crowd.center.x, crowd.center.y, crowd.center.z};
    if (ImGui::DragFloat3("Center##crowd_center", center, 0.1f)){
        crowd.center = Vector3(center[0], center[1], center[2]);
    }
    ImGui::SetItemTooltip("The area's center in the scene (the crowd GameObject's position)");

    // the ground: the selected GameObject's surface or the plane through the center
    if (surface != NULL_ENTITY) EditorModal::field("on_selected_surface", state.crowdOnSurface);
    if (ImGui::RadioButton("Ground plane##crowd_plane", !state.crowdOnSurface)) state.crowdOnSurface = false;
    ImGui::SetItemTooltip("Instances stand on the horizontal plane through the center");
    ImGui::SameLine();
    ImGui::BeginDisabled(surface == NULL_ENTITY);
    const std::string surfaceLabel = surface == NULL_ENTITY ? std::string("Selected surface") : "On " + surfaceName;
    if (ImGui::RadioButton((surfaceLabel + "##crowd_surface").c_str(), state.crowdOnSurface && surface != NULL_ENTITY)) state.crowdOnSurface = true;
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)){
        ImGui::SetTooltip("%s", surface == NULL_ENTITY ? "Select one GameObject (a ground mesh or a terrain) in the scene to stand the crowd on it"
                                                       : "Each instance is dropped onto this GameObject's surface (where it misses: the plane)");
    }

    ImGui::SetNextItemWidth(field);
    EditorModal::field("min_spacing", crowd.minSpacing);
    ImGui::DragFloat("Min Spacing##crowd_spacing", &crowd.minSpacing, 0.01f, 0.0f, 100.0f, "%.2f m");
    crowd.minSpacing = std::max(crowd.minSpacing, 0.0f);
    UIProbe::item("crowd.spacing");
    EditorModal::checkbox("Random Yaw##crowd_yaw", "random_yaw", &crowd.randomYaw);
    ImGui::SetNextItemWidth(field);
    ImGui::DragFloatRange2("Scale##crowd_scale", &crowd.scaleMin, &crowd.scaleMax, 0.01f, 0.01f, 100.0f, "%.2f", "%.2f");
    ImGui::SetNextItemWidth(field);
    ImGui::DragFloatRange2("Speed##crowd_speed", &crowd.speedMin, &crowd.speedMax, 0.01f, -10.0f, 10.0f, "%.2f", "%.2f");
    ImGui::SetItemTooltip("Each instance's own speed multiplier, picked in this range");

    ImGui::SeparatorText("Clips (weights)");
    for (size_t i = 0; i < data.clips.size() && i < MAX_ROWS; i++){
        ImGui::PushID((int)i);
        ImGui::SetNextItemWidth(field);
        EditorModal::field(("weight_" + data.clips[i].name).c_str(), state.crowdWeights[i]);
        ImGui::DragFloat(data.clips[i].name.c_str(), &state.crowdWeights[i], 0.5f, 0.0f, 1000.0f, "%.1f");
        state.crowdWeights[i] = std::max(state.crowdWeights[i], 0.0f);
        UIProbe::item("crowd.weight." + std::to_string(i));
        ImGui::PopID();
    }
    if (data.clips.empty()) ImGui::TextDisabled("No clips");

    EditorModal::checkbox("Random Start##crowd_random_start", "random_start", &crowd.randomStart);
    ImGui::SetItemTooltip("Each instance starts at a random time of its clip (out of step)");
    ImGui::SameLine();
    EditorModal::checkbox("Instance Events##crowd_events", "instance_events", &crowd.instanceEvents);
    ImGui::SetItemTooltip("Fire clip events for every instance (costs work per instance each frame)");
    int seed = (int)crowd.seed;
    ImGui::SetNextItemWidth(field);
    if (EditorModal::inputInt("Seed##crowd_seed", "seed", &seed)) crowd.seed = (uint32_t)std::max(seed, 0);
    UIProbe::item("crowd.seed");

    const bool canCreate = sceneProject && sceneProject->scene && sceneProject->sceneType == SceneType::SCENE_3D &&
                           sceneProject->playState == ScenePlayState::STOPPED;
    EditorModal::error(!state.crowdResult.empty() && !state.crowdResultOk ? state.crowdResult :
                       canCreate ? std::string() : std::string("Open a 3D scene (and stop the game) to create a crowd in it"));
    ImGui::Separator();
    ImGui::BeginDisabled(!canCreate);
    if (EditorModal::button(ICON_FA_PEOPLE_GROUP " Create##crowd_create", "ok", ImVec2(140, 0))){
        crowd.vat = document->path();
        crowd.surface = state.crowdOnSurface ? surface : NULL_ENTITY;
        crowd.clipWeights.clear();
        for (size_t i = 0; i < data.clips.size(); i++){
            if (state.crowdWeights[i] > 0.0f) crowd.clipWeights.push_back({data.clips[i].name, state.crowdWeights[i]});
        }
        if (crowd.clipWeights.size() == 1 && crowd.clipWeights[0].clip == data.clips[0].name){
            crowd.clipWeights.clear();      // every instance plays the default clip: nothing per instance
        }
        const ev::CrowdResult result = ev::createCrowd(project, sceneProject->id, crowd);
        state.crowdResult = result.message;
        state.crowdResultOk = result.ok;
        if (result.ok){
            Out::info("Create Crowd: %s", result.message.c_str());
            // the new GameObject is shown (the asset view gives way to the selection)
            project->clearInspectedAsset();
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::EndDisabled();
    UIProbe::item("crowd.create");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && !canCreate){
        ImGui::SetTooltip("Open a 3D scene (and stop the game) to create a crowd in it");
    }
    ImGui::SameLine();
    if (EditorModal::button("Cancel##crowd_cancel", "cancel", ImVec2(120, 0))){
        ImGui::CloseCurrentPopup();
    }
    if (!state.crowdResult.empty() && !state.crowdResultOk){
        errorText(state.crowdResult);
    }
    ImGui::EndPopup();
}
