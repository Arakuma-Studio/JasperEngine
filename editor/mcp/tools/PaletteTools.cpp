// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The palette tools (docs/specs/S43-palettes.md R39-R43, §6.6; group pixel_art; agent guide
// docs/mcp/guide/palettes.md): edit_palette lists, reads, creates (a built-in, Lospec, a palette
// file, a texture, colours), edits (editor/pixelart/PaletteEdit's ops), exports and measures a
// palette; apply_palette maps a texture onto a palette or reduces its colours, with the verdict
// S42's validator reads. Nothing is written without output, and a call writes one project undo step.

#include "McpTools.h"

#include "mcp/McpCall.h"
#include "mcp/McpContext.h"
#include "mcp/McpJobs.h"
#include "mcp/McpRefs.h"
#include "mcp/McpRegistry.h"
#include "mcp/McpSchema.h"
#include "mcp/tools/W4Shared.h"

#include "App.h"
#include "AppSettings.h"
#include "EditorHost.h"
#include "Project.h"
#include "command/CommandHistory.h"
#include "htmlui/HttpClient.h"
#include "pixelart/ApplyPalette.h"
#include "pixelart/BuiltinPalettes.h"
#include "pixelart/Lospec.h"
#include "pixelart/Palette.h"
#include "pixelart/PaletteAssets.h"
#include "pixelart/PaletteEdit.h"
#include "pixelart/PaletteFormats.h"
#include "pixelart/PaletteUsage.h"
#include "pixelart/PixelColor.h"
#include "pixelart/RgbaImage.h"
#include "util/Base64.h"
#include "util/FileUtils.h"
#include "util/Util.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <exception>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <thread>
#include <variant>
#include <vector>

