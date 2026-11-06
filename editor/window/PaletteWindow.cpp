// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "PaletteWindow.h"

#include "Backend.h"
#include "Out.h"
#include "Project.h"
#include "Theme.h"
#include "command/CommandHistory.h"
#include "external/IconsFontAwesome6.h"
#include "htmlui/HttpClient.h"
#include "pixelart/ApplyPalette.h"
#include "pixelart/BuiltinPalettes.h"
#include "pixelart/Lospec.h"
#include "pixelart/PaletteEdit.h"
#include "pixelart/PixelColor.h"
#include "shortcuts/CommandCatalog.h"
#include "texture/TextureData.h"
#include "util/FileDialogs.h"
#include "AppSettings.h"
#include "util/FileUtils.h"
#include "util/ProjectFileWrites.h"
#include "util/Util.h"
#include "widget/ColorField.h"
#include "widget/ColorMath.h"
#include "widget/ColorPickerWindow.h"
#include "widget/SwatchLibrary.h"
#include "window/ApplyPaletteDialog.h"
#include "window/ProjectWindow.h"

#include "imgui_internal.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <mutex>
#include <numeric>
#include <thread>
#include <utility>
#include <variant>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;
namespace pxc = jasperengine::pixelart;

namespace jasperengine::editor {

    // Lospec's offline answer before any request (R8): the Offline preference
    // (preferences.network.offline), or no curl and no recorded mirror
    bool paletteNetworkOffline();

}

bool editor::paletteNetworkOffline() {
    // the Offline preference first; then curl, unless a recorded mirror answers (fetchLospec reads it)
    if (AppSettings::isNetworkOffline()) return true;
    return htmlui::HttpClient::mirrorDirectory().empty() && !htmlui::HttpClient::available();
}

// file-local, in a namespace of its own: unity builds merge the editor's sources
namespace jasperengine::editor::palettewindow {
namespace {

    constexpr const char* kSwatchPayload = "PALETTE_SWATCHES";
    constexpr const char* kRampStepPayload = "PALETTE_RAMP_STEP";

    struct RampStepPayload {
        int ramp = -1;
        int step = -1;
    };

    std::string readText(const fs::path& path) {
        std::ifstream in(path, std::ios::binary);
        if (!in) return "";
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }

    bool readBytes(const fs::path& path, std::vector<uint8_t>& out) {
        std::ifstream in(path, std::ios::binary);
        if (!in) return false;
        out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        return true;
    }

    std::string lowerExtension(const fs::path& path) {
        std::string extension = path.extension().string();
        for (char& c : extension) c = (char)std::tolower((unsigned char)c);
        return extension;
    }

    bool insideOf(const fs::path& path, const fs::path& root) {
        if (root.empty()) return false;
        const fs::path relative = path.lexically_normal().lexically_relative(root.lexically_normal());
        return !relative.empty() && *relative.begin() != "..";
    }

    // a file name of the text (the characters a file name cannot hold become '_')
    std::string fileNameOf(const std::string& text, const char* fallback) {
        std::string name;
        for (char c : text) name += std::strchr("/\\:*?\"<>|", c) || (unsigned char)c < 32 ? '_' : c;
        while (!name.empty() && (name.back() == ' ' || name.back() == '.')) name.pop_back();
        while (!name.empty() && name.front() == ' ') name.erase(name.begin());
        return name.empty() ? std::string(fallback) : name;
    }

    std::string hexOf(pxc::Rgba8 color) {
        return pxc::toHex(color);
    }

    // "120", "1.2k", "34k", "1.0M": a badge's room
    std::string compactCount(int64_t count) {
        char text[32];
        if (count < 1000) std::snprintf(text, sizeof(text), "%lld", (long long)count);
        else if (count < 10000) std::snprintf(text, sizeof(text), "%.1fk", (double)count / 1000.0);
        else if (count < 1000000) std::snprintf(text, sizeof(text), "%lldk", (long long)(count / 1000));
        else std::snprintf(text, sizeof(text), "%.1fM", (double)count / 1000000.0);
        return text;
    }

    ImU32 imColor(pxc::Rgba8 color) {
        return IM_COL32(color.r, color.g, color.b, color.a);
    }

    // black or white, whichever reads on the colour
    ImU32 contrastOn(pxc::Rgba8 color) {
        return color.a < 128 || pxc::toOklab(color).l > 0.62f ? IM_COL32(0, 0, 0, 255) : IM_COL32(255, 255, 255, 255);
    }

    void drawChecker(ImDrawList* drawList, ImVec2 min, ImVec2 max) {
        const float cell = std::max(3.0f, Theme::dpi(4.0f));
        drawList->AddRectFilled(min, max, IM_COL32(200, 200, 200, 255));
        for (float y = min.y; y < max.y; y += cell) {
            for (float x = min.x; x < max.x; x += cell) {
                if (((int)((x - min.x) / cell) + (int)((y - min.y) / cell)) % 2 != 0) continue;
                drawList->AddRectFilled(ImVec2(x, y), ImVec2(std::min(x + cell, max.x), std::min(y + cell, max.y)), IM_COL32(128, 128, 128, 255));
            }
        }
    }

    void drawColor(ImDrawList* drawList, ImVec2 min, ImVec2 max, pxc::Rgba8 color) {
        if (color.a < 255) drawChecker(drawList, min, max);
        drawList->AddRectFilled(min, max, imColor(color));
    }

    // "same as base": diagonal hatching
    void drawHatch(ImDrawList* drawList, ImVec2 min, ImVec2 max) {
        drawList->AddRectFilled(min, max, IM_COL32(60, 60, 60, 255));
        drawList->PushClipRect(min, max, true);
        const float h = max.y - min.y;
        const float step = std::max(4.0f, Theme::dpi(5.0f));
        for (float x = min.x - h; x < max.x; x += step) {
            drawList->AddLine(ImVec2(x, max.y), ImVec2(x + h, min.y), IM_COL32(110, 110, 110, 255), 1.0f);
        }
        drawList->PopClipRect();
    }

    void smallText(ImDrawList* drawList, ImVec2 at, ImU32 color, const char* text) {
        drawList->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 0.72f, at, color, text);
    }

    ImVec2 smallTextSize(const char* text) {
        return ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize() * 0.72f, FLT_MAX, 0.0f, text);
    }

    // FNV-1a over the colours: the usage key changes when they do (a rename does not)
    uint64_t colorsHash(const std::vector<pxc::Rgba8>& colors) {
        uint64_t hash = 1469598103934665603ull;
        for (const pxc::Rgba8& color : colors) {
            for (uint8_t byte : {color.r, color.g, color.b, color.a}) {
                hash ^= byte;
                hash *= 1099511628211ull;
            }
        }
        return hash;
    }

    bool isApplicableTexture(const fs::path& path) {
        const std::string extension = lowerExtension(path);
        return extension == ".png" || extension == ".bmp" || extension == ".tga" || extension == ".gif";
    }

    std::vector<int> payloadIndices(const ImGuiPayload* payload) {
        std::vector<int> indices;
        if (!payload || !payload->Data || payload->DataSize <= 0) return indices;
        indices.resize((size_t)payload->DataSize / sizeof(int));
        std::memcpy(indices.data(), payload->Data, indices.size() * sizeof(int));
        return indices;
    }

    std::string uniqueName(const std::string& prefix, const std::function<bool(const std::string&)>& taken) {
        for (int n = 1;; n++) {
            const std::string name = prefix + " " + std::to_string(n);
            if (!taken(name)) return name;
        }
    }

}
}

namespace pwh = jasperengine::editor::palettewindow;

// ---------------------------------------------------------------------------------- workers

struct editor::PaletteWindow::LospecFetch {
    htmlui::HttpClient client;
    std::string slug;
    std::atomic<bool> done{false};
    std::atomic<bool> cancelled{false};
    std::optional<pxc::Palette> palette;
    pixelart::PaletteError error;
};

struct editor::PaletteWindow::UsageCache {
    static constexpr size_t kEntries = 16;
    std::mutex mutex;
    std::map<std::pair<uint64_t, uint64_t>, pixelart::PaletteUsageReport> reports;
};

// ---------------------------------------------------------------------------------- GPU images

bool editor::PaletteGpuImage::upload(const jasperengine::editor::pixelart::RgbaImage& image, std::vector<Texture>& retired) {
    static_assert(sizeof(pxc::Rgba8) == 4, "RGBA8 pixels are 4 bytes, tightly packed");
    retire(retired);
    if (image.empty() || !image.valid()) return false;
    static uint64_t counter = 0;
    const size_t bytes = image.pixelCount() * 4;
    // TextureData frees what it is given (stbi_image_free is free)
    void* pixels = std::malloc(bytes);
    if (!pixels) return false;
    std::memcpy(pixels, image.pixels.data(), bytes);
    TextureData data(image.width, image.height, static_cast<unsigned int>(bytes), ColorFormat::RGBA, 4, pixels);
    texture.setData("editor:palette-image:" + std::to_string(++counter), data);
    texture.setMinFilter(TextureFilter::NEAREST);
    texture.setMagFilter(TextureFilter::NEAREST);
    TextureRender* render = texture.getRender();
    texture.releaseData();
    if (!render || !render->isCreated()) {
        retire(retired);
        return false;
    }
    width = image.width;
    height = image.height;
    return true;
}

void editor::PaletteGpuImage::retire(std::vector<Texture>& retired) {
    if (!texture.empty()) retired.push_back(texture);
    texture = Texture();
    width = 0;
    height = 0;
}

ImTextureID editor::PaletteGpuImage::imguiId() {
    return Backend::getImGuiTexture(texture.getRender());
}

// ---------------------------------------------------------------------------------- shared

bool editor::PaletteWindow::writeFiles(Project* project, std::vector<WriteFilesCmd::Item> items, const std::string& stepName,
                                       const std::string& mergeKey, std::string& error) {
    if (!project) {
        error = "No project is open to write into.";
        return false;
    }
    const fs::path root = project->getProjectPath();
    bool inside = true;
    for (const WriteFilesCmd::Item& item : items) {
        std::error_code ec;
        if (fs::is_directory(item.path, ec)) {
            error = FileUtils::pathToGenericUtf8(item.path) + " is a folder.";
            return false;
        }
        inside = inside && pwh::insideOf(item.path, root);
    }
    auto failure = std::make_shared<std::string>();
    WriteFilesCmd* cmd = new WriteFilesCmd(std::move(items), [project](const std::vector<fs::path>& changed) {
        ProjectFileWrites::filesWritten(project, changed);
    });
    if (!stepName.empty()) cmd->setName(stepName);
    // a drag: one undo step for all its writes
    if (mergeKey.empty()) cmd->setNoMerge();
    else cmd->setMergeKey(mergeKey);
    cmd->reportFailureTo(failure);
    // the project's files never through a link out of it; an export elsewhere beneath its own folder
    if (inside) cmd->writeBeneath(root);
    else if (cmd->writeRoot().empty()) cmd->writeBeneathOwnFolders();
    if (!project->getProjectCommandHistory()->addCommand(cmd)) {
        error = "Writing the file failed: " +
                (failure->empty() ? std::string("permissions, a read-only disk, or a parent path that is a file") : *failure) + ".";
        return false;
    }
    return true;
}

bool editor::PaletteWindow::isPaletteFile(const fs::path& path) {
    return pwh::lowerExtension(path) == EXTENSION;
}

bool editor::PaletteWindow::isForeignPaletteFile(const fs::path& path) {
    const std::string extension = pwh::lowerExtension(path);
    return extension == ".gpl" || extension == ".hex" || extension == ".pal" || extension == ".txt" || extension == ".ase";
}

const std::vector<jasperengine::editor::pixelart::PaletteFormat>& editor::PaletteWindow::exportFormats() {
    using F = pixelart::PaletteFormat;
    static const std::vector<F> formats = [] {
        std::vector<F> all;
        for (F format : {F::Gpl, F::Hex, F::JascPal, F::RiffPal, F::AdobeAse, F::PaintNetTxt, F::PngStrip, F::SwatchLibrary}) {
            if (pixelart::canExportPalette(format)) all.push_back(format);
        }
        return all;
    }();
    return formats;
}

const char* editor::PaletteWindow::exportLabel(jasperengine::editor::pixelart::PaletteFormat format) {
    switch (format) {
        case pixelart::PaletteFormat::Gpl: return "GIMP (.gpl)";
        case pixelart::PaletteFormat::Hex: return "Hex (.hex)";
        case pixelart::PaletteFormat::JascPal: return "JASC (.pal)";
        case pixelart::PaletteFormat::RiffPal: return "RIFF (.pal)";
        case pixelart::PaletteFormat::AdobeAse: return "Adobe (.ase)";
        case pixelart::PaletteFormat::PaintNetTxt: return "Paint.NET (.txt)";
        case pixelart::PaletteFormat::PngStrip: return "PNG Strip (.png)";
        case pixelart::PaletteFormat::SwatchLibrary: return "Swatch Library (.colors)";
        case pixelart::PaletteFormat::Aseprite: return "Aseprite";
        case pixelart::PaletteFormat::Image: return "Image";
    }
    return "Palette";
}

// ---------------------------------------------------------------------------------- lifetime

editor::PaletteWindow::PaletteWindow(Project* project, ProjectWindow* projectWindow, ApplyPaletteDialog* applyDialog)
    : project(project), projectWindow(projectWindow), applyDialog(applyDialog), usageCache(std::make_shared<UsageCache>()) {
}

editor::PaletteWindow::~PaletteWindow() {
    if (lospec && !lospec->done) lospec->client.cancel();
}

void editor::PaletteWindow::setOpen(bool open) {
    if (open && !windowOpen) focusRequested = true;
    windowOpen = open;
}

bool editor::PaletteWindow::isOpen() const {
    return windowOpen;
}

bool editor::PaletteWindow::isFocused() const {
    return windowOpen && focused;
}

