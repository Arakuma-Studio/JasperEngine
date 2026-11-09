// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TextureImporterView.h"
#include "ImporterFields.h"

#include "Backend.h"
#include "Project.h"
#include "asset/AssetFileWrite.h"
#include "command/CommandHistory.h"
#include "command/type/SetAssetMetaCmd.h"
#include "external/IconsFontAwesome6.h"
#include "imgui_internal.h"
#include "texture/TextureCookReport.h"
#include "texture/TextureDecode.h"
#include "EditorHost.h"
#include "pixelart/PixelArtAssign.h"
#include "pixelart/PixelArtFix.h"
#include "pixelart/cleanup/CleanupImportCook.h"
#include "pixelart/cleanup/CleanupRecipe.h"
#include "Out.h"

#include "stb_image_resize2.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

    // the longest side the preview keeps
    constexpr int kImporterPreviewMax = 1024;

    // decoded pixels as display RGBA8: normals get Z back, single channels are grey, HDR clamped
    bool importerToRgba(GpuTextureFormat format, int width, int height, const std::vector<uint8_t>& pixels, std::vector<uint8_t>& out){
        const size_t texels = (size_t)width * (size_t)height;
        if (format == GpuTextureFormat::RGBA8){
            out = pixels;
            return out.size() >= texels * 4;
        }
        if (format == GpuTextureFormat::RGBA16F)
            return TextureDecode::convert(format, GpuTextureFormat::RGBA8, width, height, pixels.data(), pixels.size(), out);
        out.resize(texels * 4);
        for (size_t i = 0; i < texels; i++){
            uint8_t r = 0, g = 0, b = 0;
            if (format == GpuTextureFormat::RG8){
                r = pixels[i * 2];
                g = pixels[i * 2 + 1];
                const float x = r / 127.5f - 1.0f, y = g / 127.5f - 1.0f;
                b = (uint8_t)std::lround((std::sqrt(std::max(0.0f, 1.0f - x * x - y * y)) * 0.5f + 0.5f) * 255.0f);
            }else if (format == GpuTextureFormat::R16){
                r = g = b = pixels[i * 2 + 1];
            }else if (format == GpuTextureFormat::R8){
                r = g = b = pixels[i];
            }else{
                return false;
            }
            out[i * 4] = r;
            out[i * 4 + 1] = g;
            out[i * 4 + 2] = b;
            out[i * 4 + 3] = 255;
        }
        return true;
    }

    // a level at most kImporterPreviewMax on its longest side
    void importerFit(int& width, int& height, std::vector<uint8_t>& rgba){
        if (std::max(width, height) <= kImporterPreviewMax)
            return;
        const float scale = (float)kImporterPreviewMax / (float)std::max(width, height);
        const int w = std::max(1, (int)std::lround(width * scale)), h = std::max(1, (int)std::lround(height * scale));
        std::vector<uint8_t> out((size_t)w * h * 4);
        stbir_resize_uint8_srgb(rgba.data(), width, height, 0, out.data(), w, h, 0, STBIR_RGBA);
        rgba.swap(out);
        width = w;
        height = h;
    }

    const std::vector<std::string>& importerTypeLabels(){
        static const std::vector<std::string> labels = {"Default", "Normal Map", "Sprite (2D)", "UI", "Lightmap", "Single Channel", "HDR"};
        return labels;
    }

    std::vector<std::string> importerLabels(const std::vector<std::string>& names){
        std::vector<std::string> labels;
        for (const std::string& name : names){
            std::string label = name;
            for (char& c : label)
                if (c == '_') c = ' ';
            if (!label.empty())
                label[0] = (char)std::toupper((unsigned char)label[0]);
            if (name == "nearest")
                label = "Nearest (Point)";
            labels.push_back(label);
        }
        return labels;
    }

}

struct TextureImporterView::PreviewJob {
    jobs::Task<PreviewImage> task;
    int levels = 1;
};

TextureImporterView::TextureImporterView(Project* project): project(project) {
    // the prompt for edits not applied when the selection, the editor or Play moves on (S53 R17)
    ImporterPrompt::add(this, {[this](){ return hasPendingEdits(); },
                               [this](){ return assetPaths.size() == 1 ? fs::path(assetPaths[0]).filename().string()
                                                                      : std::to_string(assetPaths.size()) + " textures"; },
                               [this](){ apply(); }, [this](){ revert(); }});
}

TextureImporterView::~TextureImporterView(){
    ImporterPrompt::remove(this);
    previewTexture.destroy();
}

std::vector<TextureFamily> TextureImporterView::shownFamilies() const{
    std::vector<TextureFamily> families = {TextureFamily::Desktop, TextureFamily::Mobile, TextureFamily::Web};
    if (project->getTextureSettings().androidEtc2Fallback)
        families.push_back(TextureFamily::AndroidEtc2);
    return families;
}

TextureFamily TextureImporterView::tabFamily() const{
    const std::vector<TextureFamily> families = shownFamilies();
    return tab > 0 && tab <= (int)families.size() ? families[(size_t)tab - 1] : TextureFamily::Desktop;
}

TextureImportSettings TextureImporterView::workingSettings() const{
    TextureImportSettings settings;
    TextureImportSettings::parse(working, settings);
    return settings;
}

YAML::Node TextureImporterView::workingValue(const std::string& key) const{
    return importer::get(working, key);
}

void TextureImporterView::setWorking(const std::string& key, const YAML::Node& value){
    importer::set(working, key, value);
    touched.insert(key);
    mixed.erase(key);
}

