// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// diff_images (S80 §6.11, AC38): two project images compared with the shared ImageDiff
// (premultiplied by default, the maths of tools/mcp/pngdiff.py --premultiplied), with a
// three-panel image of a, b and their difference.

#include "McpTools.h"

#include "Project.h"
#include "mcp/McpCall.h"
#include "mcp/McpContext.h"
#include "mcp/McpRefs.h"
#include "mcp/McpRegistry.h"
#include "mcp/McpSchema.h"
#include "texsynth/SynthImage.h"
#include "util/Base64.h"
#include "util/FileUtils.h"
#include "util/ImageDiff.h"
#include "util/LunaSVGRuntime.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <sstream>

namespace jasperengine::editor::mcp {

namespace {

using namespace schema;
namespace fs = std::filesystem;

bool loadProjectImage(Project* project, const std::string& given, texsynth::Image& out, std::string& error) {
    fs::path file;
    if (!McpRefs::resolveProjectPath(project, given, file, error, true)) return false;
    texsynth::Image loaded;
    // an SVG (S87's export) is rasterized by LunaSVG at its own size, straight alpha
    std::string extension = file.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    if (extension == ".svg") {
        std::ifstream in(file, std::ios::binary);
        std::ostringstream text;
        text << in.rdbuf();
        const std::string svg = text.str();
        std::vector<uint8_t> pixels;
        int width = 0, height = 0;
        LunaSVGRuntime::ensureFonts();
        if (!in || !LunaSVGRuntime::rasterize(svg.data(), svg.size(), 1.0f, false, pixels, width, height)) {
            error = given + ": the SVG does not rasterize";
            return false;
        }
        out = texsynth::Image::make(width, height, 4, 8);
        std::copy(pixels.begin(), pixels.end(), out.data.begin());
        return true;
    }
    if (!texsynth::loadImage(file, loaded, error)) {
        error = given + ": " + error;
        return false;
    }
    out = toRgba8Image(loaded);
    return true;
}

void diffImagesTool(const CallPtr& call) {
    Project* project = call->ctx().project();
    if (!project) {
        call->fail("No project is open.");
        return;
    }
    const Json& args = call->args();
    texsynth::Image a, b;
    std::string error;
    if (!loadProjectImage(project, args.value("a", std::string()), a, error) || !loadProjectImage(project, args.value("b", std::string()), b, error)) {
        call->fail(error);
        return;
    }
    ImageDiffOptions options;
    options.premultiplied = args.value("mode", std::string("premultiplied")) != "straight";
    options.tolerance = std::clamp(args.value("tolerance", 8), 0, 255);
    options.maxFraction = std::clamp(args.value("max_fraction", 0.001), 0.0, 1.0);
    options.maxMean = std::clamp(args.value("max_mean", 0.25), 0.0, 255.0);
    options.maxDiff = std::clamp(args.value("max_diff", 48), 0, 255);
    if (args.contains("crop")) {
        const Json& crop = args["crop"];
        if (!crop.is_array() || crop.size() != 4) {
            call->fail("`crop` is [x, y, w, h] in image pixels.");
            return;
        }
        options.crop = ImageDiffRect{crop[0].get<int>(), crop[1].get<int>(), crop[2].get<int>(), crop[3].get<int>()};
    }
    const ImageDiffResult diff = diffImages(a, b, options);
    if (!diff.sameSize) {
        call->fail(diff.error + " (" + args.value("a", std::string()) + ", " + args.value("b", std::string()) + ")",
                   Json{{"size_a", Json::array({diff.widthA, diff.heightA})}, {"size_b", Json::array({diff.widthB, diff.heightB})}});
        return;
    }
    Json data = Json::object();
    data["ok"] = diff.ok;
    data["size"] = Json::array({diff.widthA, diff.heightA});
    data["mode"] = options.premultiplied ? "premultiplied" : "straight";
    data["max"] = diff.max;
    data["mean"] = std::round(diff.mean * 10000.0) / 10000.0;
    data["fraction_over"] = diff.fractionOver;
    data["changed_pct"] = std::round(diff.changedPct * 1000.0) / 1000.0;
    data["bbox"] = diff.bbox.empty() ? Json(nullptr) : Json::array({diff.bbox.x, diff.bbox.y, diff.bbox.width, diff.bbox.height});
    data["thresholds"] = Json{{"tolerance", options.tolerance}, {"max_fraction", options.maxFraction}, {"max_mean", options.maxMean},
                              {"max_diff", options.maxDiff}};
    const std::string saveDiff = args.value("save_diff", std::string());
    if (!saveDiff.empty()) {
        fs::path out;
        std::vector<uint8_t> png;
        if (!McpRefs::resolveProjectWritePath(project, saveDiff, out, error) || !texsynth::encodePng(diffImage(a, b, options), png) ||
            !FileUtils::writeFileReplacing(out, std::string(png.begin(), png.end()))) {
            call->fail("save_diff: " + (error.empty() ? "could not write " + saveDiff : error));
            return;
        }
        data["saved_diff"] = project->normalizeToProjectRelative(out).generic_string();
    }
    const std::string text = std::string(diff.ok ? "Match" : "Differ") + ": max " + std::to_string(diff.max) + ", mean " +
                             std::to_string(diff.mean).substr(0, 6) + ", " + std::to_string(diff.changedPct).substr(0, 6) + " % of pixels changed (" +
                             data["mode"].get<std::string>() + ").";
    ToolResult result = ToolResult::success(text, data);
    const std::string image = args.value("image", std::string("on_fail"));
    if (image == "always" || (image == "on_fail" && !diff.ok)) {
        std::vector<uint8_t> png;
        if (texsynth::encodePng(diffPanel(a, b, options), png)) result.images.push_back({Base64::encode(png.data(), png.size()), "image/png"});
    }
    call->reply(std::move(result));
}

}

void registerImageDiffTools(McpRegistry& registry) {
    ToolDef tool;
    tool.name = "diff_images";
    tool.title = "Diff Images";
    tool.description =
        "Compares two images of one size: ok, max, mean, fraction_over, changed_pct, bbox, and a panel a | b | diff x8. "
        "premultiplied ignores colour under alpha 0.";
    tool.inputSchema = object({
        {"a", string("")},
        {"b", string("")},
        {"mode", withDefault(stringEnum("", {"premultiplied", "straight"}), "premultiplied")},
        {"tolerance", withDefault(integer(""), 8)},
        {"max_fraction", withDefault(number(""), 0.001)},
        {"max_mean", withDefault(number(""), 0.25)},
        {"max_diff", withDefault(integer(""), 48)},
        {"crop", array(integer(""), "")},
        {"image", withDefault(stringEnum("", {"on_fail", "always", "none"}), "on_fail")},
        {"save_diff", string("")},
    }, {"a", "b"});
    tool.annotations.readOnly = true;
    tool.annotations.idempotent = true;
    tool.writesFiles = [](const Json& args) { return args.contains("save_diff"); };
    tool.activity = ToolActivity::Read;
    tool.group = "editor_tools";
    tool.handler = diffImagesTool;
    registry.addTool(std::move(tool));
}

}