bool editor::PaletteWindow::takesKeys() const {
    return isFocused() && !ImGui::GetIO().WantTextInput;
}

void editor::PaletteWindow::resetForProjectSwitch() {
    if (lospec && !lospec->done) lospec->client.cancel();
    lospec.reset();
    lospecOpen = false;
    lospecResult.reset();
    lospecError = {};
    textureImportOpen = false;
    textureImportTask = {};
    rampDialogOpen = false;
    file.clear();
    palette = pxc::Palette();
    loaded = false;
    loadError.clear();
    loadWarnings.clear();
    fileText.clear();
    notice.clear();
    tooManySource.clear();
    staged.reset();
    selection.clear();
    anchor = -1;
    selectedRamp = -1;
    picker = {};
    projectPalettes.clear();
    palettesListed = false;
    pinnedTexture.clear();
    usageTexture.clear();
    usageKey.clear();
    usageRunningKey.clear();
    usageShownKey.clear();
    usageTask = {};
    maskTask = {};
    maskRunning = -1;
    maskColorShown = -1;
    usage.reset();
    usageImage.retire(retired);
    usageMask.retire(retired);
    textureChoices.clear();
    {
        std::lock_guard<std::mutex> lock(usageCache->mutex);
        usageCache->reports.clear();
    }
    firstOpenDone = false;
}

fs::path editor::PaletteWindow::palettesFolder() const {
    return project ? project->getAssetsPath() / "Palettes" : fs::path();
}

void editor::PaletteWindow::listPalettes() {
    projectPalettes = project ? pixelart::listProjectPalettes(*project) : std::vector<pixelart::ProjectPalette>();
    palettesListed = true;
}

void editor::PaletteWindow::open(const fs::path& requested) {
    if (!project) return;
    fs::path wanted = requested;
    if (!wanted.empty() && wanted.is_relative()) wanted = project->getProjectPath() / wanted;
    if (wanted.empty()) wanted = file;
    // the first open: the Default Palette (R4b), else the project's first palette
    if (wanted.empty()) {
        const std::string defaultPath = pixelart::defaultPalettePath(*project);
        if (!defaultPath.empty()) wanted = project->getProjectPath() / FileUtils::pathFromUtf8(defaultPath);
    }
    if (wanted.empty()) {
        listPalettes();
        if (!projectPalettes.empty()) wanted = project->getProjectPath() / FileUtils::pathFromUtf8(projectPalettes.front().path);
    }
    wanted = wanted.lexically_normal();
    if (wanted != file) {
        file = wanted;
        loaded = false;
        fileText.clear();
        staged.reset();
        selection.clear();
        anchor = -1;
        selectedRamp = -1;
        loadWarnings.clear();
        notice.clear();
    }
    firstOpenDone = true;
    reload(true);
    setOpen(true);
    focusRequested = true;
}

// ---------------------------------------------------------------------------------- the file

void editor::PaletteWindow::setNotice(const std::string& text, bool error) {
    notice = text;
    noticeIsError = error;
}

void editor::PaletteWindow::reload(bool force) {
    const double now = ImGui::GetTime();
    if (!force && now - lastCheck < 0.5) return;
    lastCheck = now;
    if (file.empty()) {
        loaded = false;
        loadError.clear();
        return;
    }
    std::error_code ec;
    const fs::file_time_type time = fs::last_write_time(file, ec);
    if (ec) {
        loaded = false;
        fileText.clear();
        loadError = "There is no palette at " + FileUtils::pathToGenericUtf8(project ? project->normalizeToProjectRelative(file) : file) +
                    " (deleted or moved). Choose another one above.";
        return;
    }
    const uintmax_t size = fs::file_size(file, ec);
    if (!force && loaded && time == fileTime && size == fileSize) return;
    fileTime = time;
    fileSize = ec ? 0 : size;
    readFile();
}

void editor::PaletteWindow::readFile() {
    const std::string text = pwh::readText(file);
    if (loaded && text == fileText) return;
    fileText = text;
    pxc::Palette next;
    std::vector<pxc::Diagnostic> diagnostics;
    loadWarnings.clear();
    if (!pxc::PaletteCodec::read(text, next, diagnostics)) {
        loaded = false;
        loadError = "This palette cannot be read";
        for (const pxc::Diagnostic& diagnostic : diagnostics) {
            if (diagnostic.severity != pxc::Diagnostic::Severity::Error) continue;
            loadError += ": " + diagnostic.message + (diagnostic.field.empty() ? "" : " (" + diagnostic.field + ")") +
                         (diagnostic.fix.empty() ? "" : ". " + diagnostic.fix);
            break;
        }
        return;
    }
    for (const pxc::Diagnostic& diagnostic : diagnostics) {
        if (diagnostic.severity == pxc::Diagnostic::Severity::Warning && diagnostic.kind != "unknown_keys") loadWarnings.push_back(diagnostic.message);
    }
    palette = std::move(next);
    loaded = true;
    loadError.clear();
    clampSelection();
}

bool editor::PaletteWindow::editable() const {
    return project && loaded && !palette.readOnly;
}

std::string editor::PaletteWindow::stepName(const std::string& what) const {
    return what + " (" + FileUtils::pathToUtf8(file.filename()) + ")";
}

std::string editor::PaletteWindow::mergeKeyFor(const char* what, uint64_t serial) const {
    return "palette:" + FileUtils::pathToGenericUtf8(file) + ":" + what + "#" + std::to_string(serial);
}

const pxc::Palette& editor::PaletteWindow::base() const {
    return staged ? staged->next : palette;
}

void editor::PaletteWindow::stage(pxc::Palette next, const std::string& what, const std::string& mergeKey,
                                  std::optional<std::vector<int>> selectAfter) {
    // a second edit in one frame builds on the first (base()): they go out as one write
    if (!staged) staged = Staged();
    staged->next = std::move(next);
    if (staged->what.empty()) staged->what = what;
    if (staged->mergeKey.empty()) staged->mergeKey = mergeKey;
    if (selectAfter) {
        staged->setSelection = true;
        staged->selection = std::move(*selectAfter);
    }
}

bool editor::PaletteWindow::editOps(const nlohmann::json& ops, const std::string& what, const std::string& mergeKey,
                                    std::optional<std::vector<int>> selectAfter) {
    if (!editable()) {
        if (loaded && palette.readOnly) setNotice("This palette was written by a newer editor: it is read-only here.", true);
        return false;
    }
    pxc::Palette next = base();
    const pixelart::PaletteEditResult result = pixelart::applyPaletteOps(next, ops);
    if (!result.ok) {
        const pixelart::PixelProblem& problem = result.errors.front();
        setNotice(problem.message + (problem.fix.empty() ? "" : ": " + problem.fix), true);
        return false;
    }
    if (!result.warnings.empty()) setNotice(result.warnings.front(), false);
    else if (noticeIsError) notice.clear();
    stage(std::move(next), what, mergeKey, std::move(selectAfter));
    return true;
}

void editor::PaletteWindow::applyStaged() {
    if (!staged) return;
    Staged edit = std::move(*staged);
    staged.reset();
    if (writePalette(edit.next, edit.what, edit.mergeKey) && edit.setSelection) setSelection(std::move(edit.selection));
}

bool editor::PaletteWindow::writePalette(const pxc::Palette& next, const std::string& what, const std::string& mergeKey) {
    if (!editable()) return false;
    const std::string text = pxc::PaletteCodec::write(next);
    std::string error;
    if (!writeFiles(project, {{file, text}}, stepName(what), mergeKey, error)) {
        setNotice(error, true);
        Out::error("Palette: " + error);
        reload(true);
        return false;
    }
    palette = next;
    fileText = text;
    std::error_code ec;
    fileTime = fs::last_write_time(file, ec);
    fileSize = fs::file_size(file, ec);
    clampSelection();
    return true;
}

fs::path editor::PaletteWindow::createPaletteFile(const pxc::Palette& created, const fs::path& directory, const std::string& baseName,
                                                  bool openIt) {
    if (!project) return {};
    const std::string stem = pwh::fileNameOf(baseName, "New Palette");
    fs::path target = directory / FileUtils::pathFromUtf8(stem + EXTENSION);
    std::error_code ec;
    for (int n = 2; fs::exists(target, ec); n++) target = directory / FileUtils::pathFromUtf8(stem + " " + std::to_string(n) + EXTENSION);
    std::string error;
    if (!writeFiles(project, {{target, pxc::PaletteCodec::write(created)}}, "Create Palette " + FileUtils::pathToUtf8(target.filename()), "", error)) {
        setNotice(error, true);
        Out::error("Palette: " + error);
        return {};
    }
    palettesListed = false;
    if (openIt) open(target);
    return target;
}

fs::path editor::PaletteWindow::createNew(const fs::path& directory, bool openIt) {
    pxc::Palette created;
    created.name = "New Palette";
    created.source.kind = "user";
    created.colors = {pxc::Rgba8{0, 0, 0, 255}, pxc::Rgba8{255, 255, 255, 255}};
    return createPaletteFile(created, directory, created.name, openIt);
}

void editor::PaletteWindow::importFile(const fs::path& source) {
    if (!project || source.empty()) return;
    std::vector<uint8_t> bytes;
    if (!pwh::readBytes(source, bytes)) {
        setNotice("Cannot read " + FileUtils::pathToUtf8(source) + ".", true);
        setOpen(true);
        return;
    }
    // beside the source when it is in the project, else with the project's palettes
    const fs::path directory = project->isInsideAssetsPath(source) ? source.parent_path() : palettesFolder();
    const pixelart::ImportResult result = pixelart::importPalette(source, bytes, source == tooManySource);
    tooManySource.clear();
    if (!result.ok()) {
        if (result.error.kind == "too_many_colors" && Util::isImageFile(source.string())) {
            openTextureImport(source, true);
            setNotice(result.error.message + ": Reduce Colours makes a palette of fewer.", false);
            return;
        }
        if (result.error.kind == "too_many_colors") tooManySource = source;
        setNotice(result.error.message + (result.error.fix.empty() ? "" : ": " + result.error.fix), true);
        Out::warning("Import Palette: " + result.error.message);
        setOpen(true);
        return;
    }
    const fs::path created = createPaletteFile(*result.palette, directory, FileUtils::pathToUtf8(source.stem()));
    if (!created.empty() && !result.warnings.empty()) setNotice(result.warnings.front().message, false);
}

void editor::PaletteWindow::exportPalette(const fs::path& paletteFile, jasperengine::editor::pixelart::PaletteFormat format) {
    pxc::Palette exported;
    if (paletteFile == file && loaded) {
        exported = palette;
    } else {
        std::vector<pxc::Diagnostic> diagnostics;
        if (!pxc::PaletteCodec::read(pwh::readText(paletteFile), exported, diagnostics)) {
            Out::error("Export Palette: " + FileUtils::pathToUtf8(paletteFile.filename()) + " cannot be read.");
            return;
        }
    }
    std::vector<std::string> warnings;
    auto bytes = pixelart::exportPalette(exported, format, warnings, 1);
    if (const pixelart::PaletteError* error = std::get_if<pixelart::PaletteError>(&bytes)) {
        setNotice(error->message + (error->fix.empty() ? "" : ": " + error->fix), true);
        Out::error("Export Palette: " + error->message);
        return;
    }
    const std::string extension = pixelart::paletteFormatExtension(format);
    const std::string chosen = FileDialogs::saveFileDialog(FileUtils::pathToUtf8(paletteFile.parent_path()),
                                                           FileUtils::pathToUtf8(paletteFile.stem()) + extension, exportLabel(format),
                                                           extension.c_str() + 1);
    if (chosen.empty()) return;
    fs::path target = FileUtils::pathFromUtf8(chosen);
    if (pwh::lowerExtension(target) != extension) target += extension;
    const std::vector<uint8_t>& data = std::get<std::vector<uint8_t>>(bytes);
    std::string error;
    if (!writeFiles(project, {{target, std::string(data.begin(), data.end())}}, "Export Palette " + FileUtils::pathToUtf8(target.filename()), "",
                    error)) {
        setNotice(error, true);
        Out::error("Export Palette: " + error);
        return;
    }
    if (!warnings.empty()) setNotice(warnings.front(), false);
    Out::info("Exported %s as %s.", FileUtils::pathToUtf8(paletteFile.filename()).c_str(), FileUtils::pathToUtf8(target).c_str());
}

void editor::PaletteWindow::showUsage(const fs::path& paletteFile, const fs::path& texture) {
    open(paletteFile);
    if (!texture.empty()) pinnedTexture = texture.is_relative() && project ? project->getProjectPath() / texture : texture;
    usageFocus = true;
}

void editor::PaletteWindow::useBuiltin(const std::string& id) {
    if (!project) return;
    const pixelart::ResolvedPalette resolved = pixelart::resolvePaletteName(*project, std::string(pixelart::kBuiltinPrefix) + id);
    if (!resolved.ok) {
        setNotice(resolved.error.message + ": " + resolved.error.fix, true);
        return;
    }
    const fs::path target = project->getProjectPath() / FileUtils::pathFromUtf8(resolved.path);
    // R7: a built-in is copied once into the project; the copy is what everything names
    if (resolved.needsCopy) {
        std::string error;
        if (!writeFiles(project, {{target, resolved.copyText}}, "Copy Built-in Palette " + resolved.palette.name, "", error)) {
            setNotice(error, true);
            return;
        }
        palettesListed = false;
    }
    open(target);
}

// ---------------------------------------------------------------------------------- selection

bool editor::PaletteWindow::isSelected(int index) const {
    return std::binary_search(selection.begin(), selection.end(), index);
}

void editor::PaletteWindow::setSelection(std::vector<int> indices) {
    std::sort(indices.begin(), indices.end());
    indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
    selection = std::move(indices);
    if (!selection.empty() && !isSelected(anchor)) anchor = selection.front();
    clampSelection();
}