void TextureImporterView::select(const std::vector<fs::path>& selection){
    std::vector<std::string> assets, projects;
    for (const fs::path& path : selection){
        const fs::path absolute = (project->getProjectPath() / path).lexically_normal();
        if (!project->isInsideAssetsPath(absolute))
            continue;
        const std::string assetPath = project->normalizeToAssetsRelative(absolute).generic_string();
        if (isTextureSource(assetPath)){
            assets.push_back(assetPath);
            projects.push_back(project->normalizeToProjectRelative(absolute).generic_string());
        }
    }
    std::string key;
    for (const std::string& path : assets)
        key += path + "\n";
    // another selection with edits not applied: the prompt decides, the edited files stay shown
    // (Cancel keeps them until the selection moves again)
    if (key != selectionKey && !touched.empty()){
        if (key != promptedKey){
            promptedKey = key;
            ImporterPrompt::guard(nullptr);
        }
        return;
    }
    promptedKey.clear();
    // the stored settings changed under us (undo, another tool): read them again
    bool changed = key != selectionKey;
    if (!changed && touched.empty()){
        TextureCookQueue& queue = TextureCookQueue::get();
        for (size_t i = 0; i < assetPaths.size() && !changed; i++){
            TextureImportSettings now;
            const bool has = queue.getProject() == project ? queue.settingsOf(assetPaths[i], now)
                                                           : TextureImportSettings::load((project->getAssetsPath() / assetPaths[i]).string(), now);
            changed = has != (bool)stored[i] || (has && !(now == current[i]));
        }
    }
    if (!changed)
        return;
    const bool sameSelection = key == selectionKey;
    selectionKey = key;
    assetPaths = assets;
    projectPaths = projects;
    stored.clear();
    current.clear();
    mixed.clear();
    touched.clear();
    inferredReason.clear();
    if (!sameSelection){
        tab = 0;
        mip = 0;
    }
    for (size_t i = 0; i < assetPaths.size(); i++){
        const fs::path absolute = project->getAssetsPath() / assetPaths[i];
        const YAML::Node section = SetAssetMetaCmd::read(absolute, TextureImportSettings::metaKey);
        TextureCookInputs inputs;
        if (section && section.IsMap()){
            TextureImportSettings::parse(section, inputs.settings);
        }else{
            gatherTextureInputs(*project, assetPaths[i], inputs);
            if (i == 0)
                inferredReason = inputs.inference.reason;
        }
        stored.push_back(section ? YAML::Clone(section) : YAML::Node());
        current.push_back(inputs.settings);
    }
    if (assetPaths.empty())
        return;
    working = stored[0] ? YAML::Clone(stored[0]) : current[0].serialize();
    // keys whose values differ between the selected textures show as mixed
    std::set<std::string> keys;
    std::vector<YAML::Node> sections;
    for (size_t i = 0; i < assetPaths.size(); i++){
        sections.push_back(stored[i] ? stored[i] : current[i].serialize());
        importer::leaves(sections.back(), "", keys);
    }
    for (const std::string& leaf : keys){
        const std::string first = importer::dump(importer::get(sections[0], leaf));
        for (size_t i = 1; i < sections.size(); i++){
            if (importer::dump(importer::get(sections[i], leaf)) != first){
                mixed.insert(leaf);
                break;
            }
        }
    }
    // the platform table and the preview want every family's cook of the first one
    TextureCookQueue& queue = TextureCookQueue::get();
    if (queue.getProject() == project && isCookableTexture(assetPaths[0]))
        for (TextureFamily family : shownFamilies())
            queue.request(assetPaths[0], family, TextureCookQueue::Selected);
}

void TextureImporterView::revert(){
    touched.clear();
    selectionKey.clear();
}

void TextureImporterView::apply(){
    if (touched.empty())
        return;
    CommandHistory* history = project->getProjectCommandHistory();
    InteractiveEdit edit = history->beginInteractive("Set Texture Import Settings");
    for (size_t i = 0; i < assetPaths.size(); i++){
        YAML::Node node = stored[i] ? YAML::Clone(stored[i]) : current[i].serialize();
        for (const std::string& key : touched)
            importer::set(node, key, workingValue(key));
        // an explicit type ends the inference's say
        if (touched.count("type"))
            node.remove("inferredFrom");
        TextureImportSettings parsed;
        if (!TextureImportSettings::parse(node, parsed))
            continue;
        Command* command = new SetAssetMetaCmd(project, project->normalizeToProjectRelative(project->getAssetsPath() / assetPaths[i]),
                                               TextureImportSettings::metaKey, parsed.serialize());
        if (command->isNoOp()){
            delete command;
            continue;
        }
        command->setNoMerge();
        history->addCommand(command);
    }
    edit.commit();
    touched.clear();
    selectionKey.clear();
}