namespace jasperengine::editor::mcp {

// file-local, in a namespace of its own: a unity build merges the MCP tool sources
namespace palettetools {
namespace {

namespace fs = std::filesystem;
namespace px = jasperengine::pixelart;
namespace epx = jasperengine::editor::pixelart;
namespace ehtml = jasperengine::editor::htmlui;

using Problem = epx::PixelProblem;

// a Lospec answer comes within its 10 s timeout; the call waits a little longer for the job
constexpr double kJobWaitSeconds = 30.0;
// apply_palette runs as a job past this many units of work (pixels x the metric's cost): about 1 s
constexpr double kApplyJobUnits = 4.0e6;
constexpr int kSwatchMaxWidth = 1024;
constexpr int kLostListed = 32;
constexpr int kOffColorsListed = 32;

Problem problem(const std::string& kind, const std::string& field, const std::string& message, const std::string& fix) {
    Problem p;
    p.kind = kind;
    p.field = field;
    p.message = message;
    p.fix = fix;
    return p;
}

Json problemJson(const Problem& p) {
    Json j = {{"kind", p.kind}, {"message", p.message}, {"fix", p.fix.empty() ? std::string("see jasper://docs/palettes") : p.fix}};
    if (!p.field.empty()) j["field"] = p.field;
    return j;
}

std::string problemsText(const std::string& headline, const std::vector<Problem>& problems) {
    std::string text = headline;
    for (const Problem& p : problems) text += "\n- " + p.kind + ": " + p.message + (p.fix.empty() ? "" : " Fix: " + p.fix);
    return text;
}

Json problemsJson(const std::vector<Problem>& problems) {
    Json list = Json::array();
    for (const Problem& p : problems) list.push_back(problemJson(p));
    return list;
}

void failCall(const CallPtr& call, const std::string& headline, const std::vector<Problem>& problems) {
    call->fail(problemsText(headline, problems), Json{{"errors", problemsJson(problems)}});
}

Problem playModeProblem() {
    return problem("play_mode", "output", "writes are refused while a scene plays (a preview works)",
                   "stop play (play {action: \"stop\"}), then call again with output");
}

double rounded(double v, int digits) {
    const double scale = std::pow(10.0, digits);
    return std::round(v * scale) / scale;
}

std::string fixed2(double v) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.2f", v);
    return text;
}

bool truthy(const Json& args, const char* key) {
    if (!args.contains(key) || args[key].is_null()) return false;
    if (args[key].is_boolean()) return args[key].get<bool>();
    return !(args[key].is_string() && (args[key].get<std::string>().empty() || args[key] == Json("false")));
}

std::string lowerExtension(const std::string& path) {
    std::string ext = FileUtils::pathToUtf8(FileUtils::pathFromUtf8(path).extension());
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

std::string stemOf(const std::string& path) {
    return FileUtils::pathToUtf8(FileUtils::pathFromUtf8(path).stem());
}

bool readBytes(const fs::path& file, std::string& out) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

bool isUnder(const fs::path& file, const fs::path& root) {
    const fs::path rel = file.lexically_normal().lexically_relative(root.lexically_normal());
    return !rel.empty() && *rel.begin() != "..";
}

// an optional boolean argument: false and a problem when it is something else
bool booleanArg(const Json& args, const char* key, bool fallback, bool& out, std::vector<Problem>& problems) {
    out = fallback;
    if (!args.contains(key) || args[key].is_null()) return true;
    if (!args[key].is_boolean()) {
        problems.push_back(problem("invalid_value", key, std::string(key) + " must be true or false (got " + args[key].dump() + ")",
                                   std::string("pass ") + key + ": true or false, or leave it out"));
        return false;
    }
    out = args[key].get<bool>();
    return true;
}

// a project-relative file the tool reads
bool readablePath(Project* project, const Json& value, const std::string& field, fs::path& absolute, std::string& relative,
                  std::vector<Problem>& problems) {
    if (!value.is_string() || value.get<std::string>().empty()) {
        problems.push_back(problem("invalid_value", field, field + " must be a project-relative path (got " + value.dump() + ")",
                                   "give a path such as assets/sprites/hero.png"));
        return false;
    }
    std::string error;
    if (!McpRefs::resolveProjectPath(project, value.get<std::string>(), absolute, error, true)) {
        problems.push_back(problem("not_found", field, error, "give an existing project-relative path (list_assets lists them)"));
        return false;
    }
    relative = w4::projectRelative(project, absolute);
    return true;
}

// a project-relative file the tool writes (links leading out of the project refused)
bool writablePath(Project* project, const std::string& given, const std::string& field, fs::path& absolute, std::string& relative,
                  std::vector<Problem>& problems) {
    std::string error;
    if (given.empty() || !McpRefs::resolveProjectWritePath(project, given, absolute, error)) {
        problems.push_back(problem("invalid_value", field, error.empty() ? field + " is empty" : error, "give a project-relative path inside the project"));
        return false;
    }
    relative = w4::projectRelative(project, absolute);
    return true;
}

// a palette's file: inside the project, in the assets folder where the game reads palettes
bool paletteTarget(Project* project, const std::string& target, std::vector<Problem>& problems) {
    fs::path absolute;
    std::string error;
    if (!McpRefs::resolveProjectWritePath(project, target, absolute, error)) {
        problems.push_back(problem("invalid_value", "path", error, "give a path inside the project's assets folder"));
        return false;
    }
    if (!isUnder(absolute, project->getAssetsPath())) {
        problems.push_back(problem("invalid_value", "path", target + " is outside the assets folder, where the game reads palettes",
                                   "give a path under " + w4::projectRelative(project, project->getAssetsPath()) + "/"));
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- palette JSON

Json sourceJson(const px::PaletteSource& source) {
    Json j = Json::object();
    if (!source.kind.empty()) j["kind"] = source.kind;
    if (!source.id.empty()) j["id"] = source.id;
    if (!source.url.empty()) j["url"] = source.url;
    if (!source.fetched.empty()) j["fetched"] = source.fetched;
    if (!source.file.empty()) j["file"] = source.file;
    return j;
}

Json paletteJson(const px::Palette& palette) {
    Json colors = Json::array();
    for (const px::Rgba8& color : palette.colors) colors.push_back(px::toHex(color));
    Json ramps = Json::array();
    for (const px::PaletteRamp& ramp : palette.ramps) {
        Json r = {{"name", ramp.name}, {"colors", ramp.colors}};
        if (ramp.cycle != 0.0f) r["cycle"] = rounded(ramp.cycle, 4);
        ramps.push_back(r);
    }
    Json variants = Json::array();
    for (const px::PaletteVariant& variant : palette.variants) {
        Json list = Json::array();
        for (const std::optional<px::Rgba8>& color : variant.colors) list.push_back(color ? Json(px::toHex(*color)) : Json(nullptr));
        variants.push_back({{"name", variant.name}, {"colors", list}});
    }
    Json j = {{"name", palette.name}, {"count", palette.count()}, {"colors", colors}, {"ramps", ramps},
              {"variants", variants}, {"locked", palette.locked}, {"transparent", palette.transparent}};
    if (!palette.author.empty()) j["author"] = palette.author;
    if (!palette.names.empty()) {
        Json names = Json::object();
        for (const auto& [index, name] : palette.names) names[std::to_string(index)] = name;
        j["names"] = names;
    }
    if (!palette.source.empty()) j["source"] = sourceJson(palette.source);
    if (palette.readOnly) j["read_only"] = true;
    return j;
}

std::string assetsRelative(Project* project, const std::string& projectRelative) {
    return FileUtils::pathToGenericUtf8((project->getProjectPath() / FileUtils::pathFromUtf8(projectRelative))
                                            .lexically_normal()
                                            .lexically_relative(project->getAssetsPath().lexically_normal()));
}

// ---------------------------------------------------------------- images

// over a grey checker, so a transparent colour shows as one
px::Rgba8 onChecker(px::Rgba8 c, int x, int y) {
    const int bg = ((x / 4 + y / 4) & 1) ? 0x99 : 0x66;
    auto mix = [&](int v) { return (uint8_t)((v * c.a + bg * (255 - c.a) + 127) / 255); };
    return px::Rgba8{mix(c.r), mix(c.g), mix(c.b), 255};
}

void fillCell(epx::RgbaImage& image, int x0, int y0, int w, int h, px::Rgba8 color) {
    for (int y = std::max(0, y0); y < std::min(image.height, y0 + h); y++) {
        for (int x = std::max(0, x0); x < std::min(image.width, x0 + w); x++) image.set(x, y, onChecker(color, x, y));
    }
}

// the swatches: the colours in their columns, each variant's beside them, a strip per ramp below
epx::RgbaImage swatchImage(const px::Palette& palette) {
    const int gap = 4;
    const int columns = std::clamp(palette.columns, 1, px::Palette::kMaxColumns);
    const int rows = std::max(1, (palette.count() + columns - 1) / columns);
    const int blocks = 1 + (int)palette.variants.size();
    int cell = 16;
    while (cell > 2 && blocks * columns * cell + (blocks + 1) * gap > kSwatchMaxWidth) cell--;
    int width = blocks * columns * cell + (blocks + 1) * gap;
    int height = gap + rows * cell + gap;
    std::vector<int> rampCells;
    for (const px::PaletteRamp& ramp : palette.ramps) {
        const int length = std::max(1, (int)ramp.colors.size());
        const int rampCell = std::max(1, std::min(cell, (kSwatchMaxWidth - 2 * gap) / length));
        rampCells.push_back(rampCell);
        width = std::max(width, 2 * gap + length * rampCell);
        height += std::max(rampCell, 4) + gap;
    }
    epx::RgbaImage image(width, height, px::Rgba8{0x20, 0x20, 0x20, 255});
    for (int block = 0; block < blocks; block++) {
        const int left = gap + block * (columns * cell + gap);
        for (int i = 0; i < palette.count(); i++) {
            const px::Rgba8 color = block == 0 ? palette.colors[i] : palette.variantColor(block - 1, i);
            fillCell(image, left + (i % columns) * cell, gap + (i / columns) * cell, cell, cell, color);
        }
    }
    int y = gap + rows * cell + gap;
    for (size_t r = 0; r < palette.ramps.size(); r++) {
        const px::PaletteRamp& ramp = palette.ramps[r];
        for (size_t k = 0; k < ramp.colors.size(); k++) {
            if (palette.validIndex(ramp.colors[k])) fillCell(image, gap + (int)k * rampCells[r], y, rampCells[r], std::max(rampCells[r], 4), palette.colors[ramp.colors[k]]);
        }
        y += std::max(rampCells[r], 4) + gap;
    }
    return image;
}

// before | after side by side, nearest-sampled at an integer zoom (or an integer reduction when
// the pair is wider than maxWidth)
epx::RgbaImage beforeAfterImage(const epx::RgbaImage& before, const epx::RgbaImage& after, int maxWidth) {
    const int gap = 4;
    const int w = std::max(1, before.width), h = std::max(1, before.height);
    int outW = w, outH = h;
    const int zoom = std::max(1, (maxWidth - gap) / (2 * w));
    if (2 * w + gap > maxWidth) {
        const int shrink = (2 * w + gap + maxWidth - 1) / std::max(1, maxWidth);
        outW = std::max(1, (w + shrink - 1) / shrink);
        outH = std::max(1, (h + shrink - 1) / shrink);
    } else {
        outW = w * zoom;
        outH = h * zoom;
    }
    epx::RgbaImage image(2 * outW + gap, outH, px::Rgba8{0x20, 0x20, 0x20, 255});
    for (int side = 0; side < 2; side++) {
        const epx::RgbaImage& source = side == 0 ? before : after;
        if (!source.valid() || source.empty()) continue;
        const int left = side * (outW + gap);
        for (int y = 0; y < outH; y++) {
            const int sy = std::min(source.height - 1, (int)((int64_t)y * h / outH));
            for (int x = 0; x < outW; x++) {
                const int sx = std::min(source.width - 1, (int)((int64_t)x * w / outW));
                image.set(left + x, y, onChecker(source.at(sx, sy), left + x, y));
            }
        }
    }
    return image;
}

ToolImage pngImage(const epx::RgbaImage& image) {
    const std::string png = epx::encodeRgbaPng(image);
    return ToolImage{Base64::encode(reinterpret_cast<const unsigned char*>(png.data()), png.size()), "image/png"};
}

// ---------------------------------------------------------------- writes

// The files of one call as one project undo step, named in the result as the server names an
// edit's step ({id, name}, McpUndo): a job's result, read after the call replied, still has it
bool writeOneStep(McpContext& ctx, std::vector<w4::WriteFilesCmd::Item> items, Json& undo, std::string& error) {
    if (!w4::writeFilesUndoable(ctx, std::move(items), error)) return false;
    CommandHistory* history = ctx.project()->getProjectCommandHistory();
    const UndoEntry* top = history ? history->top() : nullptr;
    undo = top ? Json{{"id", top->info.id}, {"name", top->name()}} : Json{{"name", "Write Files"}};
    return true;
}

// ---------------------------------------------------------------- edit_palette

struct ExportArgs {
    bool given = false;
    std::string path;            // project-relative
    fs::path absolute;
    epx::PaletteFormat format = epx::PaletteFormat::Gpl;
    int swatchSize = 1;
};

struct EditArgs {
    Json ops;                    // null: none
    ExportArgs exportTo;
    std::string texture;         // project-relative, "" for none
    fs::path textureAbsolute;
    bool image = false;
    bool output = false;
    bool overwrite = false;
};

// the palette an edit starts from and where it goes
struct EditBase {
    px::Palette palette;
    std::string target;          // project-relative .palette
    bool create = false;         // a new file: written on output even without ops
    bool reused = false;         // the project's existing copy of the built-in named in from
    std::string copyText;        // the exact bytes of a built-in copy (written when no op changed it)
    std::vector<std::string> warnings;
};

struct EditOutcome {
    bool ok = false;
    std::string headline;
    std::vector<Problem> errors;
    std::string text;
    Json data;
    std::vector<ToolImage> images;
};

bool parseExport(Project* project, const Json& value, ExportArgs& out, std::vector<Problem>& problems) {
    if (!value.is_object() || !value.contains("path") || !value["path"].is_string()) {
        problems.push_back(problem("invalid_value", "export", "export is {path, format?, swatch_size?}",
                                   "e.g. {\"path\": \"assets/Palettes/hero.gpl\", \"format\": \"gpl\"}"));
        return false;
    }
    for (auto it = value.begin(); it != value.end(); ++it) {
        if (it.key() != "path" && it.key() != "format" && it.key() != "swatch_size") {
            problems.push_back(problem("invalid_value", "export." + it.key(), "export has no key \"" + it.key() + "\"", "valid keys: path, format, swatch_size"));
            return false;
        }
    }
    out.given = true;
    if (!writablePath(project, value["path"].get<std::string>(), "export.path", out.absolute, out.path, problems)) return false;
    const std::string formats = "gpl, hex, pal, pal_riff, txt, ase, png, colors";
    if (value.contains("format")) {
        if (!value["format"].is_string() || !epx::parsePaletteFormat(value["format"].get<std::string>(), out.format) ||
            !epx::canExportPalette(out.format)) {
            problems.push_back(problem("format_unsupported", "export.format", "can't export as " + value["format"].dump(), "use one of: " + formats));
            return false;
        }
    } else {
        // the format by the extension (.pal: JASC, the common one)
        const std::string ext = lowerExtension(out.path);
        bool found = false;
        for (epx::PaletteFormat format : {epx::PaletteFormat::Gpl, epx::PaletteFormat::Hex, epx::PaletteFormat::JascPal, epx::PaletteFormat::PaintNetTxt,
                                          epx::PaletteFormat::AdobeAse, epx::PaletteFormat::PngStrip, epx::PaletteFormat::SwatchLibrary}) {
            if (ext == epx::paletteFormatExtension(format)) {
                out.format = format;
                found = true;
                break;
            }
        }
        if (!found) {
            problems.push_back(problem("format_unsupported", "export.format", "no export format for the extension '" + ext + "'", "add format: one of " + formats));
            return false;
        }
    }
    if (value.contains("swatch_size")) {
        const Json& size = value["swatch_size"];
        if (!size.is_number_integer() || size.get<int>() < 1 || size.get<int>() > 64) {
            problems.push_back(problem("invalid_value", "export.swatch_size", "swatch_size is 1 to 64 pixels (got " + size.dump() + ")", "give 1 to 64 (png only)"));
            return false;
        }
        out.swatchSize = size.get<int>();
    }
    return true;
}

bool parseEditArgs(Project* project, const Json& args, EditArgs& out, std::vector<Problem>& problems) {
    if (args.contains("ops") && !args["ops"].is_null()) {
        if (!args["ops"].is_array()) {
            problems.push_back(problem("invalid_value", "ops", "ops must be a list of {op, ...}", "the ops and their fields: jasper://docs/palettes"));
            return false;
        }
        out.ops = args["ops"];
    }
    if (args.contains("export") && !args["export"].is_null() && !parseExport(project, args["export"], out.exportTo, problems)) return false;
    if (args.contains("texture") && !args["texture"].is_null()) {
        if (!readablePath(project, args["texture"], "texture", out.textureAbsolute, out.texture, problems)) return false;
        if (!Util::isImageFile(out.texture) || lowerExtension(out.texture) == ".svg") {
            problems.push_back(problem("invalid_value", "texture", out.texture + " is not a PNG, BMP, TGA or GIF image", "give a texture image"));
            return false;
        }
    }
    return booleanArg(args, "image", false, out.image, problems) && booleanArg(args, "output", false, out.output, problems) &&
           booleanArg(args, "overwrite", false, out.overwrite, problems);
}

Json usageJson(const epx::PaletteUsageReport& report, const px::Palette& palette, const std::string& texture) {
    Json counts = Json::array();
    for (size_t i = 0; i < report.counts.size(); i++) {
        if (report.counts[i] > 0) counts.push_back({{"index", i}, {"hex", px::toHex(palette.colors[i])}, {"count", report.counts[i]}});
    }
    Json off = Json::array();
    for (const epx::PaletteUsageReport::OffColor& color : report.offColors) {
        Json entry = {{"hex", px::toHex(color.color)}, {"count", color.count}};
        if (color.nearest >= 0) {
            entry["nearest"] = color.nearest;
            entry["delta_e"] = rounded(color.deltaE, 2);
        }
        off.push_back(entry);
    }
    return {{"texture", texture}, {"counts", counts}, {"off_palette_pixels", report.offPalettePixels},
            {"off_palette_colors", report.offColorCount}, {"off_colors", off}};
}

// The rest of an edit once its starting palette is known: the ops, the export, the usage, the
// image, and with output the files as one undo step. Main thread.
EditOutcome finishEdit(McpContext& ctx, const EditArgs& args, EditBase base) {
    EditOutcome out;
    Project* project = ctx.project();
    px::Palette& palette = base.palette;
    const px::Palette before = palette;
    Json data = Json::object();
    std::vector<std::string> warnings = base.warnings;
    Json changed = Json::array();

    if (!args.ops.is_null()) {
        const epx::PaletteEditResult edit = epx::applyPaletteOps(palette, args.ops);
        if (!edit.ok) {
            out.headline = "The ops were not applied (nothing changed):";
            out.errors = edit.errors;
            return out;
        }
        changed = edit.changed;
        warnings.insert(warnings.end(), edit.warnings.begin(), edit.warnings.end());
    } else {
        std::vector<px::Diagnostic> diagnostics;
        px::PaletteCodec::validate(palette, diagnostics);
        for (const px::Diagnostic& d : diagnostics) {
            if (d.severity == px::Diagnostic::Severity::Error) out.errors.push_back(problem(d.kind, d.field, d.message, d.fix));
            else if (d.kind != "unknown_keys") warnings.push_back(d.message);
        }
        if (!out.errors.empty()) {
            out.headline = "Not a valid palette:";
            return out;
        }
    }
    const bool edited = !(palette == before);

    // the export's bytes (written with output)
    std::vector<uint8_t> exported;
    if (args.exportTo.given) {
        std::vector<std::string> exportWarnings;
        auto bytes = epx::exportPalette(palette, args.exportTo.format, exportWarnings, args.exportTo.swatchSize);
        if (std::holds_alternative<epx::PaletteError>(bytes)) {
            Problem p = std::get<epx::PaletteError>(bytes);
            if (p.field.empty()) p.field = "export";
            out.headline = "Can't export the palette:";
            out.errors.push_back(p);
            return out;
        }
        exported = std::get<std::vector<uint8_t>>(bytes);
        data["export"] = {{"path", args.exportTo.path}, {"format", epx::paletteFormatName(args.exportTo.format)}, {"bytes", exported.size()},
                          {"warnings", exportWarnings}};
    }

    if (!args.texture.empty()) {
        std::string error;
        const std::optional<epx::RgbaImage> image = epx::loadRgbaImage(args.textureAbsolute, &error);
        if (!image) {
            out.headline = "Can't count the texture's colours:";
            out.errors.push_back(problem("parse_error", "texture", args.texture + ": " + error, "give a PNG, BMP, TGA or GIF image"));
            return out;
        }
        data["usage"] = usageJson(epx::measureUsage(*image, palette.colors, 1, kOffColorsListed), palette, args.texture);
    }

    const bool writePalette = base.create || edited;
    std::vector<std::string> existing;
    std::error_code ec;
    fs::path targetAbsolute;
    std::vector<Problem> problems;
    const std::string target = base.target;
    if (writePalette && !writablePath(project, target, "path", targetAbsolute, base.target, problems)) {
        out.headline = "Nothing was written:";
        out.errors = problems;
        return out;
    }
    if (base.create && fs::exists(targetAbsolute, ec)) existing.push_back(base.target);
    if (args.exportTo.given && fs::exists(args.exportTo.absolute, ec)) existing.push_back(args.exportTo.path);

    Json written = Json::array();
    std::string writeText;
    if (args.output) {
        if (ctx.isPlaying()) {
            out.headline = "Nothing was written:";
            out.errors.push_back(playModeProblem());
            return out;
        }
        if (writePalette && before.readOnly) {
            out.headline = "Nothing was written:";
            out.errors.push_back(problem("read_only", "path", base.target + " was written by a newer palette format and is read-only here",
                                         "edit it with the editor that wrote it, or write a copy (from: {file: \"" + base.target + "\"} and another path)"));
            return out;
        }
        if (!existing.empty() && !args.overwrite) {
            std::string list;
            for (const std::string& path : existing) list += (list.empty() ? "" : ", ") + path;
            out.headline = "Nothing was written:";
            out.errors.push_back(problem("exists", existing.size() == 1 && existing[0] == args.exportTo.path ? "export.path" : "path",
                                         list + " exists", "pass overwrite: true (one undo step puts it back), or another path"));
            return out;
        }
        std::vector<w4::WriteFilesCmd::Item> items;
        if (writePalette) {
            items.push_back({targetAbsolute, !edited && !base.copyText.empty() ? base.copyText : px::PaletteCodec::write(palette)});
            written.push_back(base.target);
        }
        if (args.exportTo.given) {
            items.push_back({args.exportTo.absolute, std::string(exported.begin(), exported.end())});
            written.push_back(args.exportTo.path);
        }
        if (!items.empty()) {
            Json undo;
            std::string error;
            if (!writeOneStep(ctx, std::move(items), undo, error)) {
                out.headline = "Nothing was written:";
                out.errors.push_back(problem("write_failed", "path", error, "check the folder is writable, then call again"));
                return out;
            }
            data["undo"] = std::move(undo);
            std::string list;
            for (const Json& path : written) list += (list.empty() ? "" : ", ") + path.get<std::string>();
            writeText = " Wrote " + list + " as one undo step (undo with scope project).";
        } else {
            writeText = " Nothing to write: the palette is unchanged.";
        }
        data["written"] = written;
    } else if (writePalette || args.exportTo.given) {
        std::string next = "Preview only, nothing written: call again with the same arguments plus output: true to write " +
                           std::string(writePalette ? base.target : args.exportTo.path) + (writePalette && args.exportTo.given ? " and " + args.exportTo.path : "");
        if (!existing.empty()) next += " (it exists: add overwrite: true)";
        data["next"] = next + ".";
    }

    data["path"] = base.target;
    data["asset_path"] = assetsRelative(project, base.target);
    data["palette"] = paletteJson(palette);
    data["changed"] = changed;
    data["warnings"] = warnings;
    if (base.reused) data["reused"] = true;

    out.text = "Palette " + base.target + " (" + palette.name + ", " + std::to_string(palette.count()) + " colours, " +
               std::to_string(palette.ramps.size()) + " ramps, " + std::to_string(palette.variants.size()) + " variants)" +
               (base.reused ? ": the project's copy of the built-in, reused" : "") + ".";
    if (!changed.empty()) out.text += " Changed " + std::to_string(changed.size()) + " colour(s).";
    if (data.contains("usage")) {
        const Json& usage = data["usage"];
        out.text += " " + args.texture + ": " + std::to_string(usage["off_palette_pixels"].get<int64_t>()) + " off-palette pixels in " +
                    std::to_string(usage["off_palette_colors"].get<int64_t>()) + " colours.";
    }
    out.text += writeText;
    if (data.contains("next")) out.text += " " + data["next"].get<std::string>();
    for (const std::string& warning : warnings) out.text += "\nWarning: " + warning;
    if (args.image) {
        out.images.push_back(pngImage(swatchImage(palette)));
        out.text += "\nImage: the swatches (variants beside them, a strip per ramp below).";
    }
    out.data = data;
    out.ok = true;
    return out;
}

void replyEdit(const CallPtr& call, EditOutcome outcome) {
    if (!outcome.ok) {
        failCall(call, outcome.headline, outcome.errors);
        return;
    }
    ToolResult result = ToolResult::success(outcome.text, outcome.data);
    result.images = std::move(outcome.images);
    call->reply(std::move(result));
}

void listPalettes(const CallPtr& call) {
    Project* project = call->ctx().project();
    Json palettes = Json::array();
    for (const epx::ProjectPalette& entry : epx::listProjectPalettes(*project)) {
        Json item = {{"path", entry.path}, {"asset_path", entry.assetPath}, {"name", entry.palette.name}, {"count", entry.palette.count()}};
        if (!entry.palette.source.kind.empty()) item["source"] = entry.palette.source.kind;
        if (entry.palette.source.kind == "builtin") item["builtin"] = entry.palette.source.id;
        palettes.push_back(item);
    }
    Json builtins = Json::array();
    for (const epx::BuiltinPalette& builtin : epx::builtinPalettes()) {
        builtins.push_back({{"id", builtin.id}, {"name", builtin.palette.name}, {"count", builtin.palette.count()},
                            {"ramps", builtin.palette.ramps.size()}});
    }
    const std::string text = std::to_string(palettes.size()) + " project palette(s), " + std::to_string(builtins.size()) +
                             " built-ins (name one as builtin:<id>; using it copies it into the project once).";
    call->ok(text, Json{{"palettes", palettes}, {"builtins", builtins}});
}

// ---- the Lospec job

struct LospecState {
    McpContext* ctx = nullptr;
    JobPtr job;
    fs::path root;
    EditArgs args;
    std::string slug;
    std::string target;
    std::unique_ptr<ehtml::HttpClient> client = std::make_unique<ehtml::HttpClient>();
    std::optional<px::Palette> palette;
    Problem error;
    std::vector<ToolImage> images;
    std::string text;
};

// Runs `then` on the main thread, posted by the worker as it finishes (it wakes the editor once;
// nothing polls meanwhile). Its writes are the call's undo steps, the agent's when the call is (as
// the server's own continuations are)
void finishOnMainThread(std::optional<UndoOrigin> origin, std::function<void()> then) {
    getEditorHost().enqueueMainThreadTask([origin, then = std::move(then)]() {
        McpWriteScope writeScope;
        std::optional<UndoOriginScope> attribution;
        if (origin) attribution.emplace(*origin);
        then();
    });
}

std::optional<UndoOrigin> originOf(const CallPtr& call) {
    return call->attributed() ? std::optional<UndoOrigin>(call->getUndoOrigin()) : std::nullopt;
}

void finishLospec(const std::shared_ptr<LospecState>& state) {
    const JobPtr& job = state->job;
    if (job->cancelRequested()) {
        job->markCancelled("Lospec fetch cancelled; nothing was written.");
        return;
    }
    Project* project = state->ctx->project();
    if (!project || project->getProjectPath() != state->root) {
        const std::vector<Problem> problems = {problem("project_changed", "", "another project was opened while the palette was fetched",
                                                       "call edit_palette again in the open project")};
        job->fail(problemsText("Nothing was written:", problems), Json{{"errors", problemsJson(problems)}});
        return;
    }
    if (!state->palette) {
        const std::vector<Problem> problems = {state->error};
        job->fail(problemsText("Can't fetch the Lospec palette '" + state->slug + "':", problems), Json{{"errors", problemsJson(problems)}});
        return;
    }
    EditBase base;
    base.palette = *state->palette;
    base.target = state->target;
    base.create = true;
    EditOutcome outcome = finishEdit(*state->ctx, state->args, std::move(base));
    if (!outcome.ok) {
        job->fail(problemsText(outcome.headline, outcome.errors), Json{{"errors", problemsJson(outcome.errors)}});
        return;
    }
    state->images = std::move(outcome.images);
    state->text = outcome.text;
    job->succeed(outcome.text, outcome.data);
}

void startLospec(const CallPtr& call, EditArgs args, const std::string& slug, const std::string& target) {
    McpContext& ctx = call->ctx();
    // offline at once, before any request (R8): the editor's Offline preference, or no curl
    const bool offline = AppSettings::isNetworkOffline();
    if (offline || (ehtml::HttpClient::mirrorDirectory().empty() && !ehtml::HttpClient::available())) {
        failCall(call, "Can't fetch the Lospec palette '" + slug + "':",
                 {problem("network_unavailable", "from.lospec",
                          offline ? "The editor is offline (Preferences > General > Offline, preferences.network.offline): no request was made."
                                  : "Can't reach lospec.com: curl is not installed.",
                          "Download the palette's .hex from " + epx::lospecPageUrl(slug) + ", then edit_palette {from: {file: <its path>}}")});
        return;
    }
    auto state = std::make_shared<LospecState>();
    state->ctx = &ctx;
    state->root = ctx.project()->getProjectPath();
    state->args = std::move(args);
    state->slug = slug;
    state->target = target;
    state->job = ctx.jobs().create("edit_palette", "Fetch Lospec palette " + slug);
    std::weak_ptr<LospecState> weak = state;
    state->job->setCancelHandler([weak]() {
        if (auto s = weak.lock()) s->client->cancel();
    });
    state->job->setProgress(-1.0, "fetching " + epx::lospecJsonUrl(slug));
    const std::optional<UndoOrigin> origin = originOf(call);
    std::thread([state, origin]() {
        try {
            auto fetched = epx::fetchLospec(state->slug, *state->client, epx::todayIsoDate());
            if (std::holds_alternative<px::Palette>(fetched)) state->palette = std::get<px::Palette>(fetched);
            else state->error = std::get<epx::PaletteError>(fetched);
        } catch (const std::exception& e) {
            state->error = problem("network_unavailable", "from.lospec", std::string("Can't reach lospec.com: ") + e.what(),
                                   "Download the palette's .hex from its Lospec page, then edit_palette {from: {file: <its path>}}");
        }
        if (state->error.field.empty()) state->error.field = "from.lospec";
        finishOnMainThread(origin, [state]() {
            try {
                finishLospec(state);
            } catch (const std::exception& e) {
                state->job->fail(std::string("The palette was fetched but could not be handled: ") + e.what());
            }
        });
    }).detach();

    call->detach();
    replyWhenJobDone(call, state->job, true, kJobWaitSeconds, [state](const McpJob& finished) {
        Json result = finished.result();
        result["job_id"] = finished.id();
        result["status"] = jobStatusName(finished.status());
        if (finished.status() != JobStatus::Succeeded) return ToolResult::error(finished.message(), result);
        ToolResult reply = ToolResult::success(state->text.empty() ? finished.message() : state->text, result);
        reply.images = state->images;
        return reply;
    });
}

// where a palette made from a file goes when no path is given: beside it, <stem>.palette
std::string besideSource(const std::string& source) {
    const fs::path path = FileUtils::pathFromUtf8(source);
    return FileUtils::pathToGenericUtf8(path.parent_path() / FileUtils::pathFromUtf8(stemOf(source) + ".palette"));
}

std::string builtinIds() {
    std::string ids;
    for (const epx::BuiltinPalette& entry : epx::builtinPalettes()) ids += (ids.empty() ? "" : ", ") + entry.id;
    return ids;
}

// A new palette from `from` ({builtin} | {file} | {lospec} | {texture, max_colors?} | {colors});
// a Lospec fetch continues as a job (false: the call is handled)
bool baseFromSource(const CallPtr& call, const Json& from, const std::string& givenPath, EditArgs& args, EditBase& base) {
    Project* project = call->ctx().project();
    std::vector<Problem> problems;
    const std::string kinds = "builtin, file, lospec, texture (with max_colors) or colors";
    if (!from.is_object() || from.empty()) {
        failCall(call, "Can't create the palette:", {problem("invalid_value", "from", "from names one source: " + kinds, "e.g. {\"builtin\": \"pico-8\"}")});
        return false;
    }
    std::string kind;
    for (auto it = from.begin(); it != from.end(); ++it) {
        const std::string& key = it.key();
        if (key == "max_colors") continue;
        if (key != "builtin" && key != "file" && key != "lospec" && key != "texture" && key != "colors") {
            failCall(call, "Can't create the palette:", {problem("invalid_value", "from." + key, "from has no source \"" + key + "\"", "use one of: " + kinds)});
            return false;
        }
        if (!kind.empty()) {
            failCall(call, "Can't create the palette:", {problem("invalid_value", "from", "from names one source, not " + kind + " and " + key, "keep one of them")});
            return false;
        }
        kind = key;
    }
    if (kind.empty()) {
        failCall(call, "Can't create the palette:", {problem("invalid_value", "from", "from.max_colors goes with from.texture", "add texture: <path>")});
        return false;
    }
    if (from.contains("max_colors") && kind != "texture") {
        failCall(call, "Can't create the palette:", {problem("invalid_value", "from.max_colors", "max_colors reduces a texture's colours", "use it with from.texture")});
        return false;
    }
    const Json& value = from[kind];
    if (!givenPath.empty() && lowerExtension(givenPath) != ".palette") {
        failCall(call, "Can't create the palette:", {problem("invalid_value", "path", "a palette's path ends in .palette (got \"" + givenPath + "\")",
                                                            "e.g. assets/Palettes/hero.palette")});
        return false;
    }
    std::string target = givenPath;

    if (kind == "builtin") {
        std::string id = value.is_string() ? value.get<std::string>() : std::string();
        if (id.rfind(epx::kBuiltinPrefix, 0) == 0) id = id.substr(std::string(epx::kBuiltinPrefix).size());
        const epx::BuiltinPalette* builtin = epx::findBuiltinPalette(id);
        if (!builtin) {
            failCall(call, "Can't create the palette:", {problem("not_found", "from.builtin", "no built-in palette " + value.dump(), "use one of: " + builtinIds())});
            return false;
        }
        const std::string copy = epx::findBuiltinCopy(*project, builtin->id);
        if (!copy.empty() && (target.empty() || target == copy)) {
            // the second use: the project's copy, as it is now
            const epx::ResolvedPalette resolved = epx::resolvePaletteName(*project, std::string(epx::kBuiltinPrefix) + builtin->id);
            if (!resolved.ok) {
                failCall(call, "Can't read the project's copy:", {resolved.error});
                return false;
            }
            base.palette = resolved.palette;
            base.target = resolved.path;
            base.reused = true;
            return true;
        }
        base.palette = builtin->palette;
        base.copyText = std::string(epx::builtinPaletteText(builtin->id));
        base.target = target.empty() ? epx::builtinCopyPath(*project, builtin->id) : target;
        base.create = true;
        return true;
    }
    if (kind == "lospec") {
        const std::optional<std::string> slug = value.is_string() ? epx::normaliseSlug(value.get<std::string>()) : std::nullopt;
        if (!slug) {
            failCall(call, "Can't fetch the Lospec palette:", {problem("invalid_value", "from.lospec", "not a Lospec palette: " + value.dump(),
                                                                       "give its slug (endesga-32), its lospec.com/palette-list/ address or a lospec-palette:// link")});
            return false;
        }
        if (target.empty()) target = w4::projectRelative(project, project->getAssetsPath() / "Palettes" / FileUtils::pathFromUtf8(*slug + ".palette"));
        if (args.output && call->ctx().isPlaying()) {
            failCall(call, "Nothing was fetched:", {playModeProblem()});
            return false;
        }
        if (!paletteTarget(project, target, problems)) {
            failCall(call, "Can't fetch the Lospec palette:", problems);
            return false;
        }
        startLospec(call, args, *slug, target);
        return false;
    }
    if (kind == "colors") {
        if (!value.is_array() || value.empty()) {
            failCall(call, "Can't create the palette:", {problem("invalid_value", "from.colors", "from.colors is a list of \"#rrggbb\" or \"#rrggbbaa\"", "e.g. [\"#000000\", \"#ffffff\"]")});
            return false;
        }
        if (target.empty()) {
            failCall(call, "Can't create the palette:", {problem("invalid_value", "path", "a palette made from colours needs a path", "add path: assets/Palettes/<name>.palette")});
            return false;
        }
        for (size_t i = 0; i < value.size(); i++) {
            px::Rgba8 color;
            if (!value[i].is_string() || !px::parseHex(value[i].get<std::string>(), color)) {
                failCall(call, "Can't create the palette:", {problem("invalid_value", "from.colors[" + std::to_string(i) + "]", "not a colour: " + value[i].dump(),
                                                                    "write \"#rrggbb\" or \"#rrggbbaa\"")});
                return false;
            }
            base.palette.colors.push_back(color);
        }
        base.palette.name = stemOf(target);
        base.palette.source.kind = "user";
        base.target = target;
        base.create = true;
        return true;
    }

    // a file or a texture of the project
    fs::path absolute;
    std::string relative;
    if (!readablePath(project, value, "from." + kind, absolute, relative, problems)) {
        failCall(call, "Can't create the palette:", problems);
        return false;
    }
    if (target.empty()) target = besideSource(relative);
    std::string bytes;
    if (!readBytes(absolute, bytes)) {
        failCall(call, "Can't create the palette:", {problem("not_found", "from." + kind, "can't read " + relative, "check the file is readable")});
        return false;
    }
    if (kind == "texture" && from.contains("max_colors")) {
        const Json& count = from["max_colors"];
        if (!count.is_number_integer() || count.get<int>() < 2 || count.get<int>() > px::Palette::kMaxColors) {
            failCall(call, "Can't create the palette:", {problem("invalid_value", "from.max_colors", "max_colors is 2 to 256 (got " + count.dump() + ")", "give 2 to 256")});
            return false;
        }
        std::string error;
        const std::optional<epx::RgbaImage> image = epx::decodeRgbaImage(bytes.data(), bytes.size(), &error);
        if (!image) {
            failCall(call, "Can't create the palette:", {problem("parse_error", "from.texture", relative + ": " + error, "give a PNG, BMP, TGA or GIF image")});
            return false;
        }
        epx::ApplyRequest request;
        request.reduce = count.get<int>();
        base.palette.colors = epx::applyPalette(*image, request).palette;
        base.palette.name = stemOf(relative);
        base.palette.source.kind = "texture";
        base.palette.source.file = FileUtils::pathToUtf8(absolute.filename());
    } else {
        const std::span<const uint8_t> view(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
        epx::ImportResult imported = epx::importPalette(absolute, view);
        for (const Problem& warning : imported.warnings) base.warnings.push_back(warning.message);
        if (!imported.ok()) {
            Problem error = imported.error;
            error.field = "from." + kind;
            if (kind == "texture" && error.kind == "too_many_colors") error.fix = "add from.max_colors (2 to 256) to reduce them";
            failCall(call, "Can't create the palette from " + relative + ":", {error});
            return false;
        }
        base.palette = *imported.palette;
        if (kind == "texture") base.palette.source.kind = "texture";
    }
    base.target = target;
    base.create = true;
    return true;
}

void editPaletteTool(const CallPtr& call) {
    McpContext& ctx = call->ctx();
    Project* project = ctx.project();
    const Json& args = call->args();
    const bool hasPath = args.contains("path") && !args["path"].is_null();
    const bool hasFrom = args.contains("from") && !args["from"].is_null();
    if (hasPath && (!args["path"].is_string() || args["path"].get<std::string>().empty())) {
        failCall(call, "Can't edit the palette:", {problem("invalid_value", "path", "path is a project .palette path or builtin:<id>", "e.g. assets/Palettes/hero.palette")});
        return;
    }
    EditArgs edit;
    std::vector<Problem> problems;
    if (!parseEditArgs(project, args, edit, problems)) {
        failCall(call, "Can't edit the palette:", problems);
        return;
    }
    if (!hasPath && !hasFrom) {
        if (!edit.ops.is_null() || edit.exportTo.given || !edit.texture.empty() || edit.output || edit.image) {
            failCall(call, "Which palette?", {problem("invalid_value", "path", "ops, export, texture, image and output need a palette",
                                                      "name it with path (edit_palette with no arguments lists them), or create one with from")});
            return;
        }
        listPalettes(call);
        return;
    }
    const std::string path = hasPath ? args["path"].get<std::string>() : std::string();
    EditBase base;
    if (hasFrom) {
        if (!baseFromSource(call, args["from"], path, edit, base)) return;
    } else {
        const epx::ResolvedPalette resolved = epx::resolvePaletteName(*project, path, false);
        if (!resolved.ok) {
            Problem error = resolved.error;
            error.field = "path";
            failCall(call, "Can't read the palette:", {error});
            return;
        }
        base.palette = resolved.palette;
        base.target = resolved.path;
        base.create = resolved.needsCopy;
        base.copyText = resolved.copyText;
    }
    // what gets written lives in the assets folder (a palette read elsewhere can still be measured)
    if ((base.create || !edit.ops.is_null()) && !paletteTarget(project, base.target, problems)) {
        failCall(call, "Can't edit the palette:", problems);
        return;
    }
    replyEdit(call, finishEdit(ctx, edit, std::move(base)));
}

// ---------------------------------------------------------------- apply_palette

struct ApplyArgs {
    std::string texture;         // project-relative
    fs::path textureAbsolute;
    epx::ApplyRequest request;
    std::string paletteLabel;    // what was applied, for the text
    std::string palettePath;     // the palette file used, "" for colours or none
    bool measureOnly = false;
    bool write = false;
    std::string outputPath;      // project-relative
    fs::path outputAbsolute;
    std::string savePalette;     // project-relative
    fs::path savePaletteAbsolute;
    bool image = true;
    int maxWidth = 768;
};

struct ApplyState {
    McpContext* ctx = nullptr;
    JobPtr job;
    fs::path root;
    ApplyArgs args;
    epx::RgbaImage source;
    epx::ApplyResult result;
    std::string failure;
    std::vector<ToolImage> images;
    std::string text;
};

bool parseApplyArgs(Project* project, const Json& args, ApplyArgs& out, std::vector<Problem>& problems) {
    if (!readablePath(project, args.contains("path") ? args["path"] : Json(), "path", out.textureAbsolute, out.texture, problems)) return false;
    if (!Util::isImageFile(out.texture) || lowerExtension(out.texture) == ".svg") {
        problems.push_back(problem("format_unsupported", "path", out.texture + " is not a PNG, BMP or TGA texture", "give a texture image"));
        return false;
    }
    epx::ApplyRequest& request = out.request;
    if (args.contains("reduce") && !args["reduce"].is_null()) {
        const Json& reduce = args["reduce"];
        if (!reduce.is_number_integer() || reduce.get<int>() < 2 || reduce.get<int>() > px::Palette::kMaxColors) {
            problems.push_back(problem("invalid_value", "reduce", "reduce is 2 to 256 colours (got " + reduce.dump() + ")", "give 2 to 256"));
            return false;
        }
        request.reduce = reduce.get<int>();
    }
    if (args.contains("distance") && !args["distance"].is_null()) {
        if (!args["distance"].is_string() || !px::parseColorMetric(args["distance"].get<std::string>(), request.metric)) {
            problems.push_back(problem("invalid_value", "distance", "unknown distance " + args["distance"].dump(), "use oklab (default), ciede2000 or rgb"));
            return false;
        }
    }
    if (args.contains("dither") && !args["dither"].is_null()) {
        if (!args["dither"].is_string() || !epx::parseDither(args["dither"].get<std::string>(), request.dither)) {
            problems.push_back(problem("invalid_value", "dither", "unknown dither " + args["dither"].dump(), "use none (default), bayer2, bayer4, bayer8 or floyd_steinberg"));
            return false;
        }
    }
    if (args.contains("dither_strength") && !args["dither_strength"].is_null()) {
        const Json& s = args["dither_strength"];
        if (!s.is_number() || !(s.get<double>() >= 0.0 && s.get<double>() <= 1.0)) {
            problems.push_back(problem("invalid_value", "dither_strength", "dither_strength is 0 to 1 (got " + s.dump() + ")", "give 0 to 1 (default 1)"));
            return false;
        }
        request.ditherStrength = s.get<float>();
    }
    if (args.contains("alpha_threshold") && !args["alpha_threshold"].is_null()) {
        const Json& a = args["alpha_threshold"];
        if (!a.is_number_integer() || a.get<int>() < 0 || a.get<int>() > 255) {
            problems.push_back(problem("invalid_value", "alpha_threshold", "alpha_threshold is 0 (keep alpha) to 255 (got " + a.dump() + ")", "give 0 to 255; 128 makes alpha binary"));
            return false;
        }
        request.alphaThreshold = a.get<int>();
    }
    if (!booleanArg(args, "measure_only", false, out.measureOnly, problems) || !booleanArg(args, "image", !out.measureOnly, out.image, problems)) return false;
    if (args.contains("max_width") && !args["max_width"].is_null()) {
        const Json& m = args["max_width"];
        if (!m.is_number_integer() || m.get<int>() < 64 || m.get<int>() > 4096) {
            problems.push_back(problem("invalid_value", "max_width", "max_width is 64 to 4096 pixels (got " + m.dump() + ")", "give 64 to 4096 (default 768)"));
            return false;
        }
        out.maxWidth = m.get<int>();
    }

    // output: false (preview), true (the texture itself) or a new file
    if (args.contains("output") && !args["output"].is_null()) {
        Json output = args["output"];
        // the schema also takes a path, so a model often sends output: true as the string "true"
        if (output == Json("true") || output == Json("false")) output = output == Json("true");
        if (output.is_boolean()) {
            out.write = output.get<bool>();
            if (out.write) {
                out.outputPath = out.texture;
                out.outputAbsolute = out.textureAbsolute;
            }
        } else if (output.is_string() && !output.get<std::string>().empty()) {
            out.write = true;
            if (!writablePath(project, output.get<std::string>(), "output", out.outputAbsolute, out.outputPath, problems)) return false;
        } else {
            problems.push_back(problem("invalid_value", "output", "output is true (overwrite the texture) or a new .png path (got " + output.dump() + ")",
                                       "pass output: true, or output: \"assets/sprites/hero_pal.png\""));
            return false;
        }
    }
    if (out.write && lowerExtension(out.outputPath) != ".png") {
        problems.push_back(problem("format_unsupported", "output", "the result is written as a PNG; " + out.outputPath + " is not one",
                                   "pass output: \"" + FileUtils::pathToGenericUtf8(FileUtils::pathFromUtf8(out.outputPath).replace_extension(".png")) + "\""));
        return false;
    }
    if (args.contains("save_palette") && !args["save_palette"].is_null()) {
        const Json& save = args["save_palette"];
        if (!save.is_string() || lowerExtension(save.get<std::string>()) != ".palette") {
            problems.push_back(problem("invalid_value", "save_palette", "save_palette is a .palette path (got " + save.dump() + ")", "e.g. assets/Palettes/hero.palette"));
            return false;
        }
        if (request.reduce == 0) {
            problems.push_back(problem("invalid_value", "save_palette", "save_palette writes the palette Reduce Colours finds", "add reduce: <colours>"));
            return false;
        }
        if (!writablePath(project, save.get<std::string>(), "save_palette", out.savePaletteAbsolute, out.savePalette, problems)) return false;
    }
    if (out.measureOnly && (out.write || !out.savePalette.empty())) {
        problems.push_back(problem("invalid_value", out.write ? "output" : "save_palette", "measure_only never writes",
                               "drop measure_only to write, or drop output and save_palette to measure"));
        return false;
    }

    // the palette: a path or built-in, colours, or the project's Default Palette
    const Json palette = args.contains("palette") ? args["palette"] : Json();
    if (palette.is_array()) {
        for (size_t i = 0; i < palette.size(); i++) {
            px::Rgba8 color;
            if (!palette[i].is_string() || !px::parseHex(palette[i].get<std::string>(), color)) {
                problems.push_back(problem("invalid_value", "palette[" + std::to_string(i) + "]", "not a colour: " + palette[i].dump(), "write \"#rrggbb\" or \"#rrggbbaa\""));
                return false;
            }
            request.palette.push_back(color);
        }
        if (request.palette.empty()) {
            problems.push_back(problem("invalid_value", "palette", "palette is an empty list", "give colours, a .palette path or builtin:<id>"));
            return false;
        }
        out.paletteLabel = std::to_string(request.palette.size()) + " colours";
    } else if (palette.is_string() || (palette.is_null() && request.reduce == 0)) {
        const epx::ResolvedPalette resolved = epx::resolvePaletteName(*project, palette.is_string() ? palette.get<std::string>() : std::string());
        if (!resolved.ok) {
            Problem error = resolved.error;
            error.field = "palette";
            problems.push_back(error);
            return false;
        }
        request.palette = resolved.palette.colors;
        request.locked = resolved.palette.locked;
        out.palettePath = resolved.path;
        out.paletteLabel = resolved.builtin ? resolved.palette.name + " (built-in)" : resolved.path;
    } else if (!palette.is_null()) {
        problems.push_back(problem("invalid_value", "palette", "palette is a .palette path, builtin:<id> or a list of hex colours (got " + palette.dump() + ")",
                                   "e.g. \"builtin:pico-8\""));
        return false;
    }
    return true;
}

Json applyResultJson(const ApplyArgs& args, const epx::ApplyResult& result) {
    const epx::ApplyStats& stats = result.stats;
    Json usage = Json::array();
    for (const epx::ColorUsage& entry : result.usage) usage.push_back({{"index", entry.index}, {"hex", px::toHex(entry.color)}, {"count", entry.count}});
    Json lost = Json::array();
    for (size_t i = 0; i < result.lost.size() && (int)i < kLostListed; i++) {
        const epx::LostGroup& group = result.lost[i];
        lost.push_back({{"hex", px::toHex(group.original)}, {"mapped", px::toHex(group.mapped)}, {"count", group.count},
                        {"box", Json::array({group.x, group.y, group.w, group.h})}});
    }
    Json data = {{"path", args.texture},
                 {"verdict", epx::verdictName(result.verdict)},
                 {"stats", {{"colors_before", stats.colorsBefore}, {"colors_after", stats.colorsAfter}, {"pixels_changed", stats.pixelsChanged},
                            {"pixels_mapped", stats.pixelsMapped}, {"mean_delta_e", rounded(stats.meanDeltaE, 3)}, {"max_delta_e", rounded(stats.maxDeltaE, 3)}}},
                 {"usage", usage},
                 {"lost", lost},
                 {"lost_groups", result.lost.size()}};
    Json palette = {{"count", result.palette.size()}};
    if (!args.palettePath.empty()) palette["path"] = args.palettePath;
    if (args.request.reduce > 0) {
        Json colors = Json::array();
        for (const px::Rgba8& color : result.palette) colors.push_back(px::toHex(color));
        palette["colors"] = colors;
    }
    data["palette"] = palette;
    // the command line's exit code for the verdict (R43): 0 clean, 3 fixable, 4 needs_person
    if (args.measureOnly) data["cli_exit"] = epx::verdictExitCode(result.verdict);
    return data;
}

// The result of an apply once the pixels are mapped: with output, the files as one undo step.
// Main thread. False with the problems when nothing could be written.
bool finishApply(McpContext& ctx, const ApplyArgs& args, const epx::RgbaImage& source, const epx::ApplyResult& result, Json& data,
                 std::string& text, std::vector<ToolImage>& images, std::vector<Problem>& problems) {
    data = applyResultJson(args, result);
    const epx::ApplyStats& stats = result.stats;
    text = "Apply Palette " + (args.request.reduce > 0 ? "(Reduce Colours to " + std::to_string(args.request.reduce) + ")" : args.paletteLabel) +
           " on " + args.texture + ": " + epx::verdictName(result.verdict) + ". Colours " + std::to_string(stats.colorsBefore) + " -> " +
           std::to_string(stats.colorsAfter) + ", " + std::to_string(stats.pixelsChanged) + " pixels changed, mean dE " + fixed2(stats.meanDeltaE) + ", max dE " + fixed2(stats.maxDeltaE) + ", " +
           std::to_string(result.lost.size()) + " lost-colour group(s).";
    if (args.write) {
        if (ctx.isPlaying()) {
            problems.push_back(playModeProblem());
            return false;
        }
        std::error_code ec;
        if (args.outputPath != args.texture && fs::exists(args.outputAbsolute, ec)) {
            problems.push_back(problem("exists", "output", args.outputPath + " exists", "give a new path, or output: true to overwrite the texture itself"));
            return false;
        }
        // the texture as it is and no palette to save: nothing to write
        if (stats.pixelsChanged == 0 && args.outputPath == args.texture && args.savePalette.empty()) {
            text += " Nothing to write: no pixel changes.";
            if (args.image) {
                images.push_back(pngImage(beforeAfterImage(source, result.image, args.maxWidth)));
                text += "\nImage: before | after.";
            }
            return true;
        }
        std::vector<w4::WriteFilesCmd::Item> items;
        const std::string png = epx::encodeRgbaPng(result.image);
        if (png.empty()) {
            problems.push_back(problem("write_failed", "output", "the result could not be encoded", "call again; report it if it persists"));
            return false;
        }
        items.push_back({args.outputAbsolute, png});
        if (!args.savePalette.empty()) {
            px::Palette found;
            found.name = stemOf(args.savePalette);
            found.colors = result.palette;
            found.source.kind = "texture";
            found.source.file = FileUtils::pathToUtf8(args.textureAbsolute.filename());
            items.push_back({args.savePaletteAbsolute, px::PaletteCodec::write(found)});
        }
        Json undo;
        std::string error;
        if (!writeOneStep(ctx, std::move(items), undo, error)) {
            problems.push_back(problem("write_failed", "output", error, "check the folder is writable, then call again"));
            return false;
        }
        data["written"] = args.outputPath;
        if (!args.savePalette.empty()) data["palette_written"] = args.savePalette;
        data["undo"] = std::move(undo);
        text += " Wrote " + args.outputPath + (args.savePalette.empty() ? "" : " and " + args.savePalette) + " as one undo step (undo with scope project).";
    } else if (!args.measureOnly) {
        data["next"] = "Preview only, nothing written: call again with the same arguments plus output: true (overwrite " + args.texture +
                       ") or output: \"<new .png path>\".";
        text += " " + data["next"].get<std::string>();
    }
    if (result.verdict == epx::Verdict::NeedsPerson && !result.lost.empty()) {
        text += " The lost groups (lost[].box) lose their look: pick a fitting palette, add those colours, or ask a person.";
    }
    if (args.image) {
        images.push_back(pngImage(beforeAfterImage(source, result.image, args.maxWidth)));
        text += "\nImage: before | after.";
    }
    return true;
}

double applyUnits(const epx::RgbaImage& image, const epx::ApplyRequest& request) {
    double perPixel = request.metric == px::ColorMetric::Ciede2000 ? 6.0 : 1.0;
    if (request.dither == epx::Dither::FloydSteinberg) perPixel *= 2.0;
    if (request.reduce > 0) perPixel += 8.0;
    return (double)image.pixelCount() * perPixel;
}

void applyPaletteTool(const CallPtr& call) {
    McpContext& ctx = call->ctx();
    Project* project = ctx.project();
    ApplyArgs args;
    std::vector<Problem> problems;
    if (!parseApplyArgs(project, call->args(), args, problems)) {
        failCall(call, "Can't apply the palette:", problems);
        return;
    }
    if (args.write && ctx.isPlaying()) {
        failCall(call, "Nothing was written:", {playModeProblem()});
        return;
    }
    std::string error;
    std::optional<epx::RgbaImage> image = epx::loadRgbaImage(args.textureAbsolute, &error);
    if (!image) {
        failCall(call, "Can't apply the palette:", {problem("parse_error", "path", args.texture + ": " + error, "give a PNG, BMP or TGA texture")});
        return;
    }

    if (applyUnits(*image, args.request) <= kApplyJobUnits) {
        const epx::ApplyResult result = epx::applyPalette(*image, args.request);
        Json data;
        std::string text;
        std::vector<ToolImage> images;
        if (!finishApply(ctx, args, *image, result, data, text, images, problems)) {
            failCall(call, "Nothing was written:", problems);
            return;
        }
        ToolResult reply = ToolResult::success(text, data);
        reply.images = std::move(images);
        call->reply(std::move(reply));
        return;
    }

    // long work: the mapping on a worker, the result (and the writes) back on the main thread
    auto state = std::make_shared<ApplyState>();
    state->ctx = &ctx;
    state->root = project->getProjectPath();
    state->args = args;
    state->source = std::move(*image);
    state->job = ctx.jobs().create("apply_palette", "Apply Palette " + args.texture + (args.write ? "" : " (preview)"));
    state->job->setProgress(-1.0, "mapping " + std::to_string(state->source.width) + "x" + std::to_string(state->source.height) + " pixels");
    const std::optional<UndoOrigin> origin = originOf(call);
    std::thread([state, origin]() {
        try {
            state->result = epx::applyPalette(state->source, state->args.request);
        } catch (const std::exception& e) {
            state->failure = e.what();
        }
        finishOnMainThread(origin, [state]() {
            const JobPtr& job = state->job;
            Project* current = state->ctx->project();
            std::vector<Problem> problems;
            if (!state->failure.empty()) {
                problems.push_back(problem("failed", "", "the mapping failed: " + state->failure, "call again; report it if it persists"));
            } else if (!current || current->getProjectPath() != state->root) {
                problems.push_back(problem("project_changed", "", "another project was opened while the texture was mapped", "call apply_palette again in the open project"));
            }
            Json data;
            if (problems.empty()) {
                try {
                    if (finishApply(*state->ctx, state->args, state->source, state->result, data, state->text, state->images, problems)) {
                        job->succeed(state->text, data);
                        return;
                    }
                } catch (const std::exception& e) {
                    problems.push_back(problem("failed", "", e.what(), "call again"));
                }
            }
            job->fail(problemsText("Nothing was written:", problems), Json{{"errors", problemsJson(problems)}});
        });
    }).detach();

    call->detach();
    replyWhenJobDone(call, state->job, true, kJobWaitSeconds * 4.0, [state](const McpJob& finished) {
        Json result = finished.result();
        result["job_id"] = finished.id();
        result["status"] = jobStatusName(finished.status());
        if (finished.status() != JobStatus::Succeeded) return ToolResult::error(finished.message(), result);
        ToolResult reply = ToolResult::success(state->text.empty() ? finished.message() : state->text, result);
        reply.images = state->images;
        return reply;
    });
}

} // namespace
} // namespace palettetools

void registerPaletteTools(McpRegistry& registry) {
    using namespace schema;
    auto types = [](std::initializer_list<const char*> names) {
        Json j;
        j["type"] = Json::array();
        for (const char* n : names) j["type"].push_back(n);
        return j;
    };
    {
        ToolDef tool;
        tool.name = "edit_palette";
        tool.title = "Edit Palette";
        tool.description =
            "Palettes (.palette). No path or from: lists project palettes and the built-ins. from creates one (builtin, Lospec "
            "slug, file, texture, colors); ops edit it in one batch (ops and fields: jasper://docs/palettes); export writes "
            "gpl/hex/pal/ase/png...; texture counts its colours. A preview until output: true writes it (one undo). "
            "builtin:<id> names a built-in. Recolour a texture: apply_palette.";
        // nested values stay loose where the handler names every field (ops, the tool budget)
        tool.inputSchema = object({
            {"path", string("")},
            {"from", object({{"builtin", string("")}, {"file", string("")}, {"lospec", string("")}, {"texture", string("")},
                             {"max_colors", integer("", 2, 256)}, {"colors", array(string(""), "")}})},
            {"ops", Json{{"type", "array"}}},
            {"export", object({{"path", string("")}, {"format", stringEnum("", {"gpl", "hex", "pal", "pal_riff", "txt", "ase", "png", "colors"})},
                               {"swatch_size", integer("", 1, 64)}}, {"path"})},
            {"texture", string("")},
            {"image", boolean("")},
            {"output", boolean("")},
            {"overwrite", boolean("")}
        });
        // not idempotent: ops such as add_variant or add_color add again on a second call
        tool.annotations.idempotent = false;
        tool.annotations.openWorld = true;   // a Lospec fetch
        tool.writesFiles = [](const Json& args) { return palettetools::truthy(args, "output") || args.contains("export"); };
        // previews work while playing; the handler refuses writes then (play_mode)
        tool.allowedWhilePlaying = true;
        tool.activity = ToolActivity::Edit;
        tool.handler = palettetools::editPaletteTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "apply_palette";
        tool.title = "Apply Palette";
        tool.description =
            "Maps a texture onto a palette (nearest colour, optional dither) or reduce: N colours: before|after image, stats, "
            "usage, lost colours, verdict clean|fixable|needs_person. A preview until output: true overwrites the texture or "
            "output: \"<path>\" writes a new one (one undo). measure_only only checks. palette: path, builtin:<id> or [hex] "
            "(default: the Default Palette). Palettes: edit_palette.";
        tool.inputSchema = object({
            {"path", string("")},
            {"palette", Json{{"type", Json::array({"string", "array"})}, {"items", {{"type", "string"}}}}},
            {"reduce", integer("", 2, 256)},
            {"distance", stringEnum("", {"oklab", "ciede2000", "rgb"})},
            {"dither", stringEnum("", {"none", "bayer2", "bayer4", "bayer8", "floyd_steinberg"})},
            {"dither_strength", number("", 0, 1)},
            {"alpha_threshold", integer("", 0, 255)},
            {"measure_only", boolean("")},
            {"output", types({"boolean", "string"})},
            {"save_palette", string("")},
            {"image", boolean("")},
            {"max_width", integer("", 64, 4096)}
        }, {"path"});
        tool.annotations.idempotent = true;
        tool.writesFiles = [](const Json& args) { return palettetools::truthy(args, "output") || args.contains("save_palette"); };
        tool.allowedWhilePlaying = true;
        tool.activity = ToolActivity::Edit;
        tool.handler = palettetools::applyPaletteTool;
        registry.addTool(std::move(tool));
    }
}

}