void editor::PaletteWindow::clampSelection() {
    const int count = palette.count();
    selection.erase(std::remove_if(selection.begin(), selection.end(), [count](int i) { return i < 0 || i >= count; }), selection.end());
    if (anchor >= count) anchor = -1;
    if (selectedRamp >= (int)palette.ramps.size()) selectedRamp = -1;
}

void editor::PaletteWindow::select(int index, bool shift, bool ctrl) {
    if (shift && anchor >= 0) {
        std::vector<int> range;
        for (int i = std::min(anchor, index); i <= std::max(anchor, index); i++) range.push_back(i);
        if (ctrl) range.insert(range.end(), selection.begin(), selection.end());
        const int keep = anchor;
        setSelection(std::move(range));
        anchor = keep;
        return;
    }
    if (ctrl) {
        std::vector<int> next = selection;
        auto it = std::find(next.begin(), next.end(), index);
        if (it != next.end()) next.erase(it);
        else next.push_back(index);
        setSelection(std::move(next));
        anchor = index;
        return;
    }
    setSelection({index});
    anchor = index;
}

std::vector<int> editor::PaletteWindow::targetOf(int index) const {
    return isSelected(index) ? selection : std::vector<int>{index};
}

// ---------------------------------------------------------------------------------- edits

void editor::PaletteWindow::deleteSelection() {
    if (selection.empty()) return;
    editOps(nlohmann::json::array({{{"op", "remove"}, {"indices", selection}}}), selection.size() == 1 ? "Delete Colour" : "Delete Colours", {},
            std::vector<int>());
}

void editor::PaletteWindow::duplicateSelection() {
    if (selection.empty()) return;
    const pxc::Palette& p = base();
    nlohmann::json colors = nlohmann::json::array();
    for (int index : selection) {
        if (p.validIndex(index)) colors.push_back(pwh::hexOf(p.colors[index]));
    }
    const int at = selection.back() + 1;
    std::vector<int> created;
    for (size_t i = 0; i < colors.size(); i++) created.push_back(at + (int)i);
    editOps(nlohmann::json::array({{{"op", "insert"}, {"index", at}, {"colors", colors}}}),
            selection.size() == 1 ? "Duplicate Colour" : "Duplicate Colours", {}, created);
}

void editor::PaletteWindow::moveSelection(int direction) {
    if (selection.empty() || !editable()) return;
    const pxc::Palette& p = base();
    const int count = p.count();
    std::vector<int> order(count);
    std::iota(order.begin(), order.end(), 0);
    std::vector<bool> chosen(count, false);
    for (int index : selection) {
        if (index >= 0 && index < count) chosen[index] = true;
    }
    if (direction < 0) {
        if (chosen[0]) return;
        for (int i = 1; i < count; i++) {
            if (chosen[order[i]] && !chosen[order[i - 1]]) std::swap(order[i], order[i - 1]);
        }
    } else {
        if (chosen[count - 1]) return;
        for (int i = count - 2; i >= 0; i--) {
            if (chosen[order[i]] && !chosen[order[i + 1]]) std::swap(order[i], order[i + 1]);
        }
    }
    pxc::Palette next = p;
    next.remap(order);
    std::vector<int> moved;
    for (int i = 0; i < count; i++) {
        if (chosen[order[i]]) moved.push_back(i);
    }
    stage(std::move(next), selection.size() == 1 ? "Move Colour" : "Move Colours", {}, moved);
}

void editor::PaletteWindow::reorder(const std::vector<int>& moved, int insertAt) {
    if (moved.empty() || !editable()) return;
    const pxc::Palette& p = base();
    const int count = p.count();
    std::vector<bool> chosen(count, false);
    for (int index : moved) {
        if (index >= 0 && index < count) chosen[index] = true;
    }
    std::vector<int> order;
    for (int i = 0; i < insertAt && i < count; i++) {
        if (!chosen[i]) order.push_back(i);
    }
    const int first = (int)order.size();
    for (int i = 0; i < count; i++) {
        if (chosen[i]) order.push_back(i);
    }
    const int last = (int)order.size();
    for (int i = std::max(0, insertAt); i < count; i++) {
        if (!chosen[i]) order.push_back(i);
    }
    bool same = true;
    for (int i = 0; i < count && same; i++) same = order[i] == i;
    if (same) return;
    pxc::Palette next = p;
    next.remap(order);
    std::vector<int> placed;
    for (int i = first; i < last; i++) placed.push_back(i);
    stage(std::move(next), placed.size() == 1 ? "Move Colour" : "Move Colours", {}, placed);
}

void editor::PaletteWindow::sortBy(int sortValue) {
    if (!loaded) return;
    const auto sort = static_cast<pixelart::PaletteSort>(sortValue);
    const std::string what = std::string("Sort by ") + pixelart::paletteSortName(sort);
    if (sortIntoNewPalette) {
        // Pixelorama's default: the original keeps its numbers
        pxc::Palette sorted = base();
        sorted.remap(pixelart::paletteSortOrder(sorted, sort));
        sorted.readOnly = false;
        const std::string name = (sorted.name.empty() ? FileUtils::pathToUtf8(file.stem()) : sorted.name) + " (Sorted)";
        sorted.name = name;
        createPaletteFile(sorted, file.parent_path(), FileUtils::pathToUtf8(file.stem()) + " Sorted");
        return;
    }
    editOps(nlohmann::json::array({{{"op", "sort"}, {"by", pixelart::paletteSortName(sort)}}}), what, {}, std::vector<int>());
}

// ---------------------------------------------------------------------------------- the Color window

void editor::PaletteWindow::openPicker(int index, int variant) {
    const pxc::Palette& p = base();
    if (!p.validIndex(index) || (variant >= 0 && variant >= (int)p.variants.size())) return;
    const pxc::Rgba8 color = variant < 0 ? p.colors[index] : p.variantColor(variant, index);
    picker.id = ImGui::GetID(("##palette_picker_" + std::to_string(variant) + "_" + std::to_string(index)).c_str());
    picker.index = index;
    picker.variant = variant;
    picker.serial = ++pickerSerial;
    picker.clearOnCancel = false;
    ui::ColorFieldOptions options;
    options.alpha = true;
    options.hdr = false;
    // the palette's bytes as the window shows them (its hex is the palette's hex)
    options.linearStorage = false;
    options.label = variant < 0 ? "Palette Colour" : "Variant Colour";
    ui::ColorPickerWindow::get().open(picker.id, Vector4(colormath::fromByte(color.r), colormath::fromByte(color.g), colormath::fromByte(color.b),
                                                         colormath::fromByte(color.a)), options);
}

void editor::PaletteWindow::pollPicker() {
    if (picker.id == 0) return;
    Vector4 value;
    const ui::EditPhase phase = ui::ColorPickerWindow::get().consume(picker.id, value);
    if (phase == ui::EditPhase::Changing || phase == ui::EditPhase::Cancelled) {
        const pxc::Rgba8 color{colormath::toByte(value.x), colormath::toByte(value.y), colormath::toByte(value.z), colormath::toByte(value.w)};
        nlohmann::json op;
        if (picker.variant < 0) {
            op = {{"op", "set"}, {"index", picker.index}, {"color", pwh::hexOf(color)}};
        } else if (picker.variant < (int)base().variants.size() && phase == ui::EditPhase::Cancelled && picker.clearOnCancel) {
            op = {{"op", "set_variant"}, {"name", base().variants[picker.variant].name}, {"index", picker.index}};
        } else if (picker.variant < (int)base().variants.size()) {
            op = {{"op", "set_variant"}, {"name", base().variants[picker.variant].name}, {"index", picker.index}, {"color", pwh::hexOf(color)}};
        }
        // Esc writes the original back under the same key: the merged step is no change at all
        if (!op.is_null()) editOps(nlohmann::json::array({op}), picker.variant < 0 ? "Change Colour" : "Change Variant Colour",
                                   mergeKeyFor("picker", picker.serial));
    }
    if (phase == ui::EditPhase::Committed || phase == ui::EditPhase::Cancelled) picker = {};
}

// ---------------------------------------------------------------------------------- keys

void editor::PaletteWindow::bindCommands(CommandCatalog& catalog) {
    auto bind = [&catalog, this](const char* id, std::function<void()> run) {
        const CommandDef* existing = catalog.find(id);
        if (!existing) return;
        CommandDef def = *existing;
        def.run = [run](const CommandArgs&) { run(); };
        def.enabled = [this]() { return windowOpen && editable() && !selection.empty(); };
        // not while a swatch is dragged or a field is edited
        def.keyGate = [this]() { return takesKeys() && !ImGui::IsAnyItemActive(); };
        catalog.add(std::move(def));
    };
    bind("palette.delete", [this]() { deleteSelection(); });
    bind("palette.duplicate", [this]() { duplicateSelection(); });
    bind("palette.moveLeft", [this]() { moveSelection(-1); });
    bind("palette.moveRight", [this]() { moveSelection(1); });
}

// ---------------------------------------------------------------------------------- show

void editor::PaletteWindow::show() {
    for (Texture& texture : retired) texture.destroy();
    retired.clear();
    // every frame, drawn or not: the Color window ends a session whose field is not polled
    pollPicker();
    // a Color window session may run with this window closed: its edits are written all the same
    applyStaged();
    if (windowOpen || picker.id != 0) reload(false);
    pollLospec();
    if (textureImportTask.isValid() && textureImportTask.isReady()) {
        std::shared_ptr<TextureImport> result;
        try {
            result = textureImportTask.get();
        } catch (const std::exception& e) {
            textureImportError = e.what();
        }
        textureImportTask = {};
        if (result && result->palette) {
            const fs::path directory = project && project->isInsideAssetsPath(textureImportSource) ? textureImportSource.parent_path() : palettesFolder();
            if (!createPaletteFile(*result->palette, directory, FileUtils::pathToUtf8(textureImportSource.stem())).empty()) textureImportOpen = false;
        } else if (result) {
            textureImportError = result->error;
        }
    }

    if (windowOpen) {
        if (!firstOpenDone) open();
        ImGui::SetNextWindowSize(ImVec2(Theme::dpi(460.0f), Theme::dpi(620.0f)), ImGuiCond_FirstUseEver);
        if (focusRequested) {
            ImGui::SetNextWindowFocus();
            focusRequested = false;
        }
        bool keepOpen = true;
        if (ImGui::Begin(WINDOW_NAME, &keepOpen, ImGuiWindowFlags_HorizontalScrollbar)) {
            focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
            drawToolbar();
            if (!notice.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, noticeIsError ? Theme::Colors::ErrorText : Theme::Colors::WarningText);
                ImGui::TextWrapped("%s", notice.c_str());
                ImGui::PopStyleColor();
                if (!tooManySource.empty()) {
                    if (ImGui::Button("Import the First 256")) importFile(tooManySource);
                    ImGui::SameLine();
                }
                if (ImGui::SmallButton("Dismiss##palette_notice")) {
                    notice.clear();
                    tooManySource.clear();
                }
            }
            if (!loaded) {
                if (!loadError.empty()) {
                    ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::ErrorText);
                    ImGui::TextWrapped("%s", loadError.c_str());
                    ImGui::PopStyleColor();
                } else {
                    ImGui::TextWrapped("No palette. Choose one above, make a New one, or Import one (a file, Lospec, a texture).");
                }
            } else {
                if (palette.readOnly) {
                    ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::WarningText);
                    ImGui::TextWrapped("This palette was written by a newer version of the editor: it is read-only here.");
                    ImGui::PopStyleColor();
                }
                for (const std::string& warning : loadWarnings) ImGui::TextDisabled("%s", warning.c_str());
                updateUsage();
                drawGrid();
                drawRamps();
                drawVariants();
                drawUsage();
                drawInfo();
            }
            drawRenamePopup();
            handleWindowDrops();
        } else {
            focused = false;
        }
        ImGui::End();
        if (!keepOpen) setOpen(false);
    } else {
        focused = false;
    }

    drawRampDialog();
    drawLospecDialog();
    drawTextureImportDialog();
}

// ---------------------------------------------------------------------------------- toolbar