bool TextureImporterView::comboField(const char* label, const std::string& key, const std::vector<std::string>& names,
                                     const std::vector<std::string>& labels, const std::string& shown, const char* tooltip){
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    if (tooltip && ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tooltip);
    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(-1);
    const bool isMixed = mixed.count(key) > 0;
    size_t index = 0;
    for (size_t i = 0; i < names.size(); i++)
        if (names[i] == shown) index = i;
    bool changed = false;
    const std::string id = std::string("##") + key;
    if (isMixed) ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, true);
    if (ImGui::BeginCombo(id.c_str(), isMixed ? "\xE2\x80\x94" : labels[index].c_str())){
        for (size_t i = 0; i < names.size(); i++){
            if (ImGui::Selectable(labels[i].c_str(), i == index && !isMixed)){
                setWorking(key, YAML::Node(names[i]));
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    if (isMixed) ImGui::PopItemFlag();
    return changed;
}

bool TextureImporterView::boolField(const char* label, const std::string& key, bool shown, const char* tooltip){
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    if (tooltip && ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tooltip);
    ImGui::TableSetColumnIndex(1);
    const bool isMixed = mixed.count(key) > 0;
    bool value = shown;
    const std::string id = std::string("##") + key;
    if (isMixed) ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, true);
    const bool changed = ImGui::Checkbox(id.c_str(), &value);
    if (isMixed) ImGui::PopItemFlag();
    if (changed)
        setWorking(key, YAML::Node(value));
    return changed;
}

void TextureImporterView::drawGeneral(const ResolvedTextureSettings& r){
    const std::vector<std::string>& types = textureEnumValues("type");
    comboField("Texture Type", "type", types, importerTypeLabels(), textureImportTypeName(r.type),
               "What the texture is: it picks the format, the colour space and the mip chain.");
    // S42 R4, R7, R20: Pixel Art and what an assignment copies
    const TextureImportSettings settings = workingSettings();
    boolField("Pixel Art", "pixelArt", settings.resolvedPixelArt(),
              "Point filtering, no mipmaps, lossless on every platform, Nearest resizing and binary alpha (Alpha Clip) for "
              "the fields left at their defaults.");
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Presets");
    ImGui::TableSetColumnIndex(1);
    if (ImGui::SmallButton("Presets##TexturePresets"))
        ImGui::OpenPopup("##TextureImportPresets");
    if (ImGui::BeginPopup("##TextureImportPresets")){
        if (ImGui::MenuItem("Pixel Art")){
            // the switch on, and the fields that would contradict it cleared (one undo step at Apply)
            setWorking("pixelArt", YAML::Node(true));
            for (const char* key : {"mipmaps", "mipFilter", "resizeFilter", "quality", "streamable", "alphaIsTransparency", "alphaClip", "filterMode"})
                setWorking(key, YAML::Node());
            for (size_t f = 0; f < kTextureFamilyCount; f++){
                const std::string prefix = std::string("platforms.") + textureFamilyName((TextureFamily)f) + ".";
                setWorking(prefix + "format", YAML::Node());
                setWorking(prefix + "quality", YAML::Node());
            }
        }
        ImGui::EndPopup();
    }
    comboField("Filter Mode", "filterMode", textureEnumValues("filterMode"), {"Point (no filter)", "Bilinear", "Trilinear"},
               textureFilterModeName(settings.resolvedFilterMode(project->getDefaultFilterMode())),
               "What a reference to this texture starts with when you assign it (the reference keeps its own afterwards).");
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Pixels Per Unit");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("What a Sprite or Tilemap takes when this texture is assigned (the project's: %d).", project->getPixelsPerUnit());
    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(-1);
    int ppu = settings.resolvedPixelsPerUnit(project->getPixelsPerUnit());
    if (ImGui::InputInt("##pixelsPerUnit", &ppu, 0))
        setWorking("pixelsPerUnit", YAML::Node(std::clamp(ppu, 1, 256)));
    boolField("Alpha Clip", "alphaClip", r.alphaClip, "Alpha becomes fully opaque at the cutoff and above, fully transparent below.");
    if (r.alphaClip && !r.preserveCoverage){
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Alpha Cutoff");
        ImGui::TableSetColumnIndex(1);
        ImGui::SetNextItemWidth(-1);
        float cutoff = r.alphaCutoff;
        if (ImGui::SliderFloat("##alphaClipCutoff", &cutoff, 0.0f, 1.0f, "%.2f"))
            setWorking("alphaCutoff", YAML::Node(cutoff));
    }
    comboField("Texture Shape", "shape", textureEnumValues("shape"), {"2D", "Cube"}, textureShapeName(r.shape),
               "Cube: one file with the six faces as a 4:3 or 3:4 cross (skyboxes, reflection probes).");
    boolField("sRGB (Color Texture)", "srgb", r.srgb, "Off for data (masks, roughness, normals): sampled linearly.");
    comboField("Alpha Source", "alphaSource", textureEnumValues("alphaSource"), {"Input Texture Alpha", "From Gray Scale", "None"},
               alphaSourceName(r.alphaSource), nullptr);
    boolField("Alpha Is Transparency", "alphaIsTransparency", r.alphaIsTransparency,
              "Fills the colour of transparent texels from their neighbours, so filtering shows no dark fringes.");
    if (r.type == TextureImportType::SingleChannel)
        comboField("Channel", "channel", textureEnumValues("channel"), {"Red", "Green", "Blue", "Alpha", "Luminance"},
                   textureChannelName(r.channel), "The source channel a single-channel texture keeps.");
    if (r.type == TextureImportType::Normal)
        boolField("Flip Green Channel", "flipGreen", r.flipGreen, "For DirectX-style (Y down) normal maps.");
}

void TextureImporterView::drawAdvanced(const ResolvedTextureSettings& r){
    boolField("Read/Write", "readable", r.readable,
              "Keeps the pixels readable on the CPU (Lua getData): stored uncompressed. Terrain height and density "
              "maps, LUTs and spot masks are readable without it.");
    boolField("Streamable", "streamable", r.streamable,
              "The texture quality setting and the memory budget may drop its top mips.");
    const YAML::Node ship = workingValue("shipSource");
    boolField("Ship Source File", "shipSource", ship && ship.as<bool>(false),
              "Exports keep the source file next to the cooked one (code that reads the file itself).");
    comboField("Resize Filter", "resizeFilter", textureEnumValues("resizeFilter"), importerLabels(textureEnumValues("resizeFilter")),
               mipFilterName(r.resizeFilter), "The filter that shrinks a source larger than Max Size.");
}

void TextureImporterView::drawMipMaps(const ResolvedTextureSettings& r){
    boolField("Generate Mip Maps", "mipmaps", r.mipmaps, nullptr);
    if (!r.mipmaps)
        return;
    comboField("Mip Filter", "mipFilter", textureEnumValues("mipFilter"), importerLabels(textureEnumValues("mipFilter")),
               mipFilterName(r.mipFilter), nullptr);
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Mip Limit");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The most levels kept (0: all of them).");
    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(-1);
    int limit = r.mipLimit;
    if (mixed.count("mipLimit")) ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, true);
    if (ImGui::SliderInt("##mipLimit", &limit, 0, 16))
        setWorking("mipLimit", YAML::Node(limit));
    if (mixed.count("mipLimit")) ImGui::PopItemFlag();
    boolField("Preserve Coverage", "preserveCoverage", r.preserveCoverage,
              "Keeps the alpha-tested area of every mip (foliage, fences) at the source's.");
    if (r.preserveCoverage){
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Alpha Cutoff");
        ImGui::TableSetColumnIndex(1);
        ImGui::SetNextItemWidth(-1);
        float cutoff = r.alphaCutoff;
        if (ImGui::SliderFloat("##alphaCutoff", &cutoff, 0.0f, 1.0f, "%.2f"))
            setWorking("alphaCutoff", YAML::Node(cutoff));
    }
}