void editor::PaletteWindow::drawSelector() {
    std::string preview = "(no palette)";
    if (loaded) preview = palette.name.empty() ? FileUtils::pathToUtf8(file.stem()) : palette.name;
    else if (!file.empty()) preview = FileUtils::pathToUtf8(file.stem());
    ImGui::SetNextItemWidth(Theme::dpi(170.0f));
    if (ImGui::BeginCombo("##palette_select", preview.c_str(), ImGuiComboFlags_HeightLarge)) {
        if (ImGui::IsWindowAppearing() || !palettesListed) listPalettes();
        ImGui::TextDisabled("Project");
        if (projectPalettes.empty()) ImGui::TextDisabled("  (none yet)");
        for (size_t i = 0; i < projectPalettes.size(); i++) {
            const pixelart::ProjectPalette& entry = projectPalettes[i];
            const fs::path path = project->getProjectPath() / FileUtils::pathFromUtf8(entry.path);
            const std::string stem = FileUtils::pathToUtf8(path.stem());
            const std::string name = entry.palette.name.empty() || entry.palette.name == stem ? stem : entry.palette.name + " (" + stem + ")";
            if (ImGui::Selectable((name + "##project_palette_" + std::to_string(i)).c_str(), path.lexically_normal() == file)) open(path);
            ImGui::SetItemTooltip("%s, %d colours", entry.path.c_str(), entry.palette.count());
        }
        ImGui::Separator();
        ImGui::TextDisabled("Built-in");
        for (const pixelart::BuiltinPalette& builtin : pixelart::builtinPalettes()) {
            if (ImGui::Selectable((builtin.palette.name + "##builtin_palette_" + builtin.id).c_str(), false)) useBuiltin(builtin.id);
            ImGui::SetItemTooltip("builtin:%s, %d colours. Using it copies it into the project (Assets/Palettes) once.", builtin.id.c_str(),
                                  builtin.palette.count());
        }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("The palette shown: the project's, or a built-in one (copied into the project when used)");
}

void editor::PaletteWindow::drawImportMenu() {
    if (ImGui::MenuItem("File...")) {
        const std::string chosen = FileDialogs::openFileDialog(project ? FileUtils::pathToUtf8(project->getAssetsPath()) : std::string());
        if (!chosen.empty()) importFile(FileUtils::pathFromUtf8(chosen));
    }
    ImGui::SetItemTooltip("GIMP .gpl, Lospec .hex, JASC and RIFF .pal, Paint.NET .txt, Adobe .ase, an image, or a .colors swatch library");
    if (ImGui::MenuItem("From Lospec...")) openLospec();
    if (ImGui::MenuItem("From Texture...")) {
        fs::path texture;
        if (projectWindow) {
            const std::vector<fs::path> chosen = projectWindow->getSelectedFiles();
            if (chosen.size() == 1 && pwh::isApplicableTexture(chosen[0])) texture = chosen[0];
        }
        openTextureImport(texture, false);
    }
    if (ImGui::BeginMenu("From Swatch Library")) {
        const std::vector<SwatchLibrary>& libraries = ui::ColorPickerWindow::get().libraries();
        if (libraries.empty()) ImGui::TextDisabled("No swatch libraries");
        for (size_t i = 0; i < libraries.size(); i++) {
            const std::string label = libraries[i].name + (libraries[i].scope == LibraryScope::Project ? " (Project)" : "") + "##library_" +
                                      std::to_string(i);
            if (ImGui::MenuItem(label.c_str())) importFile(libraries[i].path);
        }
        ImGui::EndMenu();
    }
}

void editor::PaletteWindow::drawExportMenu() {
    for (pixelart::PaletteFormat format : exportFormats()) {
        if (ImGui::MenuItem(exportLabel(format), nullptr, false, loaded)) exportPalette(file, format);
    }
}

void editor::PaletteWindow::drawSortMenu() {
    ImGui::TextDisabled("Sorting changes colour numbers; indexed Aseprite files keep their old numbers");
    ImGui::Checkbox("Sort into a New Palette", &sortIntoNewPalette);
    ImGui::Separator();
    static const pixelart::PaletteSort sorts[] = {pixelart::PaletteSort::Hue, pixelart::PaletteSort::Saturation, pixelart::PaletteSort::Luma,
                                                  pixelart::PaletteSort::Value, pixelart::PaletteSort::Reverse};
    static const char* labels[] = {"Hue", "Saturation", "Luma", "Value", "Reverse"};
    for (int i = 0; i < 5; i++) {
        if (ImGui::MenuItem(labels[i], nullptr, false, loaded && (sortIntoNewPalette || editable()))) sortBy((int)sorts[i]);
    }
}

void editor::PaletteWindow::drawToolbar() {
    drawSelector();
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_PLUS " New")) createNew(file.empty() ? palettesFolder() : file.parent_path());
    ImGui::SetItemTooltip("A new palette (black and white) beside this one");
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_FILE_IMPORT " Import")) ImGui::OpenPopup("##palette_import_menu");
    if (ImGui::BeginPopup("##palette_import_menu")) {
        drawImportMenu();
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!loaded);
    if (ImGui::Button(ICON_FA_FILE_EXPORT " Export")) ImGui::OpenPopup("##palette_export_menu");
    ImGui::EndDisabled();
    if (ImGui::BeginPopup("##palette_export_menu")) {
        drawExportMenu();
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!loaded);
    if (ImGui::Button(ICON_FA_ARROW_DOWN_WIDE_SHORT " Sort")) ImGui::OpenPopup("##palette_sort_menu");
    ImGui::EndDisabled();
    if (ImGui::BeginPopup("##palette_sort_menu")) {
        drawSortMenu();
        ImGui::EndPopup();
    }

    ImGui::BeginDisabled(!editable());
    if (ImGui::Button(ICON_FA_WAND_MAGIC_SPARKLES " Generate Ramp...")) openRampDialog();
    ImGui::SetItemTooltip("Grow a shading ramp from a base colour (Hue Shift or OKLCh)");
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_CLONE " Create Variant")) {
        const pxc::Palette& p = base();
        const std::string name = pwh::uniqueName("Variant", [&p](const std::string& n) { return p.findVariant(n) >= 0; });
        editOps(nlohmann::json::array({{{"op", "add_variant"}, {"name", name}}}), "Create Variant");
    }
    ImGui::SetItemTooltip("A recolouring of this palette (team colours, a hurt flash): each index its own colour, or the base one");
    ImGui::EndDisabled();

    ImGui::SetNextItemWidth(Theme::dpi(110.0f));
    ImGui::SliderFloat("Size##swatch_size", &swatchSize, 12.0f, 48.0f, "%.0f px", ImGuiSliderFlags_AlwaysClamp);
    ImGui::SameLine();
    ImGui::Checkbox("Always Show Indices", &alwaysShowIndices);
    if (loaded) {
        ImGui::SameLine();
        int columns = std::clamp(base().columns, 1, pxc::Palette::kMaxColumns);
        ImGui::SetNextItemWidth(Theme::dpi(70.0f));
        ImGui::BeginDisabled(!editable());
        if (ImGui::DragInt("Columns##palette_columns", &columns, 0.1f, 1, pxc::Palette::kMaxColumns, "%d", ImGuiSliderFlags_AlwaysClamp) &&
            columns != base().columns) {
            editOps(nlohmann::json::array({{{"op", "set_info"}, {"columns", columns}}}), "Change Columns", mergeKeyFor("columns", dragSerial));
        }
        if (ImGui::IsItemDeactivated()) dragSerial++;
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("How many swatches a row shows (stored with the palette)");
    }
    ImGui::Separator();
}

// ---------------------------------------------------------------------------------- the grid

void editor::PaletteWindow::drawGrid() {
    const pxc::Palette& p = palette;
    const int count = p.count();
    const int columns = std::clamp(p.columns, 1, pxc::Palette::kMaxColumns);
    const float size = Theme::dpi(swatchSize);
    const float spacing = Theme::dpi(2.0f);
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImGuiIO& io = ImGui::GetIO();
    const bool counted = usage && usageShownKey == usageKey && usage->error.empty() && (int)usage->report.counts.size() == count;
    std::vector<bool> inRamp(count, false);
    if (selectedRamp >= 0 && selectedRamp < (int)p.ramps.size()) {
        for (uint16_t index : p.ramps[selectedRamp].colors) {
            if (index < count) inRamp[index] = true;
        }
    }
    const bool canEdit = editable();
    const ImU32 accent = ImGui::GetColorU32(ImGuiCol_NavCursor);

    for (int i = 0; i < count; i++) {
        if (i % columns != 0) ImGui::SameLine(0.0f, spacing);
        ImGui::PushID(i);
        const pxc::Rgba8 color = p.colors[i];
        const ImVec2 min = ImGui::GetCursorScreenPos();
        const ImVec2 max(min.x + size, min.y + size);
        // the label carries what a test reads (its colour, its usage); the id after ### stays put
        std::string hex = pwh::hexOf(color).substr(1);
        std::string label = "color " + hex + (counted ? " count " + std::to_string(usage->report.counts[i]) : std::string()) +
                            "###palette_swatch_" + std::to_string(i);
        ImGui::InvisibleButton(label.c_str(), ImVec2(size, size));
        const bool hovered = ImGui::IsItemHovered();

        pwh::drawColor(drawList, min, max, color);
        if (p.transparent == i) {
            pwh::drawChecker(drawList, min, ImVec2(min.x + size * 0.5f, max.y));
            pwh::smallText(drawList, ImVec2(max.x - pwh::smallTextSize(ICON_FA_EYE_SLASH).x - 1.0f, min.y + 1.0f), pwh::contrastOn(color),
                           ICON_FA_EYE_SLASH);
        }
        if (p.isLocked(i)) pwh::smallText(drawList, ImVec2(min.x + 1.0f, min.y + 1.0f), pwh::contrastOn(color), ICON_FA_LOCK);
        if (inRamp[i]) drawList->AddRect(ImVec2(min.x + 1.0f, min.y + 1.0f), ImVec2(max.x - 1.0f, max.y - 1.0f), IM_COL32(255, 200, 40, 255), 0.0f, 0,
                                         Theme::dpi(2.0f));
        if (isSelected(i)) {
            drawList->AddRect(ImVec2(min.x - 1.0f, min.y - 1.0f), ImVec2(max.x + 1.0f, max.y + 1.0f), IM_COL32(0, 0, 0, 255), 0.0f, 0, 1.0f);
            drawList->AddRect(min, max, IM_COL32(255, 255, 255, 255), 0.0f, 0, Theme::dpi(2.0f));
        } else {
            drawList->AddRect(min, max, IM_COL32(0, 0, 0, 120));
        }
        if (alwaysShowIndices || hovered) {
            const std::string number = std::to_string(i);
            const ImVec2 textSize = pwh::smallTextSize(number.c_str());
            pwh::smallText(drawList, ImVec2(min.x + (size - textSize.x) * 0.5f, min.y + (size - textSize.y) * 0.5f), pwh::contrastOn(color),
                           number.c_str());
        }
        if (counted) {
            const std::string badge = pwh::compactCount(usage->report.counts[i]);
            const ImVec2 textSize = pwh::smallTextSize(badge.c_str());
            const ImVec2 at(max.x - textSize.x - 2.0f, max.y - textSize.y - 1.0f);
            drawList->AddRectFilled(ImVec2(at.x - 1.0f, at.y), ImVec2(max.x - 1.0f, max.y - 1.0f),
                                    usage->report.counts[i] > 0 ? IM_COL32(0, 0, 0, 170) : IM_COL32(120, 20, 20, 190), Theme::dpi(2.0f));
            pwh::smallText(drawList, at, IM_COL32(255, 255, 255, 255), badge.c_str());
        }

        // a press on a selected swatch keeps the selection for a drag; its release alone selects it
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
            swatchDragged = false;
            if (!io.KeyCtrl && !io.KeyShift && isSelected(i)) pendingSingleSelect = i;
            else select(i, io.KeyShift, io.KeyCtrl);
        }
        if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && canEdit) openPicker(i, -1);
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
            if (!isSelected(i)) {
                setSelection({i});
                anchor = i;
            }
            swatchDragged = true;
            ImGui::SetDragDropPayload(pwh::kSwatchPayload, selection.data(), selection.size() * sizeof(int));
            ImGui::Text("%d colour%s", (int)selection.size(), selection.size() == 1 ? "" : "s");
            ImGui::EndDragDropSource();
        }
        if (canEdit && ImGui::BeginDragDropTarget()) {
            const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(pwh::kSwatchPayload,
                                                                       ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect);
            if (payload) {
                // the insertion marker: before or after this swatch, by the pointer's half
                const bool after = io.MousePos.x > (min.x + max.x) * 0.5f;
                const float x = after ? max.x + spacing * 0.5f : min.x - spacing * 0.5f;
                drawList->AddLine(ImVec2(x, min.y - 2.0f), ImVec2(x, max.y + 2.0f), accent, Theme::dpi(3.0f));
                if (payload->IsDelivery()) reorder(pwh::payloadIndices(payload), after ? i + 1 : i);
            }
            ImGui::EndDragDropTarget();
        }
        if (ImGui::IsItemDeactivated() && pendingSingleSelect == i) {
            if (!swatchDragged) {
                setSelection({i});
                anchor = i;
            }
            pendingSingleSelect = -1;
        }
        if (hovered && !ImGui::GetDragDropPayload()) {
            std::string tip = "#" + std::to_string(i);
            const std::string name = p.colorName(i);
            if (!name.empty()) tip += "  " + name;
            tip += "\n" + pwh::hexOf(color);
            if (p.isLocked(i)) tip += "\nLocked";
            if (p.transparent == i) tip += "\nTransparent";
            if (counted) tip += "\n" + std::to_string(usage->report.counts[i]) + " pixels of the texture";
            ImGui::SetTooltip("%s", tip.c_str());
        }
        drawSwatchMenu(i);
        ImGui::PopID();
    }

    // "+": a copy of the last selected colour (else white), the Color window open on it, one step
    if (canEdit && count < pxc::Palette::kMaxColors) {
        if (count % columns != 0) ImGui::SameLine(0.0f, spacing);
        const ImVec2 min = ImGui::GetCursorScreenPos();
        const ImVec2 max(min.x + size, min.y + size);
        const bool add = ImGui::InvisibleButton("Add Colour###palette_add_color", ImVec2(size, size));
        drawList->AddRect(min, max, ImGui::GetColorU32(ImGuiCol_Border));
        const ImVec2 mid((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f);
        const ImU32 text = ImGui::GetColorU32(ImGuiCol_Text);
        drawList->AddLine(ImVec2(mid.x - size * 0.25f, mid.y), ImVec2(mid.x + size * 0.25f, mid.y), text);
        drawList->AddLine(ImVec2(mid.x, mid.y - size * 0.25f), ImVec2(mid.x, mid.y + size * 0.25f), text);
        ImGui::SetItemTooltip("Add a colour");
        if (add) {
            const pxc::Rgba8 color = !selection.empty() && p.validIndex(selection.back()) ? p.colors[selection.back()] : pxc::Rgba8{255, 255, 255, 255};
            const uint64_t serial = pickerSerial + 1;
            if (editOps(nlohmann::json::array({{{"op", "insert"}, {"color", pwh::hexOf(color)}}}), "Add Colour", mergeKeyFor("picker", serial),
                        std::vector<int>{count})) {
                openPicker(count, -1);
            }
        }
    }
}

void editor::PaletteWindow::drawSwatchMenu(int index) {
    if (!ImGui::BeginPopupContextItem("##swatch_menu")) return;
    if (ImGui::IsWindowAppearing() && !isSelected(index)) {
        setSelection({index});
        anchor = index;
    }
    const pxc::Palette& p = base();
    if (!p.validIndex(index)) {
        ImGui::EndPopup();
        return;
    }
    const std::vector<int> targets = targetOf(index);
    const bool canEdit = editable();
    if (ImGui::MenuItem("Edit Colour...", nullptr, false, canEdit)) openPicker(index, -1);
    if (ImGui::MenuItem("Rename...", nullptr, false, canEdit)) requestRename(RenameKind::Color, index, p.colorName(index));
    bool allLocked = true;
    for (int target : targets) allLocked = allLocked && p.isLocked(target);
    if (ImGui::MenuItem(allLocked ? "Unlock" : "Lock", nullptr, false, canEdit)) {
        editOps(nlohmann::json::array({{{"op", "lock"}, {"indices", targets}, {"locked", !allLocked}}}), allLocked ? "Unlock Colours" : "Lock Colours");
    }
    ImGui::SetItemTooltip("Locked colours are kept by Reduce Colours and never remapped by the cleanup tools");
    const bool transparent = p.transparent == index;
    if (ImGui::MenuItem(transparent ? "Clear Transparent" : "Set as Transparent", nullptr, false, canEdit)) {
        editOps(nlohmann::json::array({{{"op", "set_transparent"}, {"index", transparent ? -1 : index}}}),
                transparent ? "Clear Transparent Colour" : "Set Transparent Colour");
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Copy Hex")) {
        std::string text;
        for (int target : targets) text += (text.empty() ? "" : "\n") + pwh::hexOf(p.colors[target]);
        ImGui::SetClipboardText(text.c_str());
    }
    // one colour per target, in order; one colour pastes into every target
    std::vector<pxc::Rgba8> pasted;
    if (const char* clipboard = ImGui::GetClipboardText()) {
        std::string token;
        const std::string text = clipboard;
        for (size_t i = 0; i <= text.size() && pasted.size() < 256; i++) {
            const char c = i < text.size() ? text[i] : ' ';
            if (std::isspace((unsigned char)c) || c == ',' || c == ';') {
                pxc::Rgba8 color;
                if (!token.empty() && pxc::parseHex(token, color)) pasted.push_back(color);
                token.clear();
            } else {
                token += c;
            }
        }
    }
    if (ImGui::MenuItem("Paste Hex", nullptr, false, canEdit && !pasted.empty())) {
        nlohmann::json ops = nlohmann::json::array();
        for (size_t i = 0; i < targets.size(); i++) {
            const pxc::Rgba8 color = pasted.size() == 1 ? pasted[0] : pasted[std::min(i, pasted.size() - 1)];
            ops.push_back({{"op", "set"}, {"index", targets[i]}, {"color", pwh::hexOf(color)}});
        }
        editOps(ops, "Paste Colours");
    }
    ImGui::Separator();
    CommandCatalog& catalog = CommandCatalog::editor();
    if (ImGui::MenuItem("Duplicate", catalog.menuShortcutText("palette.duplicate").c_str(), false, canEdit)) duplicateSelection();
    if (ImGui::MenuItem("Delete", catalog.menuShortcutText("palette.delete").c_str(), false, canEdit)) deleteSelection();
    ImGui::EndPopup();
}

// ---------------------------------------------------------------------------------- rename

void editor::PaletteWindow::requestRename(RenameKind kind, int index, const std::string& current) {
    renameKind = kind;
    renameIndex = index;
    std::snprintf(renameBuffer, sizeof(renameBuffer), "%s", current.c_str());
    renameOpenRequested = true;
}

void editor::PaletteWindow::drawRenamePopup() {
    if (renameOpenRequested) {
        ImGui::OpenPopup("##palette_rename");
        renameOpenRequested = false;
    }
    if (!ImGui::BeginPopup("##palette_rename")) return;
    ImGui::TextUnformatted(renameKind == RenameKind::Color ? "Rename Colour" : renameKind == RenameKind::Ramp ? "Rename Ramp" : "Rename Variant");
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(Theme::dpi(220.0f));
    const bool entered = ImGui::InputText("##rename_text", renameBuffer, sizeof(renameBuffer), ImGuiInputTextFlags_EnterReturnsTrue);
    const bool ok = ImGui::Button("OK##rename_ok") || entered;
    ImGui::SameLine();
    const bool cancel = ImGui::Button("Cancel##rename_cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    if (ok) {
        const pxc::Palette& p = base();
        const std::string name = renameBuffer;
        if (renameKind == RenameKind::Color && p.validIndex(renameIndex)) {
            editOps(nlohmann::json::array({{{"op", "rename_color"}, {"index", renameIndex}, {"name", name}}}), "Rename Colour");
        } else if (renameKind == RenameKind::Ramp && renameIndex >= 0 && renameIndex < (int)p.ramps.size() && !name.empty()) {
            editOps(nlohmann::json::array({{{"op", "set_ramp"}, {"name", p.ramps[renameIndex].name}, {"rename", name}}}), "Rename Ramp");
        } else if (renameKind == RenameKind::Variant && renameIndex >= 0 && renameIndex < (int)p.variants.size() && !name.empty()) {
            editOps(nlohmann::json::array({{{"op", "set_variant"}, {"name", p.variants[renameIndex].name}, {"rename", name}}}), "Rename Variant");
        }
    }
    if (ok || cancel) {
        renameKind = RenameKind::None;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// ---------------------------------------------------------------------------------- ramps (R11)

void editor::PaletteWindow::drawRamps() {
    if (!ImGui::CollapsingHeader("Ramps", ImGuiTreeNodeFlags_DefaultOpen)) return;
    const pxc::Palette& p = palette;
    const bool canEdit = editable();
    const float size = Theme::dpi(std::max(14.0f, swatchSize * 0.75f));
    const float nameWidth = Theme::dpi(110.0f);
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImGuiIO& io = ImGui::GetIO();
    const ImU32 accent = ImGui::GetColorU32(ImGuiCol_NavCursor);

    auto appendTo = [this](int ramp, const std::vector<int>& indices) {
        const pxc::Palette& current = base();
        if (ramp < 0 || ramp >= (int)current.ramps.size() || indices.empty()) return;
        std::vector<int> colors(current.ramps[ramp].colors.begin(), current.ramps[ramp].colors.end());
        colors.insert(colors.end(), indices.begin(), indices.end());
        editOps(nlohmann::json::array({{{"op", "set_ramp"}, {"name", current.ramps[ramp].name}, {"colors", colors}}}), "Add to Ramp");
    };

    if (p.ramps.empty()) ImGui::TextDisabled("No ramps: drag swatches onto New Ramp, or use Generate Ramp...");
    for (int r = 0; r < (int)p.ramps.size(); r++) {
        const pxc::PaletteRamp& ramp = p.ramps[r];
        ImGui::PushID(r);
        std::string title = ramp.name;
        if (ramp.cycle != 0.0f) {
            char speed[32];
            std::snprintf(speed, sizeof(speed), "  %.3g/s", ramp.cycle);
            title += speed;
        }
        if (ImGui::Selectable((title + "###palette_ramp_" + std::to_string(r)).c_str(), selectedRamp == r, 0, ImVec2(nameWidth, size))) {
            selectedRamp = selectedRamp == r ? -1 : r;
        }
        ImGui::SetItemTooltip("%s: %d steps%s. Click to highlight its swatches; drop swatches here to append them.", ramp.name.c_str(),
                              (int)ramp.colors.size(), ramp.cycle != 0.0f ? ", colour cycling" : "");
        if (canEdit && ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(pwh::kSwatchPayload)) appendTo(r, pwh::payloadIndices(payload));
            ImGui::EndDragDropTarget();
        }
        if (ImGui::BeginPopupContextItem("##ramp_menu")) {
            if (ImGui::MenuItem("Rename...", nullptr, false, canEdit)) requestRename(RenameKind::Ramp, r, ramp.name);
            if (ImGui::MenuItem("Reverse", nullptr, false, canEdit)) {
                editOps(nlohmann::json::array({{{"op", "set_ramp"}, {"name", ramp.name}, {"reverse", true}}}), "Reverse Ramp");
            }
            if (ImGui::BeginMenu("Fit to N Steps", canEdit)) {
                ImGui::SetNextItemWidth(Theme::dpi(90.0f));
                ImGui::InputInt("Steps##fit_steps", &fitSteps);
                fitSteps = std::clamp(fitSteps, 1, pxc::Palette::kMaxColors);
                if (ImGui::MenuItem("Fit")) {
                    editOps(nlohmann::json::array({{{"op", "set_ramp"}, {"name", ramp.name}, {"fit", fitSteps}}}), "Fit Ramp");
                }
                ImGui::SetItemTooltip("Resample to this many steps, keeping both ends (never blended)");
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Cycle Speed", canEdit)) {
                float cycle = r < (int)base().ramps.size() ? base().ramps[r].cycle : ramp.cycle;
                ImGui::SetNextItemWidth(Theme::dpi(120.0f));
                if (ImGui::DragFloat("Steps per Second##cycle_speed", &cycle, 0.05f, -60.0f, 60.0f, "%.2f")) {
                    editOps(nlohmann::json::array({{{"op", "set_ramp"}, {"name", ramp.name}, {"cycle", cycle}}}), "Change Cycle Speed",
                            mergeKeyFor("cycle", dragSerial));
                }
                if (ImGui::IsItemDeactivated()) dragSerial++;
                ImGui::SetItemTooltip("Palette Cycling: how many steps the ramp's colours turn each second (0: none)");
                ImGui::EndMenu();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Delete", nullptr, false, canEdit)) {
                editOps(nlohmann::json::array({{{"op", "remove_ramp"}, {"name", ramp.name}}}), "Delete Ramp");
                if (selectedRamp == r) selectedRamp = -1;
            }
            ImGui::EndPopup();
        }

        for (int k = 0; k < (int)ramp.colors.size(); k++) {
            ImGui::SameLine(0.0f, k == 0 ? Theme::dpi(6.0f) : 1.0f);
            ImGui::PushID(k);
            const int index = ramp.colors[k];
            const pxc::Rgba8 color = p.validIndex(index) ? p.colors[index] : pxc::Rgba8{0, 0, 0, 0};
            const ImVec2 min = ImGui::GetCursorScreenPos();
            const ImVec2 max(min.x + size, min.y + size);
            ImGui::InvisibleButton(("###palette_ramp_" + std::to_string(r) + "_step_" + std::to_string(k)).c_str(), ImVec2(size, size));
            pwh::drawColor(drawList, min, max, color);
            drawList->AddRect(min, max, IM_COL32(0, 0, 0, 120));
            if (ImGui::IsItemHovered() && !ImGui::GetDragDropPayload()) {
                ImGui::SetTooltip("Step %d: colour #%d %s", k, index, pwh::hexOf(color).c_str());
            }
            if (canEdit && ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
                const pwh::RampStepPayload step{r, k};
                ImGui::SetDragDropPayload(pwh::kRampStepPayload, &step, sizeof(step));
                ImGui::Text("Step %d", k);
                ImGui::EndDragDropSource();
            }
            if (canEdit && ImGui::BeginDragDropTarget()) {
                const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(pwh::kRampStepPayload,
                                                                           ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect);
                pwh::RampStepPayload dragged;
                if (payload && payload->DataSize == (int)sizeof(dragged)) std::memcpy(&dragged, payload->Data, sizeof(dragged));
                if (payload && dragged.ramp == r) {
                    const bool after = io.MousePos.x > (min.x + max.x) * 0.5f;
                    const float x = after ? max.x : min.x - 1.0f;
                    drawList->AddLine(ImVec2(x, min.y - 2.0f), ImVec2(x, max.y + 2.0f), accent, Theme::dpi(3.0f));
                    if (payload->IsDelivery()) {
                        const pxc::Palette& current = base();
                        if (r < (int)current.ramps.size() && dragged.step >= 0 && dragged.step < (int)current.ramps[r].colors.size()) {
                            std::vector<int> colors(current.ramps[r].colors.begin(), current.ramps[r].colors.end());
                            const int moving = colors[dragged.step];
                            int insertAt = after ? k + 1 : k;
                            colors.erase(colors.begin() + dragged.step);
                            if (dragged.step < insertAt) insertAt--;
                            colors.insert(colors.begin() + std::clamp(insertAt, 0, (int)colors.size()), moving);
                            editOps(nlohmann::json::array({{{"op", "set_ramp"}, {"name", current.ramps[r].name}, {"colors", colors}}}), "Reorder Ramp");
                        }
                    }
                }
                if (const ImGuiPayload* swatches = ImGui::AcceptDragDropPayload(pwh::kSwatchPayload)) appendTo(r, pwh::payloadIndices(swatches));
                ImGui::EndDragDropTarget();
            }
            if (ImGui::BeginPopupContextItem("##step_menu")) {
                if (ImGui::MenuItem("Select Colour")) {
                    setSelection({index});
                    anchor = index;
                }
                if (ImGui::MenuItem("Remove from Ramp", nullptr, false, canEdit && ramp.colors.size() > 1)) {
                    std::vector<int> colors(ramp.colors.begin(), ramp.colors.end());
                    colors.erase(colors.begin() + k);
                    editOps(nlohmann::json::array({{{"op", "set_ramp"}, {"name", ramp.name}, {"colors", colors}}}), "Remove from Ramp");
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }

        // the end of the strip: swatches dropped here are appended
        if (canEdit) {
            ImGui::SameLine(0.0f, Theme::dpi(4.0f));
            const ImVec2 min = ImGui::GetCursorScreenPos();
            const ImVec2 max(min.x + size, min.y + size);
            ImGui::InvisibleButton(("###palette_ramp_" + std::to_string(r) + "_drop").c_str(), ImVec2(size, size));
            drawList->AddRect(min, max, ImGui::GetColorU32(ImGuiCol_TextDisabled), 0.0f, 0, 1.0f);
            const ImVec2 mid((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f);
            drawList->AddLine(ImVec2(mid.x - size * 0.2f, mid.y), ImVec2(mid.x + size * 0.2f, mid.y), ImGui::GetColorU32(ImGuiCol_TextDisabled));
            drawList->AddLine(ImVec2(mid.x, mid.y - size * 0.2f), ImVec2(mid.x, mid.y + size * 0.2f), ImGui::GetColorU32(ImGuiCol_TextDisabled));
            ImGui::SetItemTooltip("Drop swatches here to append them to %s", ramp.name.c_str());
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(pwh::kSwatchPayload)) appendTo(r, pwh::payloadIndices(payload));
                ImGui::EndDragDropTarget();
            }
        }
        ImGui::PopID();
    }

    if (canEdit) {
        const bool fromSelection = ImGui::Button(ICON_FA_PLUS " New Ramp###palette_new_ramp");
        ImGui::SetItemTooltip("A ramp of the selected swatches (or drop swatches here)");
        std::vector<int> colors;
        if (fromSelection) colors = selection;
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(pwh::kSwatchPayload)) colors = pwh::payloadIndices(payload);
            ImGui::EndDragDropTarget();
        }
        if (!colors.empty()) {
            const pxc::Palette& current = base();
            const std::string name = pwh::uniqueName("Ramp", [&current](const std::string& n) { return current.findRamp(n) != nullptr; });
            const int created = (int)current.ramps.size();
            if (editOps(nlohmann::json::array({{{"op", "add_ramp"}, {"name", name}, {"colors", colors}}}), "New Ramp")) selectedRamp = created;
        } else if (fromSelection) {
            setNotice("Select the swatches of the ramp first (or drop them on New Ramp).", false);
        }
    }
}