void TextureImporterView::drawPlatform(const ResolvedTextureSettings& r){
    static const std::vector<std::string> sizes = {"32", "64", "128", "256", "512", "1024", "2048", "4096", "8192", "16384"};
    const bool platformTab = tab > 0;
    const std::string prefix = platformTab ? std::string("platforms.") + textureFamilyName(tabFamily()) + "." : std::string();
    bool overrides = true;
    if (platformTab){
        const YAML::Node value = workingValue(prefix + "override");
        overrides = value && value.as<bool>(false);
        const std::string label = std::string("Override for ") + textureFamilyLabel(tabFamily());
        boolField(label.c_str(), prefix + "override", overrides, "This platform's own max size, format, quality and effort.");
    }
    ImGui::BeginDisabled(!overrides);
    comboField("Max Size", prefix + "maxSize", sizes, sizes, std::to_string(r.maxSize),
               "A larger source is shrunk to this on its longest side (Resize Filter).");
    if (platformTab){
        std::vector<std::string> formats = {""};
        std::vector<std::string> labels = {"Automatic"};
        for (const std::string& name : TextureFormatSelector::formatNames()){
            formats.push_back(name);
            labels.push_back(name);
        }
        const YAML::Node current = workingValue(prefix + "format");
        const std::string shown = current ? current.as<std::string>("") : std::string();
        if (comboField("Format", prefix + "format", formats, labels, shown, "Automatic follows the type and quality (R5).") &&
            workingValue(prefix + "format").as<std::string>("").empty())
            setWorking(prefix + "format", YAML::Node());
    }
    comboField("Compression", prefix + "quality", textureEnumValues("quality"), {"None", "Low Quality", "Normal Quality", "High Quality"},
               textureQualityName(r.quality), "None keeps the texture uncompressed.");
    comboField("Encoder Effort", prefix + "effort", textureEnumValues("effort"), {"Fast", "Normal", "Best"}, textureEffortName(r.effort),
               "More effort: better quality for the same size, slower cooks.");
    if (platformTab){
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("RDO / ETC1S Level");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Rate-distortion lambda of BC7 and UASTC (smaller files, more error); ETC1S: its quality level 1-255. "
                              "Negative: the table's.");
        ImGui::TableSetColumnIndex(1);
        ImGui::SetNextItemWidth(-1);
        float rdo = r.rdo;
        if (ImGui::DragFloat("##rdo", &rdo, 0.05f, -1.0f, 255.0f, rdo < 0.0f ? "default" : "%.2f"))
            setWorking(prefix + "rdo", rdo < 0.0f ? YAML::Node() : YAML::Node(rdo));
    }
    ImGui::EndDisabled();
}

void TextureImporterView::drawPlatformTable(){
    if (!ImGui::BeginTable("texture_platforms", 5, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp))
        return;
    ImGui::TableSetupColumn("Platform");
    ImGui::TableSetupColumn("Format");
    ImGui::TableSetupColumn("VRAM");
    ImGui::TableSetupColumn("Disk");
    ImGui::TableSetupColumn("Status");
    ImGui::TableHeadersRow();
    TextureCookQueue& queue = TextureCookQueue::get();
    for (TextureFamily family : shownFamilies()){
        TextureCookState state;
        const bool known = queue.getState(assetPaths[0], family, state);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(textureFamilyLabel(family));
        ImGui::TableSetColumnIndex(1);
        if (state.status == TextureCookStatus::Cooked)
            ImGui::TextUnformatted(state.record.label.c_str());
        else if (!state.plan.key.empty())
            ImGui::TextDisabled("%s", state.plan.choice.label().c_str());
        ImGui::TableSetColumnIndex(2);
        if (state.status == TextureCookStatus::Cooked)
            ImGui::TextUnformatted(TextureCookReport::formatBytes(state.record.gpuBytes).c_str());
        ImGui::TableSetColumnIndex(3);
        if (state.status == TextureCookStatus::Cooked)
            ImGui::TextUnformatted(TextureCookReport::formatBytes(state.record.diskBytes).c_str());
        ImGui::TableSetColumnIndex(4);
        const char* status = !known ? "not cooked" : state.stale ? "stale" : textureCookStatusName(state.status);
        if (state.status == TextureCookStatus::Failed){
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.35f, 1.0f), ICON_FA_TRIANGLE_EXCLAMATION " failed");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", state.error.c_str());
        }else if (state.status == TextureCookStatus::Cooked && state.record.psnr > 0.0){
            ImGui::Text("%s \xC2\xB7 %.1f dB", status, state.record.psnr);
        }else{
            ImGui::TextUnformatted(status);
        }
    }
    ImGui::EndTable();
}

void TextureImporterView::updatePreviewJobs(const TextureCookState* state){
    const fs::path absolute = project->getAssetsPath() / assetPaths[0];
    const bool cube = workingSettings().shape.value_or(TextureShape::Tex2D) == TextureShape::Cube;
    // the source at the shown mip
    std::error_code ec;
    const auto written = fs::last_write_time(absolute, ec);
    const std::string sKey = assetPaths[0] + "|" + std::to_string(mip) + "|" + (cube ? "c" : "2") + "|" +
                             std::to_string(ec ? 0 : (long long)written.time_since_epoch().count());
    if (sKey != sourceKey){
        sourceKey = sKey;
        auto job = std::make_shared<PreviewJob>();
        const int level = mip;
        jobs::SubmitOptions options;
        options.lane = jobs::Lane::Background;
        options.name = "texture-preview";
        job->task = jobs::submit([absolute, cube, level](){
            PreviewImage image;
            std::array<TextureData, 6> faces;
            if (TextureCooker::decodeSource(absolute, cube, faces, image.error) == 0)
                return image;
            std::vector<uint8_t> rgba;
            if (!faces[0].toRgba8(rgba)){
                image.error = "the source cannot be shown";
                return image;
            }
            int w = faces[0].getWidth(), h = faces[0].getHeight();
            for (TextureData& face : faces)
                face.releaseImageData();
            // the mip as a box-filtered level of the source
            const int lw = mipDimension(w, level), lh = mipDimension(h, level);
            if (lw != w || lh != h){
                std::vector<uint8_t> out((size_t)lw * lh * 4);
                stbir_resize_uint8_srgb(rgba.data(), w, h, 0, out.data(), lw, lh, 0, STBIR_RGBA);
                rgba.swap(out);
                w = lw;
                h = lh;
            }
            importerFit(w, h, rgba);
            image.width = w;
            image.height = h;
            image.rgba = std::move(rgba);
            return image;
        }, options);
        sourceJob = job;
    }
    // the cooked level of the tab's family
    const std::string cKey = state && state->status == TextureCookStatus::Cooked ? state->record.key + "|" + std::to_string(mip) : std::string();
    if (cKey != cookedKey){
        cookedKey = cKey;
        cooked = PreviewImage();
        cookedJob.reset();
        if (!cKey.empty()){
            auto job = std::make_shared<PreviewJob>();
            const fs::path file = TextureCookQueue::get().getCache().artifactPath(state->record.key);
            const int level = mip;
            jobs::SubmitOptions options;
            options.lane = jobs::Lane::Background;
            options.name = "texture-preview";
            job->task = jobs::submit([file, level](){
                PreviewImage image;
                std::string text;
                std::vector<uint8_t> bytes;
                if (!assetio::readFile(file, text)){
                    image.error = "the cooked file is missing";
                    return image;
                }
                bytes.assign(text.begin(), text.end());
                GpuTextureFormat format;
                int w = 0, h = 0, faces = 1;
                std::vector<uint8_t> pixels;
                if (!decodeKtx2ForPreview(bytes, (uint32_t)level, format, w, h, faces, pixels, image.error))
                    return image;
                std::vector<uint8_t> rgba;
                // face 0 of a cube
                if (!importerToRgba(format, w, h, pixels, rgba)){
                    image.error = std::string(gpuFormatName(format)) + " cannot be shown";
                    return image;
                }
                rgba.resize((size_t)w * h * 4);
                importerFit(w, h, rgba);
                image.width = w;
                image.height = h;
                image.rgba = std::move(rgba);
                return image;
            }, options);
            cookedJob = job;
        }
    }
    if (sourceJob && sourceJob->task.isReady()){
        try { source = sourceJob->task.get(); } catch (const std::exception& e) { source = PreviewImage(); source.error = e.what(); }
        sourceJob.reset();
        compositeKey.clear();
    }
    if (cookedJob && cookedJob->task.isReady()){
        try { cooked = cookedJob->task.get(); } catch (const std::exception& e) { cooked = PreviewImage(); cooked.error = e.what(); }
        cookedJob.reset();
        compositeKey.clear();
    }
}