// ---------------------------------------------------------------------------------- variants

void editor::PaletteWindow::drawVariants() {
    if (!ImGui::CollapsingHeader("Variants", ImGuiTreeNodeFlags_DefaultOpen)) return;
    const pxc::Palette& p = palette;
    if (p.variants.empty()) {
        ImGui::TextDisabled("No variants: Create Variant adds one (each index its own colour, or the base colour).");
        return;
    }
    const bool canEdit = editable();
    const int columns = 2 + (int)p.variants.size();
    const float cell = Theme::dpi(18.0f);
    const float rowHeight = cell + ImGui::GetStyle().CellPadding.y * 2.0f;
    const float height = std::min((float)p.count(), 12.0f) * rowHeight + ImGui::GetFrameHeightWithSpacing() + Theme::dpi(4.0f);
    const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY |
                                  ImGuiTableFlags_SizingFixedFit;
    if (!ImGui::BeginTable("##palette_variants", columns, flags, ImVec2(0.0f, height))) return;
    ImGui::TableSetupScrollFreeze(2, 1);
    ImGui::TableSetupColumn("Index");
    ImGui::TableSetupColumn("Base");
    for (int v = 0; v < (int)p.variants.size(); v++) {
        ImGui::TableSetupColumn((p.variants[v].name + "###variant_column_" + std::to_string(v)).c_str());
    }
    ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
    for (int c = 0; c < columns; c++) {
        ImGui::TableSetColumnIndex(c);
        ImGui::TableHeader(ImGui::TableGetColumnName(c));
        if (c < 2) continue;
        const int v = c - 2;
        ImGui::PushID(v);
        if (ImGui::BeginPopupContextItem("##variant_menu")) {
            if (ImGui::MenuItem("Rename...", nullptr, false, canEdit)) requestRename(RenameKind::Variant, v, p.variants[v].name);
            if (ImGui::MenuItem("Delete", nullptr, false, canEdit)) {
                editOps(nlohmann::json::array({{{"op", "remove_variant"}, {"name", p.variants[v].name}}}), "Delete Variant");
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImGuiListClipper clipper;
    clipper.Begin(p.count(), rowHeight);
    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++) {
            ImGui::TableNextRow(0, rowHeight);
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%d", i);
            ImGui::TableSetColumnIndex(1);
            {
                const ImVec2 min = ImGui::GetCursorScreenPos();
                ImGui::Dummy(ImVec2(cell * 2.0f, cell));
                pwh::drawColor(drawList, min, ImVec2(min.x + cell * 2.0f, min.y + cell), p.colors[i]);
            }
            for (int v = 0; v < (int)p.variants.size(); v++) {
                ImGui::TableSetColumnIndex(2 + v);
                ImGui::PushID(v * pxc::Palette::kMaxColors + i);
                const pxc::PaletteVariant& variant = p.variants[v];
                const std::optional<pxc::Rgba8> value = i < (int)variant.colors.size() ? variant.colors[i] : std::nullopt;
                const ImVec2 min = ImGui::GetCursorScreenPos();
                const ImVec2 max(min.x + cell * 2.0f, min.y + cell);
                const std::string label = (value ? "color " + pwh::hexOf(*value).substr(1) : std::string("same as base")) + "###palette_variant_" +
                                          std::to_string(v) + "_" + std::to_string(i);
                const bool clicked = ImGui::InvisibleButton(label.c_str(), ImVec2(cell * 2.0f, cell));
                if (value) pwh::drawColor(drawList, min, max, *value);
                else pwh::drawHatch(drawList, min, max);
                drawList->AddRect(min, max, IM_COL32(0, 0, 0, 120));
                ImGui::SetItemTooltip("%s, index %d: %s", variant.name.c_str(), i,
                                      value ? pwh::hexOf(*value).c_str() : "same as the base colour (click to give it its own)");
                if (clicked && canEdit) {
                    // a hatched cell takes the base colour first, in the same step as the Color window's edit
                    if (!value) {
                        editOps(nlohmann::json::array({{{"op", "set_variant"}, {"name", variant.name}, {"index", i}, {"color", pwh::hexOf(p.colors[i])}}}),
                                "Change Variant Colour", mergeKeyFor("picker", pickerSerial + 1));
                    }
                    openPicker(i, v);
                    picker.clearOnCancel = !value.has_value();
                }
                if (ImGui::BeginPopupContextItem("##variant_cell_menu")) {
                    if (ImGui::MenuItem("Edit Colour...", nullptr, false, canEdit)) openPicker(i, v);
                    if (ImGui::MenuItem("Same as Base", nullptr, false, canEdit && value.has_value())) {
                        editOps(nlohmann::json::array({{{"op", "set_variant"}, {"name", variant.name}, {"index", i}}}), "Clear Variant Colour");
                    }
                    ImGui::EndPopup();
                }
                ImGui::PopID();
            }
        }
    }
    ImGui::EndTable();
}

// ---------------------------------------------------------------------------------- info

void editor::PaletteWindow::drawInfo() {
    if (!ImGui::CollapsingHeader("Info")) return;
    // the fields follow the file while none of them is being typed in
    if (infoSyncedText != fileText && !infoActive) {
        std::snprintf(infoName, sizeof(infoName), "%s", palette.name.c_str());
        std::snprintf(infoAuthor, sizeof(infoAuthor), "%s", palette.author.c_str());
        std::snprintf(infoDescription, sizeof(infoDescription), "%s", palette.description.c_str());
        infoSyncedText = fileText;
    }
    const bool canEdit = editable();
    ImGui::BeginDisabled(!canEdit);
    bool active = false;
    auto field = [&](const char* label, const char* key, char* buffer, size_t size, bool multiline) {
        if (multiline) ImGui::InputTextMultiline(label, buffer, size, ImVec2(-1.0f, ImGui::GetTextLineHeight() * 3.5f));
        else ImGui::InputText(label, buffer, size);
        active = active || ImGui::IsItemActive();
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            editOps(nlohmann::json::array({{{"op", "set_info"}, {key, std::string(buffer)}}}), "Change Palette Info");
        }
    };
    field("Name##palette_name", "name", infoName, sizeof(infoName), false);
    field("Author##palette_author", "author", infoAuthor, sizeof(infoAuthor), false);
    ImGui::TextUnformatted("Description");
    field("##palette_description", "description", infoDescription, sizeof(infoDescription), true);
    ImGui::EndDisabled();
    infoActive = active;
    const pxc::PaletteSource& source = palette.source;
    if (!source.empty()) {
        std::string from = source.kind;
        if (!source.id.empty()) from += " " + source.id;
        if (!source.file.empty()) from += " (" + source.file + ")";
        if (!source.fetched.empty()) from += ", fetched " + source.fetched;
        ImGui::TextDisabled("Source: %s", from.c_str());
        if (!source.url.empty()) ImGui::TextDisabled("%s", source.url.c_str());
    }
    ImGui::TextDisabled("%d colours, %d ramps, %d variants", palette.count(), (int)palette.ramps.size(), (int)palette.variants.size());
}

// ---------------------------------------------------------------------------------- drops

void editor::PaletteWindow::handleWindowDrops() {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (!window || !ImGui::GetDragDropPayload()) return;
    // the window's own targets (swatches, ramps, the Usage texture) are smaller: they win
    if (!ImGui::BeginDragDropTargetCustom(window->InnerRect, window->ID)) return;
    std::vector<std::string> paths;
    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("external_files")) paths = Util::getStringsFromPayload(payload);
    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("resource_files")) paths = Util::getStringsFromPayload(payload);
    ImGui::EndDragDropTarget();
    for (const std::string& text : paths) {
        const fs::path path = FileUtils::pathFromUtf8(text);
        if (isPaletteFile(path)) {
            open(path);
        } else if (isForeignPaletteFile(path) || Util::isImageFile(text) || pwh::lowerExtension(path) == swatches::kExtension) {
            importFile(path);
        } else {
            setNotice(FileUtils::pathToUtf8(path.filename()) + " is not a palette: drop a .palette, .gpl, .hex, .pal, .txt, .ase, .colors or an image.",
                      true);
        }
    }
}

// ---------------------------------------------------------------------------------- Generate Ramp (R10)

void editor::PaletteWindow::openRampDialog() {
    if (!selection.empty() && base().validIndex(selection.front())) recipe.base = base().colors[selection.front()];
    const pxc::Palette& p = base();
    const std::string name = pwh::uniqueName("Ramp", [&p](const std::string& n) { return p.findRamp(n) != nullptr; });
    std::snprintf(rampName, sizeof(rampName), "%s", name.c_str());
    rampDialogOpen = true;
    rampDialogFocus = true;
}