void TextureImporterView::composite(){
    char key[96];
    snprintf(key, sizeof(key), "%d%d%d%d|%.3f|%zu|%zu", showChannel[0], showChannel[1], showChannel[2], showChannel[3], split,
             source.rgba.size(), cooked.rgba.size());
    if (compositeKey == key)
        return;
    compositeKey = key;
    // the cooked size wins (what ships); the source is resampled to it
    const PreviewImage& base = !cooked.rgba.empty() ? cooked : source;
    if (base.rgba.empty()){
        previewTexture.destroy();
        previewWidth = previewHeight = 0;
        return;
    }
    const int w = base.width, h = base.height;
    std::vector<uint8_t> left;
    if (!source.rgba.empty()){
        if (source.width == w && source.height == h){
            left = source.rgba;
        }else{
            left.resize((size_t)w * h * 4);
            stbir_resize_uint8_srgb(source.rgba.data(), source.width, source.height, 0, left.data(), w, h, 0, STBIR_RGBA);
        }
    }
    const std::vector<uint8_t>& right = !cooked.rgba.empty() ? cooked.rgba : left;
    const int splitX = !cooked.rgba.empty() && !left.empty() ? (int)std::lround(split * w) : (left.empty() ? 0 : w);
    const size_t bytes = (size_t)w * h * 4;
    unsigned char* out = static_cast<unsigned char*>(std::malloc(bytes));
    if (!out)
        return;
    const int cell = std::max(4, std::max(w, h) / 32);
    for (int y = 0; y < h; y++){
        for (int x = 0; x < w; x++){
            const size_t i = ((size_t)y * w + x) * 4;
            const uint8_t* p = (x < splitX ? left : right).data() + i;
            const bool onlyAlpha = showChannel[3] && !showChannel[0] && !showChannel[1] && !showChannel[2];
            float c[3];
            if (onlyAlpha){
                c[0] = c[1] = c[2] = p[3] / 255.0f;
            }else{
                for (int k = 0; k < 3; k++)
                    c[k] = showChannel[k] ? p[k] / 255.0f : 0.0f;
            }
            // under alpha, a checkerboard
            const float a = (showChannel[3] && !onlyAlpha) ? p[3] / 255.0f : 1.0f;
            const float checker = (((x / cell) + (y / cell)) & 1) ? 0.8f : 0.55f;
            for (int k = 0; k < 3; k++)
                out[i + k] = (uint8_t)std::lround(std::clamp(c[k] * a + checker * (1.0f - a), 0.0f, 1.0f) * 255.0f);
            out[i + 3] = 255;
            // the split line
            if (splitX > 0 && splitX < w && (x == splitX || x == splitX - 1)){
                out[i] = 255;
                out[i + 1] = 210;
                out[i + 2] = 60;
            }
        }
    }
    previewTexture.destroy();
    TextureData data(w, h, (unsigned int)bytes, ColorFormat::RGBA, 4, out);
    // the copies the constructor makes must not free the pixels: the texture's own data owns them
    data.setDataOwned(false);
    previewTexture = Texture("editor-texture-importer-preview-" + std::to_string(++previewSerial), data);
    previewTexture.getData().setDataOwned(true);
    previewTexture.setMinFilter(TextureFilter::LINEAR);
    previewTexture.setMagFilter(TextureFilter::NEAREST);
    previewTexture.load();
    previewWidth = w;
    previewHeight = h;
}

void TextureImporterView::drawPreview(const TextureCookState* state){
    updatePreviewJobs(state);
    composite();
    const float avail = ImGui::GetContentRegionAvail().x;
    if (previewWidth > 0 && previewHeight > 0){
        if (TextureRender* render = previewTexture.getRender()){
            float scale = std::min(avail / previewWidth, 256.0f / previewHeight);
            // pixel art (S42 R20): whole-number zoom with nearest sampling, or 1x
            if (workingSettings().resolvedPixelArt() && scale >= 1.0f)
                scale = previewOneX ? 1.0f : std::floor(scale);
            const ImVec2 size(previewWidth * scale, previewHeight * scale);
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail - size.x) * 0.5f);
            ImGui::Image(Backend::getImGuiTexture(render), size);
        }
    }else if (!source.error.empty()){
        ImGui::TextDisabled("%s", source.error.c_str());
    }else{
        ImGui::TextDisabled("Loading preview...");
    }
    static const char* const channelNames[] = {"R", "G", "B", "A"};
    for (int c = 0; c < 4; c++){
        if (c > 0) ImGui::SameLine();
        const bool on = showChannel[c];
        if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::SmallButton(channelNames[c]))
            showChannel[c] = !showChannel[c];
        if (on) ImGui::PopStyleColor();
    }
    if (workingSettings().resolvedPixelArt()){
        ImGui::SameLine();
        if (previewOneX) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::SmallButton("1x"))
            previewOneX = !previewOneX;
        if (previewOneX) ImGui::PopStyleColor();
        ImGui::SetItemTooltip("One texel per screen pixel");
    }
    ImGui::SameLine();
    const int levels = state && state->status == TextureCookStatus::Cooked ? std::max(1, state->record.levels) : 1;
    mip = std::min(mip, levels - 1);
    ImGui::SetNextItemWidth(std::max(60.0f, ImGui::GetContentRegionAvail().x * 0.4f));
    ImGui::SliderInt("##mip", &mip, 0, levels - 1, "Mip %d");
    if (!cooked.rgba.empty() && !source.rgba.empty()){
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1);
        ImGui::SliderFloat("##split", &split, 0.0f, 1.0f, "Source | Cooked");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Left of the line: the source. Right: the %s cook, decoded on the CPU.",
                              textureFamilyLabel(tabFamily()));
    }
}

bool TextureImporterView::draw(const std::vector<fs::path>& selection){
    select(selection);
    if (assetPaths.empty())
        return false;

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(ICON_FA_IMAGE);
    ImGui::SameLine();
    if (assetPaths.size() == 1)
        ImGui::TextUnformatted(fs::path(assetPaths[0]).filename().string().c_str());
    else
        ImGui::Text("%zu textures", assetPaths.size());
    ImGui::TextDisabled("Texture Import Settings");
    ImGui::Separator();

    const bool cookable = isCookableTexture(assetPaths[0]);
    if (!cookable){
        ImGui::TextWrapped("%s loads as it is (vector images rasterize at load, KTX2 files are already GPU formats).",
                           fs::path(assetPaths[0]).extension().string().c_str());
        return true;
    }

    const TextureFamily family = tabFamily();
    TextureCookState state;
    const bool hasState = TextureCookQueue::get().getState(assetPaths[0], family, state);
    drawPreview(hasState ? &state : nullptr);
    if (hasState)
        ImGui::TextWrapped("%s", TextureCookReport::infoLine(state).c_str());
    else
        ImGui::TextDisabled("Not cooked yet");

    // the tabs: Default, then each family
    const std::vector<TextureFamily> families = shownFamilies();
    if (ImGui::BeginTabBar("texture_importer_tabs")){
        if (ImGui::BeginTabItem("Default")){
            tab = 0;
            ImGui::EndTabItem();
        }
        for (size_t i = 0; i < families.size(); i++){
            if (ImGui::BeginTabItem(textureFamilyLabel(families[i]))){
                tab = (int)i + 1;
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }

    const TextureImportSettings settings = workingSettings();
    const ResolvedTextureSettings resolved = settings.resolve(family, textureProjectSettingsOf(*project));
    const ImGuiTableFlags flags = ImGuiTableFlags_SizingStretchProp;
    auto section = [&](const char* title, auto&& body){
        if (ImGui::CollapsingHeader(title, ImGuiTreeNodeFlags_DefaultOpen)){
            if (ImGui::BeginTable(title, 2, flags)){
                ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthStretch, 0.45f);
                ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 0.55f);
                body();
                ImGui::EndTable();
            }
        }
    };
    if (tab == 0){
        if (!stored[0] && !inferredReason.empty())
            ImGui::TextDisabled("Inferred: %s", inferredReason.c_str());
        else if (!settings.inferredFrom.empty())
            ImGui::TextDisabled("Inferred: %s", settings.inferredFrom.c_str());
        section("General", [&]{ drawGeneral(resolved); });
        section("Advanced", [&]{ drawAdvanced(resolved); });
        section("Mip Maps", [&]{ drawMipMaps(resolved); });
    }
    section(tab == 0 ? "Compression (all platforms)" : "Platform", [&]{ drawPlatform(resolved); });

    // the warnings of this platform: references that do not fit, R6, clamping
    if (hasState){
        for (const std::string& warning : state.warnings)
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), ICON_FA_TRIANGLE_EXCLAMATION " %s", warning.c_str());
        if (state.status == TextureCookStatus::Failed)
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.35f, 1.0f), ICON_FA_TRIANGLE_EXCLAMATION " %s", state.error.c_str());
    }
    drawPixelArtNotes(settings);

    ImGui::Separator();
    drawPlatformTable();

    if (assetPaths.size() == 1) {
        ImGui::Separator();
        drawSpriteMode(assetPaths[0]);
        ImGui::Separator();
        drawPixelArtCleanup(assetPaths[0]);
    }

    ImGui::Separator();
    ImGui::BeginDisabled(touched.empty());
    if (ImGui::Button("Revert"))
        revert();
    ImGui::SameLine();
    if (ImGui::Button("Apply"))
        apply();
    ImGui::EndDisabled();
    return true;
}

void TextureImporterView::reloadCleanupSection(const std::string& assetPath){
    const fs::path absolute = project->getAssetsPath() / fs::path(assetPath);
    std::error_code ec;
    const fs::file_time_type time = fs::last_write_time(SetAssetMetaCmd::metaPathOf(absolute), ec);
    const std::string key = assetPath + "|" + (ec ? std::string() : std::to_string(time.time_since_epoch().count()));
    if (key == cleanupSectionKey)
        return;
    cleanupSectionKey = key;
    cleanupSection = SetAssetMetaCmd::read(absolute, "import.pixelCleanup");
    cleanupSummary.clear();
    const std::string text = pixelart::cleanup::importSectionRecipeText(cleanupSection);
    if (text.empty())
        return;
    pixelart::cleanup::Recipe recipe;
    std::vector<pixelart::PixelProblem> problems;
    if (!pixelart::cleanup::decodeRecipeText(text, recipe, problems)){
        cleanupSummary = problems.empty() ? std::string("The recipe could not be read.") : problems.front().message;
        return;
    }
    cleanupSummary = std::string("Grid ") + pixelart::cleanup::gridKindName(recipe.grid.kind);
    if (recipe.palette && !recipe.palette->colors.empty())
        cleanupSummary += ", palette " + (recipe.palette->name.empty() ? std::to_string(recipe.palette->colors.size()) + " colors" : recipe.palette->name);
    else if (recipe.autoPalette)
        cleanupSummary += ", palette auto (" + std::to_string(recipe.autoPalette->count) + ")";
    if (recipe.targetWidth)
        cleanupSummary += ", width " + std::to_string((int)*recipe.targetWidth);
    if (recipe.removeBackground)
        cleanupSummary += ", no background";
    if (!recipe.source.sha256.empty())
        cleanupSummary += ", saved from this file";
}