void editor::PaletteWindow::drawRampDialog() {
    if (!rampDialogOpen) return;
    ImGui::SetNextWindowSize(ImVec2(Theme::dpi(380.0f), 0.0f), ImGuiCond_FirstUseEver);
    if (rampDialogFocus) {
        ImGui::SetNextWindowFocus();
        rampDialogFocus = false;
    }
    bool keepOpen = true;
    if (ImGui::Begin("Generate Ramp###PaletteGenerateRamp", &keepOpen, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Base Color");
        ImGui::SameLine();
        ui::ColorFieldOptions options;
        options.alpha = false;
        options.linearStorage = false;
        options.label = "Ramp Base Color";
        options.width = Theme::dpi(110.0f);
        Vector3 rgb(colormath::fromByte(recipe.base.r), colormath::fromByte(recipe.base.g), colormath::fromByte(recipe.base.b));
        const ui::EditPhase phase = ui::colorField("##ramp_base", rgb, options);
        if (phase == ui::EditPhase::Changing || phase == ui::EditPhase::Cancelled || phase == ui::EditPhase::Applied) {
            recipe.base = pxc::Rgba8{colormath::toByte(rgb.x), colormath::toByte(rgb.y), colormath::toByte(rgb.z), recipe.base.a};
        }
        ImGui::SameLine();
        const bool haveSwatch = loaded && !selection.empty() && palette.validIndex(selection.front());
        ImGui::BeginDisabled(!haveSwatch);
        if (ImGui::Button("Use Selected Swatch")) recipe.base = palette.colors[selection.front()];
        ImGui::EndDisabled();

        ImGui::SliderInt("Darker", &recipe.darker, 0, pixelart::RampRecipe::kMaxSteps, "%d", ImGuiSliderFlags_AlwaysClamp);
        ImGui::SliderInt("Lighter", &recipe.lighter, 0, pixelart::RampRecipe::kMaxSteps, "%d", ImGuiSliderFlags_AlwaysClamp);
        int mode = recipe.mode == pixelart::RampMode::HueShift ? 0 : 1;
        if (ImGui::Combo("Mode", &mode, "Hue Shift\0OKLCh\0")) recipe.mode = mode == 0 ? pixelart::RampMode::HueShift : pixelart::RampMode::Oklch;
        if (recipe.mode == pixelart::RampMode::HueShift) {
            ImGui::DragFloat("Hue (\xC2\xB0)", &recipe.hue, 0.5f, 0.0f, 180.0f, "%.1f", ImGuiSliderFlags_AlwaysClamp);
            ImGui::DragFloat("Saturation (%)", &recipe.saturation, 0.5f, 0.0f, 100.0f, "%.1f", ImGuiSliderFlags_AlwaysClamp);
            ImGui::DragFloat("Value (%)", &recipe.value, 0.5f, 0.0f, 100.0f, "%.1f", ImGuiSliderFlags_AlwaysClamp);
            if (ImGui::TreeNode("Advanced")) {
                ImGui::DragFloat("Lighten Hue Limit (\xC2\xB0)", &recipe.hueLightenLimit, 1.0f, 0.0f, 360.0f, "%.0f", ImGuiSliderFlags_AlwaysClamp);
                ImGui::DragFloat("Darken Hue Limit (\xC2\xB0)", &recipe.hueDarkenLimit, 1.0f, 0.0f, 360.0f, "%.0f", ImGuiSliderFlags_AlwaysClamp);
                ImGui::DragFloat("Saturation Floor (%)", &recipe.saturationFloor, 0.5f, 0.0f, 100.0f, "%.1f", ImGuiSliderFlags_AlwaysClamp);
                ImGui::DragFloat("Value Floor (%)", &recipe.valueFloor, 0.5f, 0.0f, 100.0f, "%.1f", ImGuiSliderFlags_AlwaysClamp);
                ImGui::TreePop();
            }
        } else {
            ImGui::SliderFloat("Darkest L", &recipe.darkestL, 0.0f, 1.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
            ImGui::SliderFloat("Lightest L", &recipe.lightestL, 0.0f, 1.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
            ImGui::SliderFloat("Chroma Peak", &recipe.chromaPeak, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
            ImGui::DragFloat("Hue (\xC2\xB0)", &recipe.hue, 0.5f, 0.0f, 180.0f, "%.1f", ImGuiSliderFlags_AlwaysClamp);
        }
        ImGui::InputText("Ramp Name", rampName, sizeof(rampName));

        // the preview strip, live
        const std::vector<pxc::Rgba8> colors = pixelart::generateRamp(recipe);
        const float cell = Theme::dpi(18.0f);
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        for (size_t i = 0; i < colors.size(); i++) {
            if (i > 0) ImGui::SameLine(0.0f, 1.0f);
            const ImVec2 min = ImGui::GetCursorScreenPos();
            ImGui::PushID((int)i);
            ImGui::InvisibleButton("##ramp_preview", ImVec2(cell, cell));
            ImGui::PopID();
            pwh::drawColor(drawList, min, ImVec2(min.x + cell, min.y + cell), colors[i]);
            if ((int)i == recipe.darker) drawList->AddRect(min, ImVec2(min.x + cell, min.y + cell), IM_COL32(255, 255, 255, 255), 0.0f, 0, 2.0f);
            ImGui::SetItemTooltip("%s%s", pwh::hexOf(colors[i]).c_str(), (int)i == recipe.darker ? " (base)" : "");
        }

        const bool canEdit = editable();
        auto generateOp = [&](const std::string& replace) {
            nlohmann::json op = {{"op", "generate_ramp"},
                                 {"base", pwh::hexOf(recipe.base)},
                                 {"darker", recipe.darker},
                                 {"lighter", recipe.lighter},
                                 {"mode", recipe.mode == pixelart::RampMode::HueShift ? "hue_shift" : "oklch"},
                                 {"hue", recipe.hue},
                                 {"saturation", recipe.saturation},
                                 {"value", recipe.value},
                                 {"hue_lighten_limit", recipe.hueLightenLimit},
                                 {"hue_darken_limit", recipe.hueDarkenLimit},
                                 {"saturation_floor", recipe.saturationFloor},
                                 {"value_floor", recipe.valueFloor},
                                 {"darkest_l", recipe.darkestL},
                                 {"lightest_l", recipe.lightestL},
                                 {"chroma_peak", recipe.chromaPeak}};
            if (replace.empty()) op["name"] = std::string(rampName);
            else op["replace"] = replace;
            return op;
        };
        ImGui::BeginDisabled(!canEdit || rampName[0] == 0);
        if (ImGui::Button("Add to Palette##generate_ramp_add")) {
            const pxc::Palette& p = base();
            const int ramps = (int)p.ramps.size();
            if (editOps(nlohmann::json::array({generateOp("")}), "Generate Ramp")) {
                selectedRamp = ramps;
                const pxc::Palette& next = base();
                const std::string name = pwh::uniqueName("Ramp", [&next](const std::string& n) { return next.findRamp(n) != nullptr; });
                std::snprintf(rampName, sizeof(rampName), "%s", name.c_str());
            }
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Append the colours (an exact duplicate reuses its index) and a ramp of them");
        ImGui::SameLine();
        const bool haveRamp = loaded && selectedRamp >= 0 && selectedRamp < (int)palette.ramps.size();
        ImGui::BeginDisabled(!canEdit || !haveRamp);
        if (ImGui::Button("Replace Ramp##generate_ramp_replace") && haveRamp) {
            editOps(nlohmann::json::array({generateOp(palette.ramps[selectedRamp].name)}), "Replace Ramp");
        }
        ImGui::EndDisabled();
        if (haveRamp) ImGui::SetItemTooltip("Rewrite the selected ramp (%s) with these colours", palette.ramps[selectedRamp].name.c_str());
        else ImGui::SetItemTooltip("Select a ramp under the grid first");
        if (!notice.empty() && noticeIsError) {
            ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::ErrorText);
            ImGui::TextWrapped("%s", notice.c_str());
            ImGui::PopStyleColor();
        }
    }
    ImGui::End();
    if (!keepOpen) rampDialogOpen = false;
}

// ---------------------------------------------------------------------------------- Lospec (R8)

void editor::PaletteWindow::openLospec() {
    lospecOpen = true;
    lospecFocus = true;
    lospecOffline = paletteNetworkOffline();
    if (!lospec) {
        lospecResult.reset();
        lospecError = {};
    }
}

void editor::PaletteWindow::startLospecFetch() {
    if (lospec) return;
    lospecResult.reset();
    lospecError = {};
    const std::optional<std::string> slug = pixelart::normaliseSlug(lospecInput);
    if (!slug) {
        lospecError = {"invalid_value", "slug", "That is not a Lospec palette name.",
                       "Type its slug (endesga-32), its page's address, or a lospec-palette:// link."};
        return;
    }
    lospecOffline = paletteNetworkOffline();
    if (lospecOffline) return;
    lospecSlug = *slug;
    auto fetch = std::make_shared<LospecFetch>();
    fetch->slug = *slug;
    lospec = fetch;
    // a worker of its own: the request blocks for up to 10 s
    std::thread([fetch]() {
        auto result = pixelart::fetchLospec(fetch->slug, fetch->client, pixelart::todayIsoDate());
        if (pxc::Palette* found = std::get_if<pxc::Palette>(&result)) fetch->palette = std::move(*found);
        else fetch->error = std::get<pixelart::PaletteError>(result);
        fetch->done = true;
    }).detach();
}

void editor::PaletteWindow::pollLospec() {
    if (!lospec || !lospec->done) return;
    if (!lospec->cancelled) {
        if (lospec->palette) lospecResult = std::move(lospec->palette);
        else lospecError = lospec->error;
    }
    lospec.reset();
}

void editor::PaletteWindow::drawLospecDialog() {
    if (!lospecOpen) return;
    ImGui::SetNextWindowSize(ImVec2(Theme::dpi(420.0f), 0.0f), ImGuiCond_FirstUseEver);
    if (lospecFocus) {
        ImGui::SetNextWindowFocus();
        lospecFocus = false;
    }
    bool keepOpen = true;
    if (ImGui::Begin("From Lospec###PaletteLospec", &keepOpen, ImGuiWindowFlags_NoCollapse)) {
        const bool running = lospec != nullptr;
        if (lospecOffline) {
            ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::WarningText);
            ImGui::TextWrapped("Offline: the editor has no network access here (no curl, or network access is off in Preferences).");
            ImGui::PopStyleColor();
            ImGui::TextWrapped("Download the palette's .hex from its Lospec page and import it, or drop it on the Palette window.");
            if (ImGui::Button("Import File...##lospec_import")) {
                const std::string chosen = FileDialogs::openFileDialog(project ? FileUtils::pathToUtf8(project->getAssetsPath()) : std::string());
                if (!chosen.empty()) importFile(FileUtils::pathFromUtf8(chosen));
            }
            ImGui::SameLine();
            if (ImGui::Button("Check Again##lospec_retry")) lospecOffline = paletteNetworkOffline();
        } else {
            ImGui::BeginDisabled(running);
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - Theme::dpi(80.0f));
            const bool entered = ImGui::InputTextWithHint("##lospec_slug", "endesga-32, its lospec.com address or a lospec-palette:// link",
                                                          lospecInput, sizeof(lospecInput), ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (running) {
                if (ImGui::Button("Cancel##lospec_cancel", ImVec2(-1.0f, 0.0f))) {
                    lospec->cancelled = true;
                    lospec->client.cancel();
                }
            } else if ((ImGui::Button("Fetch##lospec_fetch", ImVec2(-1.0f, 0.0f)) || entered) && lospecInput[0]) {
                startLospecFetch();
            }
            if (running) {
                const char* spinner = "|/-\\";
                ImGui::Text("%c Fetching %s from lospec.com...", spinner[(int)(ImGui::GetTime() * 8.0) % 4], lospecSlug.c_str());
            }
            if (!lospecError.kind.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::ErrorText);
                ImGui::TextWrapped("%s", lospecError.message.c_str());
                ImGui::PopStyleColor();
                if (!lospecError.fix.empty()) ImGui::TextWrapped("%s", lospecError.fix.c_str());
                if (lospecError.kind == "network_unavailable" && ImGui::Button("Import File...##lospec_error_import")) {
                    const std::string chosen = FileDialogs::openFileDialog(project ? FileUtils::pathToUtf8(project->getAssetsPath()) : std::string());
                    if (!chosen.empty()) importFile(FileUtils::pathFromUtf8(chosen));
                }
            }
            if (lospecResult) {
                const pxc::Palette& found = *lospecResult;
                ImGui::Separator();
                ImGui::Text("%s", found.name.c_str());
                if (!found.author.empty()) ImGui::TextDisabled("by %s", found.author.c_str());
                ImGui::TextDisabled("%d colours", found.count());
                const float cell = Theme::dpi(14.0f);
                const int perRow = std::max(1, (int)(ImGui::GetContentRegionAvail().x / (cell + 1.0f)));
                ImDrawList* drawList = ImGui::GetWindowDrawList();
                for (int i = 0; i < found.count(); i++) {
                    if (i % perRow != 0) ImGui::SameLine(0.0f, 1.0f);
                    const ImVec2 min = ImGui::GetCursorScreenPos();
                    ImGui::Dummy(ImVec2(cell, cell));
                    pwh::drawColor(drawList, min, ImVec2(min.x + cell, min.y + cell), found.colors[i]);
                }
                ImGui::BeginDisabled(!project);
                if (ImGui::Button("Create##lospec_create")) {
                    const std::string slug = found.source.id.empty() ? lospecSlug : found.source.id;
                    if (!createPaletteFile(found, palettesFolder(), slug).empty()) {
                        lospecResult.reset();
                        keepOpen = false;
                    }
                }
                ImGui::EndDisabled();
                ImGui::SetItemTooltip("Write Assets/Palettes/%s.palette and open it", lospecSlug.c_str());
            }
        }
    }
    ImGui::End();
    if (!keepOpen) {
        lospecOpen = false;
        if (lospec) {
            lospec->cancelled = true;
            lospec->client.cancel();
        }
    }
}

// ---------------------------------------------------------------------------------- From Texture

void editor::PaletteWindow::openTextureImport(const fs::path& texture, bool reduce) {
    std::snprintf(textureImportPath, sizeof(textureImportPath), "%s", FileUtils::pathToUtf8(texture).c_str());
    textureImportReduce = reduce;
    textureImportError.clear();
    textureImportOpen = true;
    textureImportFocus = true;
}

void editor::PaletteWindow::drawTextureImportDialog() {
    if (!textureImportOpen) return;
    ImGui::SetNextWindowSize(ImVec2(Theme::dpi(420.0f), 0.0f), ImGuiCond_FirstUseEver);
    if (textureImportFocus) {
        ImGui::SetNextWindowFocus();
        textureImportFocus = false;
    }
    bool keepOpen = true;
    if (ImGui::Begin("From Texture###PaletteFromTexture", &keepOpen, ImGuiWindowFlags_NoCollapse)) {
        const bool running = textureImportTask.isValid();
        ImGui::BeginDisabled(running);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - Theme::dpi(90.0f));
        ImGui::InputText("##from_texture_path", textureImportPath, sizeof(textureImportPath));
        if (ImGui::BeginDragDropTarget()) {
            for (const char* type : {"resource_files", "external_files"}) {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(type)) {
                    const std::vector<std::string> paths = Util::getStringsFromPayload(payload);
                    if (!paths.empty()) std::snprintf(textureImportPath, sizeof(textureImportPath), "%s", paths.front().c_str());
                }
            }
            ImGui::EndDragDropTarget();
        }
        ImGui::SameLine();
        if (ImGui::Button("Browse...##from_texture_browse", ImVec2(-1.0f, 0.0f))) {
            const std::string chosen =
                FileDialogs::openFileDialog(project ? FileUtils::pathToUtf8(project->getAssetsPath()) : std::string(), FILE_DIALOG_IMAGE);
            if (!chosen.empty()) std::snprintf(textureImportPath, sizeof(textureImportPath), "%s", chosen.c_str());
        }
        if (ImGui::RadioButton("Unique Colours", !textureImportReduce)) textureImportReduce = false;
        ImGui::SetItemTooltip("Every colour of the image in reading order (up to 256), or one per square of a swatch strip");
        ImGui::SameLine();
        if (ImGui::RadioButton("Reduce Colours", textureImportReduce)) textureImportReduce = true;
        ImGui::SetItemTooltip("The best N colours for the image: median cut in OKLab, then k-means (deterministic)");
        if (textureImportReduce) ImGui::SliderInt("Colours##from_texture_colors", &textureImportColors, 2, pxc::Palette::kMaxColors, "%d",
                                                  ImGuiSliderFlags_AlwaysClamp);
        ImGui::EndDisabled();
        if (running) {
            const char* spinner = "|/-\\";
            ImGui::Text("%c Reading the texture...", spinner[(int)(ImGui::GetTime() * 8.0) % 4]);
        } else {
            ImGui::BeginDisabled(textureImportPath[0] == 0 || !project);
            if (ImGui::Button("Create##from_texture_create")) {
                textureImportError.clear();
                textureImportSource = FileUtils::pathFromUtf8(textureImportPath);
                if (textureImportSource.is_relative() && project) textureImportSource = project->getProjectPath() / textureImportSource;
                const fs::path source = textureImportSource;
                const bool reduce = textureImportReduce;
                const int colors = textureImportColors;
                textureImportTask = jobs::submit(
                    [source, reduce, colors]() {
                        auto out = std::make_shared<TextureImport>();
                        if (!reduce) {
                            std::vector<uint8_t> bytes;
                            if (!pwh::readBytes(source, bytes)) {
                                out->error = "Cannot read " + FileUtils::pathToUtf8(source) + ".";
                                return out;
                            }
                            pixelart::ImportResult result = pixelart::importPalette(source, bytes);
                            if (result.ok()) out->palette = std::move(*result.palette);
                            else out->error = result.error.message + (result.error.kind == "too_many_colors" ? ": choose Reduce Colours." : "");
                            return out;
                        }
                        std::string error;
                        std::optional<pixelart::RgbaImage> image = pixelart::loadRgbaImage(source, &error);
                        if (!image) {
                            out->error = error.empty() ? "Not an image." : error;
                            return out;
                        }
                        pixelart::ApplyRequest request;
                        request.reduce = colors;
                        const pixelart::ApplyResult reduced = pixelart::applyPalette(*image, request);
                        if (reduced.palette.empty()) {
                            out->error = "The texture has no visible pixels.";
                            return out;
                        }
                        pxc::Palette found;
                        found.name = FileUtils::pathToUtf8(source.stem());
                        found.source.kind = "texture";
                        found.source.file = FileUtils::pathToUtf8(source.filename());
                        found.colors = reduced.palette;
                        out->palette = std::move(found);
                        return out;
                    },
                    {jobs::Lane::Background, jobs::GateClass::Default, "palette-from-texture"});
            }
            ImGui::EndDisabled();
        }
        if (!textureImportError.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::ErrorText);
            ImGui::TextWrapped("%s", textureImportError.c_str());
            ImGui::PopStyleColor();
        }
    }
    ImGui::End();
    if (!keepOpen) textureImportOpen = false;
}

// ---------------------------------------------------------------------------------- Usage (R33)

void editor::PaletteWindow::updateUsage() {
    fs::path texture = pinnedTexture;
    if (texture.empty() && projectWindow) {
        // the Project window's single selected texture; another selection keeps the last one
        const std::vector<fs::path> chosen = projectWindow->getSelectedFiles();
        if (chosen.size() == 1 && pwh::isApplicableTexture(chosen[0])) texture = chosen[0];
        else texture = usageTexture;
    }
    usageTexture = texture;

    if (usageTask.isValid() && usageTask.isReady()) {
        std::shared_ptr<UsageResult> result;
        try {
            result = usageTask.get();
        } catch (const std::exception& e) {
            result = std::make_shared<UsageResult>();
            result->error = e.what();
        }
        usageTask = {};
        usage = result;
        usageShownKey = usageRunningKey;
        usageRunningKey.clear();
        // a highlight being made is of the old picture
        usageMask.retire(retired);
        maskTask = {};
        maskRunning = -1;
        maskColorShown = -1;
        hoveredOffColor = -1;
        if (usage && usage->error.empty()) usageImage.upload(usage->image, retired);
        else usageImage.retire(retired);
    }
    if (maskTask.isValid() && maskTask.isReady()) {
        std::shared_ptr<pixelart::RgbaImage> mask;
        try {
            mask = maskTask.get();
        } catch (const std::exception&) {
        }
        maskTask = {};
        if (mask && usageMask.upload(*mask, retired)) maskColorShown = maskRunning;
        maskRunning = -1;
    }

    if (texture.empty() || !loaded) {
        usageKey.clear();
        return;
    }
    std::error_code ec;
    const auto time = fs::last_write_time(texture, ec);
    const uintmax_t size = ec ? 0 : fs::file_size(texture, ec);
    const uint64_t paletteHash = pwh::colorsHash(palette.colors);
    usageKey = FileUtils::pathToGenericUtf8(texture) + "|" + std::to_string((long long)time.time_since_epoch().count()) + "|" + std::to_string(size) +
               "|" + std::to_string(paletteHash);
    if (usageTask.isValid() || usageKey == usageShownKey) return;

    const std::vector<pxc::Rgba8> colors = palette.colors;
    const std::shared_ptr<UsageCache> cache = usageCache;
    usageRunningKey = usageKey;
    usageTask = jobs::submit(
        [texture, colors, cache, paletteHash]() {
            auto out = std::make_shared<UsageResult>();
            std::string error;
            std::optional<pixelart::RgbaImage> image = pixelart::loadRgbaImage(texture, &error);
            if (!image) {
                out->error = error.empty() ? "Not an image." : error;
                return out;
            }
            // the report of these pixels and these colours, measured once
            const std::pair<uint64_t, uint64_t> key(pixelart::imageContentHash(*image), paletteHash);
            bool cached = false;
            {
                std::lock_guard<std::mutex> lock(cache->mutex);
                auto it = cache->reports.find(key);
                if (it != cache->reports.end()) {
                    out->report = it->second;
                    cached = true;
                }
            }
            if (!cached) {
                out->report = pixelart::measureUsage(*image, colors, 1, 64);
                std::lock_guard<std::mutex> lock(cache->mutex);
                if (cache->reports.size() >= UsageCache::kEntries) cache->reports.erase(cache->reports.begin());
                cache->reports[key] = out->report;
            }
            out->image = std::move(*image);
            return out;
        },
        {jobs::Lane::Background, jobs::GateClass::Default, "palette-usage"});
}

void editor::PaletteWindow::drawUsage() {
    if (usageFocus) {
        ImGui::SetNextItemOpen(true);
        ImGui::SetScrollHereY(1.0f);
        usageFocus = false;
    }
    if (!ImGui::CollapsingHeader("Usage", ImGuiTreeNodeFlags_DefaultOpen)) return;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Texture");
    ImGui::SameLine();
    std::string preview = pinnedTexture.empty() ? (usageTexture.empty() ? std::string("Follow the Project Window")
                                                                         : FileUtils::pathToUtf8(usageTexture.filename()) + " (Project Window)")
                                                : FileUtils::pathToUtf8(pinnedTexture.filename());
    ImGui::SetNextItemWidth(Theme::dpi(240.0f));
    if (ImGui::BeginCombo("##usage_texture", preview.c_str(), ImGuiComboFlags_HeightLarge)) {
        if (ImGui::IsWindowAppearing() && project) {
            textureChoices.clear();
            std::error_code ec;
            const fs::path assets = project->getAssetsPath();
            for (fs::recursive_directory_iterator it(assets, fs::directory_options::skip_permission_denied, ec), end; it != end; it.increment(ec)) {
                if (ec) break;
                const fs::path& path = it->path();
                if (it->is_directory(ec) && !path.filename().empty() && path.filename().string()[0] == '.') {
                    it.disable_recursion_pending();
                    continue;
                }
                if (it->is_regular_file(ec) && pwh::isApplicableTexture(path)) textureChoices.push_back(path);
            }
            std::sort(textureChoices.begin(), textureChoices.end());
        }
        if (ImGui::Selectable("Follow the Project Window##usage_follow", pinnedTexture.empty())) pinnedTexture.clear();
        for (size_t i = 0; i < textureChoices.size(); i++) {
            const std::string name = FileUtils::pathToGenericUtf8(project->normalizeToProjectRelative(textureChoices[i]));
            if (ImGui::Selectable((name + "##usage_texture_" + std::to_string(i)).c_str(), pinnedTexture == textureChoices[i])) {
                pinnedTexture = textureChoices[i];
            }
        }
        ImGui::EndCombo();
    }
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("resource_files")) {
            const std::vector<std::string> paths = Util::getStringsFromPayload(payload);
            if (!paths.empty() && pwh::isApplicableTexture(FileUtils::pathFromUtf8(paths.front()))) pinnedTexture = FileUtils::pathFromUtf8(paths.front());
        }
        ImGui::EndDragDropTarget();
    }
    ImGui::SetItemTooltip("The texture counted: the Project window's selected one, or one pinned here (drop a texture on it)");

    if (usageTexture.empty()) {
        ImGui::TextDisabled("Select a texture in the Project window, or pick one here.");
        return;
    }
    const bool current = usage && usageShownKey == usageKey;
    if (usageTask.isValid() || !current) {
        const char* spinner = "|/-\\";
        ImGui::Text("%c Counting %s...", spinner[(int)(ImGui::GetTime() * 8.0) % 4], FileUtils::pathToUtf8(usageTexture.filename()).c_str());
    }
    if (current && !usage->error.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::ErrorText);
        ImGui::TextWrapped("%s", usage->error.c_str());
        ImGui::PopStyleColor();
    } else if (current) {
        const pixelart::PaletteUsageReport& report = usage->report;
        int used = 0;
        int64_t onPalette = 0;
        for (int64_t count : report.counts) {
            used += count > 0 ? 1 : 0;
            onPalette += count;
        }
        ImGui::Text("%lld pixels on the palette, %d of its %d colours used", (long long)onPalette, used, palette.count());
        if (report.offPalettePixels == 0) {
            ImGui::TextUnformatted("Every visible pixel is a palette colour.");
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::WarningText);
            ImGui::Text("Off-palette: %lld pixels in %lld colours", (long long)report.offPalettePixels, (long long)report.offColorCount);
            ImGui::PopStyleColor();
            const float rowHeight = ImGui::GetTextLineHeightWithSpacing();
            const float listHeight = std::min((float)report.offColors.size(), 8.0f) * rowHeight + Theme::dpi(6.0f);
            if (ImGui::BeginChild("##usage_off_colors", ImVec2(0.0f, listHeight), ImGuiChildFlags_Borders)) {
                ImDrawList* drawList = ImGui::GetWindowDrawList();
                for (int i = 0; i < (int)report.offColors.size(); i++) {
                    const pixelart::PaletteUsageReport::OffColor& off = report.offColors[i];
                    const ImVec2 min = ImGui::GetCursorScreenPos();
                    const float cell = ImGui::GetTextLineHeight();
                    ImGui::Dummy(ImVec2(cell, cell));
                    pwh::drawColor(drawList, min, ImVec2(min.x + cell, min.y + cell), off.color);
                    ImGui::SameLine();
                    char row[160];
                    if (off.nearest >= 0 && palette.validIndex(off.nearest)) {
                        std::snprintf(row, sizeof(row), "%s  %lld px  nearest #%d %s  dE %.1f##usage_off_%d", pwh::hexOf(off.color).c_str(),
                                      (long long)off.count, off.nearest, pwh::hexOf(palette.colors[off.nearest]).c_str(), off.deltaE, i);
                    } else {
                        std::snprintf(row, sizeof(row), "%s  %lld px##usage_off_%d", pwh::hexOf(off.color).c_str(), (long long)off.count, i);
                    }
                    ImGui::Selectable(row, hoveredOffColor == i);
                    if (ImGui::IsItemHovered()) hoveredOffColor = i;
                    else if (hoveredOffColor == i) hoveredOffColor = -1;
                    if (off.nearest >= 0 && ImGui::IsItemClicked()) {
                        setSelection({off.nearest});
                        anchor = off.nearest;
                    }
                }
                if (report.offColorCount > (int64_t)report.offColors.size()) {
                    ImGui::TextDisabled("and %lld more", (long long)(report.offColorCount - (int64_t)report.offColors.size()));
                }
            }
            ImGui::EndChild();
        }

        // the highlight of the hovered colour's pixels, made on a worker
        if (hoveredOffColor >= 0 && hoveredOffColor < (int)report.offColors.size() && hoveredOffColor != maskColorShown && !maskTask.isValid()) {
            const std::shared_ptr<const UsageResult> measured = usage;
            const pxc::Rgba8 wanted = report.offColors[hoveredOffColor].color;
            maskRunning = hoveredOffColor;
            maskTask = jobs::submit(
                [measured, wanted]() {
                    const pixelart::RgbaImage& image = measured->image;
                    auto mask = std::make_shared<pixelart::RgbaImage>(image.width, image.height);
                    for (size_t i = 0; i < image.pixels.size(); i++) {
                        const pxc::Rgba8 pixel = image.pixels[i];
                        if (pixel.a > 0 && pixel.r == wanted.r && pixel.g == wanted.g && pixel.b == wanted.b) mask->pixels[i] = pxc::Rgba8{255, 0, 255, 255};
                    }
                    return mask;
                },
                {jobs::Lane::Background, jobs::GateClass::Default, "palette-usage-mask"});
        }

        if (usageImage.valid()) {
            // integer zoom (or an integer reduction) to fit, Nearest
            const float area = std::max(Theme::dpi(64.0f), std::min(ImGui::GetContentRegionAvail().x, Theme::dpi(320.0f)));
            const float longest = (float)std::max(usageImage.width, usageImage.height);
            const float zoom = longest <= area ? std::floor(area / longest) : 1.0f / std::ceil(longest / area);
            const ImVec2 shown((float)usageImage.width * zoom, (float)usageImage.height * zoom);
            const ImVec2 min = ImGui::GetCursorScreenPos();
            const ImVec2 max(min.x + shown.x, min.y + shown.y);
            ImGui::Dummy(shown);
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            pwh::drawChecker(drawList, min, max);
            drawList->AddImage(usageImage.imguiId(), min, max);
            if (hoveredOffColor >= 0 && hoveredOffColor == maskColorShown && usageMask.valid()) {
                drawList->AddRectFilled(min, max, IM_COL32(0, 0, 0, 140));
                drawList->AddImage(usageMask.imguiId(), min, max);
            }
            drawList->AddRect(min, max, ImGui::GetColorU32(ImGuiCol_Border));
            ImGui::TextDisabled("%d x %d, %s%g", usageImage.width, usageImage.height, zoom >= 1.0f ? "x" : "1/", zoom >= 1.0f ? zoom : 1.0f / zoom);
        }
    }
    ImGui::BeginDisabled(!applyDialog || !loaded);
    if (ImGui::Button("Apply Palette...##usage_apply")) {
        applyDialog->open(usageTexture, FileUtils::pathToGenericUtf8(project->normalizeToProjectRelative(file)), false);
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Map the texture's pixels to this palette (a preview first, then one undo step)");
}