void TextureImporterView::drawPixelArtCleanup(const std::string& assetPath){
    const std::string lower = [&](){
        std::string name = fs::path(assetPath).extension().string();
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c){ return (char)std::tolower(c); });
        return name;
    }();
    // .aseprite, .svg and .ktx2 never take the step
    if (!isCookableTexture(assetPath) || lower == ".aseprite" || lower == ".ase")
        return;
    if (!ImGui::CollapsingHeader("Pixel Art Cleanup", ImGuiTreeNodeFlags_DefaultOpen))
        return;
    reloadCleanupSection(assetPath);
    const bool hasSection = cleanupSection && cleanupSection.IsMap();
    bool enabled = hasSection && (!cleanupSection["enabled"] || cleanupSection["enabled"].as<bool>(false));
    if (ImGui::Checkbox("Enabled", &enabled)){
        YAML::Node node = hasSection ? YAML::Clone(cleanupSection) : YAML::Node(YAML::NodeType::Map);
        node["enabled"] = enabled;
        auto* command = new SetAssetMetaCmd(project, project->normalizeToProjectRelative(project->getAssetsPath() / assetPath), "import.pixelCleanup", node);
        command->setAction(enabled ? "Enable Pixel Art Cleanup" : "Disable Pixel Art Cleanup");
        command->setNoMerge();
        project->getProjectCommandHistory()->addCommand(command);
        cleanupSectionKey.clear();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Cook the fixed pixels instead of the source. The source file stays as it is.");
    ImGui::SameLine();
    if (ImGui::SmallButton("Open in Cleanup Window"))
        getEditorHost().openPixelArtCleanup(project->normalizeToProjectRelative(project->getAssetsPath() / assetPath));
    if (!cleanupSummary.empty())
        ImGui::TextDisabled("%s", cleanupSummary.c_str());
    TextureCookState state;
    if (enabled && TextureCookQueue::get().getState(assetPath, TextureFamily::Desktop, state) && state.status == TextureCookStatus::Cooked){
        const std::string& stepState = state.plan.analysis.stepState;
        if (stepState == "applied")
            ImGui::TextColored(ImVec4(0.3f, 0.72f, 0.36f, 1.0f), ICON_FA_CIRCLE_CHECK " Fixed pixels cooked");
        else if (stepState == "guarded")
            ImGui::TextColored(ImVec4(0.9f, 0.64f, 0.2f, 1.0f), ICON_FA_TRIANGLE_EXCLAMATION " Cleanup needs a person (%s): the raw source was cooked", state.plan.analysis.stepGuard.c_str());
        else if (stepState == "failed")
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.35f, 1.0f), ICON_FA_TRIANGLE_EXCLAMATION " Cleanup did not run: %s", state.plan.analysis.stepGuard.c_str());
    }
}

void TextureImporterView::drawPixelArtNotes(const TextureImportSettings& settings){
    if (assetPaths.size() != 1)
        return;
    const ImVec4 warning(1.0f, 0.75f, 0.3f, 1.0f);
    if (settings.resolvedPixelArt()){
        const TextureProjectSettings projectSettings = textureProjectSettingsOf(*project);
        std::string lossy;
        for (size_t f = 0; f < kTextureFamilyCount; f++){
            const ResolvedTextureSettings r = settings.resolve((TextureFamily)f, projectSettings);
            const bool block = r.quality != TextureCookQuality::None || (!r.format.empty() && r.format != "RGBA8");
            if (block || r.mipmaps)
                lossy += std::string(lossy.empty() ? "" : ", ") + textureFamilyLabel((TextureFamily)f);
        }
        if (!lossy.empty())
            ImGui::TextColored(warning, ICON_FA_TRIANGLE_EXCLAMATION " mipmaps_compression: this pixel-art texture is compressed or "
                               "mipmapped on %s (an override wins over Pixel Art): colours shift and edges smear.", lossy.c_str());
        if (previewWidth * previewHeight > 2048 * 2048 || (source.width > 2048 && source.height > 2048))
            ImGui::TextColored(warning, ICON_FA_TRIANGLE_EXCLAMATION " Over 2048x2048 and uncompressed (4 bytes a texel): a sheet this "
                               "big is better split (a Sprite Atlas packs the parts).");
    }
    // only where a Filter Mode is meant: a pixel-art texture, one whose Filter Mode is set, or a
    // Pixel Art Mode project (elsewhere references choose their own filters)
    TextureImportSettings stored;
    TextureImportSettings::load((project->getAssetsPath() / fs::path(assetPaths[0])).string(), stored);
    if (!stored.resolvedPixelArt() && !stored.filterMode && !project->getPixelArtSettings().enabled){
        referenceCount = 0;
        return;
    }
    // the open scenes' references, read again twice a second rather than every frame
    const double now = ImGui::GetTime();
    if (referenceChangesPath != assetPaths[0] || now - referenceChangesAt > 0.5){
        referenceCount = pixelart::referenceFilterChanges(*project, assetPaths[0]).size();
        referenceChangesPath = assetPaths[0];
        referenceChangesAt = now;
    }
    if (referenceCount > 0){
        const size_t count = referenceCount;
        const std::string label = "Apply to " + std::to_string(count) + " Reference" + (count == 1 ? "" : "s");
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("%zu reference(s) in the open scenes are not filtered as this Filter Mode.", count);
        ImGui::PopStyleColor();
        if (ImGui::SmallButton(label.c_str())){
            // what the references are now, not the cached list
            const std::vector<pixelart::FixChange> changes = pixelart::referenceFilterChanges(*project, assetPaths[0]);
            pixelart::FixApplyReport report;
            std::string error;
            if (!changes.empty() &&
                !pixelart::applyChanges(*project, changes, "Apply Filter Mode to " + std::to_string(changes.size()) + " References", report,
                                        error, nullptr))
                Out::warning("Apply to References: %s", error.c_str());
            referenceChangesAt = -1.0;
        }
    }
}
