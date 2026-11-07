// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AsepriteCli.h"

#include "AsepriteDocument.h"

#include "cli/CommandLine.h"
#include "io/AtomicFile.h"

#include "json.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace jasperengine::editor::pixelart::aseprite {

    namespace clidetail {

        namespace {

            constexpr int kExitOk = 0;
            constexpr int kExitFailed = 1;
            constexpr int kExitUsage = 2;
            constexpr int kExitRead = 3;
            constexpr int kExitDiffer = 4;

            constexpr int kMaxScale = 32;

            using UsagePrinter = void (*)(const std::string& commandName, std::ostream& out);

            void printInfoUsage(const std::string& commandName, std::ostream& out) {
                out << "Usage:\n"
                    << "  " << commandName << " aseprite info <file> [--json] [--cels]\n\n"
                    << "A summary of an .aseprite / .ase file: size, colour mode, frames and duration, the layer\n"
                    << "tree, tags, slices and the read warnings.\n\n"
                    << "Options:\n"
                    << "      --json       The document description as JSON on stdout (the shape get_asset_info\n"
                    << "                   gives); the read warnings go to stderr.\n"
                    << "      --cels       With --json: the full description, every cel included.\n"
                    << "  -h, --help       Show this help.\n\n"
                    << "Exit code: 0 ok, 2 usage error, 3 the file could not be read.\n";
            }

            void printRenderUsage(const std::string& commandName, std::ostream& out) {
                out << "Usage:\n"
                    << "  " << commandName << " aseprite render <file> <out.png> [options]\n\n"
                    << "Draws a frame as Aseprite's export does, or every frame of a tag's playback (once) as a sheet.\n\n"
                    << "Options:\n"
                    << "      --frame <n>                  The frame to draw, from 0. Default: 0.\n"
                    << "      --tag <name>                 The frames of a tag in playback order (forward, reverse,\n"
                    << "                                   ping-pong), one pass. Not with --frame.\n"
                    << "      --layers <a,b,...>           Only these layers (names or paths such as \"Body/Arm\"; a\n"
                    << "                                   group brings its children). A named layer shows even when hidden.\n"
                    << "      --sheet horizontal|rows      Several frames: one row (default), or rows of ceil(sqrt(n)) columns.\n"
                    << "      --scale <n>                  Nearest-neighbour upscale, 1 to 32. Default: 1.\n"
                    << "      --classic-blend              Aseprite's classic blending (its --classic-blend).\n"
                    << "      --legacy-hsl                 Hue and Saturation as Aseprite 1.3.12 and earlier drew them.\n"
                    << "      --flatten-groups             Ignore group opacity and blend modes, as Aseprite's CLI export does.\n"
                    << "  -h, --help                       Show this help.\n\n"
                    << "Exit code: 0 written, 1 the PNG could not be written, 2 usage error (an unknown tag or\n"
                    << "layer included), 3 the file could not be read.\n";
            }

            void printResaveUsage(const std::string& commandName, std::ostream& out) {
                out << "Usage:\n"
                    << "  " << commandName << " aseprite resave <file> <out> [--expect-identical]\n\n"
                    << "Reads a file and writes it back through the codec (unchanged chunks keep their bytes).\n\n"
                    << "Options:\n"
                    << "      --expect-identical   Exit 4 when the written bytes differ from the input, naming the\n"
                    << "                           frame and chunk of the first difference (the output is still written).\n"
                    << "  -h, --help               Show this help.\n\n"
                    << "Exit code: 0 written, 1 the file could not be encoded or written, 2 usage error,\n"
                    << "3 the file could not be read, 4 the output differs (--expect-identical).\n";
            }

            int usageError(const std::string& commandName, const char* sub, const std::string& message, UsagePrinter printer,
                           std::ostream& err) {
                // a usage printer follows a malformed command line; an unknown tag or layer names the valid ones instead
                err << commandName << " aseprite" << (sub ? std::string(" ") + sub : std::string()) << ": " << message << "\n";
                if (printer) {
                    err << "\n";
                    printer(commandName, err);
                }
                return kExitUsage;
            }

            bool isHelp(const std::string& arg) { return arg == "-h" || arg == "--help"; }

            bool parseIntArg(const std::string& text, int lo, int hi, int& value) {
                if (text.empty()) return false;
                char* end = nullptr;
                const long v = std::strtol(text.c_str(), &end, 10);
                if (*end != 0 || v < lo || v > hi) return false;
                value = static_cast<int>(v);
                return true;
            }

            // One argument list: positionals and options, "--name value" or "--name=value"
            struct ParsedArgs {
                std::vector<std::string> positional;
                std::vector<std::pair<std::string, std::string>> options;   // name, value ("" for flags)
                bool help = false;
            };

            // `withValue`: the options that take a value; `flags`: those that do not. False with
            // `error` for an unknown option or one without its value.
            bool splitArgs(int argc, char** argv, int first, const std::vector<std::string>& withValue,
                           const std::vector<std::string>& flags, ParsedArgs& parsed, std::string& error) {
                auto known = [](const std::vector<std::string>& list, const std::string& name) {
                    return std::find(list.begin(), list.end(), name) != list.end();
                };
                for (int i = first; i < argc; i++) {
                    const std::string arg = argv[i];
                    if (isHelp(arg)) {
                        parsed.help = true;
                        continue;
                    }
                    if (arg.size() < 2 || arg[0] != '-') {
                        parsed.positional.push_back(arg);
                        continue;
                    }
                    std::string name = arg;
                    std::optional<std::string> inlineValue;
                    const size_t eq = arg.find('=');
                    if (arg.rfind("--", 0) == 0 && eq != std::string::npos) {
                        name = arg.substr(0, eq);
                        inlineValue = arg.substr(eq + 1);
                    }
                    if (known(flags, name)) {
                        if (inlineValue) {
                            error = name + " takes no value";
                            return false;
                        }
                        parsed.options.emplace_back(name, "");
                    } else if (known(withValue, name)) {
                        if (inlineValue) {
                            parsed.options.emplace_back(name, *inlineValue);
                        } else if (i + 1 < argc) {
                            parsed.options.emplace_back(name, argv[++i]);
                        } else {
                            error = name + " needs a value";
                            return false;
                        }
                    } else {
                        error = "unknown option '" + arg + "'";
                        return false;
                    }
                }
                return true;
            }

            void printProblem(std::ostream& err, const std::string& prefix, const char* label, const CodecError& e) {
                err << prefix << label << ": " << e.kind << ": " << e.message;
                if (!e.field.empty()) err << " (" << e.field << ")";
                err << "\n";
                if (!e.fix.empty()) err << prefix << "  fix: " << e.fix << "\n";
            }

            void printWarnings(std::ostream& err, const std::string& prefix, const std::vector<CodecError>& warnings) {
                for (const CodecError& w : warnings) printProblem(err, prefix, "warning", w);
            }

            // The read for info and render (no provenance: nothing is written back)
            std::optional<Document> loadDocument(const std::filesystem::path& path, const std::string& prefix, std::ostream& err,
                                                 std::vector<CodecError>& warnings) {
                ReadOptions options;
                options.provenance = false;
                ReadResult result = readFile(path, options);
                if (!result.ok()) {
                    printProblem(err, prefix, "error", result.error);
                    return std::nullopt;
                }
                if (result.document->frameCount() < 1) {
                    CodecError e;
                    e.kind = "invalid_document";
                    e.message = "the sprite has no frames";
                    e.fix = "open the file in Aseprite and save it again";
                    e.field = "header.frames";
                    printProblem(err, prefix, "error", e);
                    return std::nullopt;
                }
                warnings = std::move(result.warnings);
                return std::move(result.document);
            }

            std::string layerFlagsText(const Document& doc, int index) {
                const Layer& l = doc.layers[static_cast<size_t>(index)];
                std::string text = l.isGroup() ? "group" : l.type == LayerType::Tilemap ? "tilemap" : "image";
                if (!l.visible()) text += ", hidden";
                if (l.reference()) text += ", reference";
                if (l.background()) text += ", background";
                if (l.blend != BlendMode::Normal) text += std::string(", ") + blendModeName(l.blend);
                if (l.opacity != 255) text += ", opacity " + std::to_string(l.opacity);
                return text;
            }

            int layerDepth(const Document& doc, int index) {
                int depth = 0;
                for (int p = doc.layers[static_cast<size_t>(index)].parent, guard = 0;
                     p >= 0 && p < static_cast<int>(doc.layers.size()) && guard <= static_cast<int>(doc.layers.size());
                     p = doc.layers[static_cast<size_t>(p)].parent, guard++) {
                    depth++;
                }
                return depth;
            }

            std::string rectText(const IntRect& r) {
                return std::to_string(r.x) + "," + std::to_string(r.y) + " " + std::to_string(r.w) + "x" + std::to_string(r.h);
            }

            void printSummary(std::ostream& out, const std::string& file, const Document& doc, const std::vector<CodecError>& warnings) {
                int total = 0;
                bool uniform = true;
                for (uint16_t d : doc.frameDurations) {
                    total += d;
                    uniform = uniform && d == doc.frameDurations.front();
                }
                out << file << ": " << doc.width << "x" << doc.height << " " << colorModeName(doc.mode) << ", " << doc.frameCount()
                    << (doc.frameCount() == 1 ? " frame, " : " frames, ") << total << " ms";
                if (uniform && doc.frameCount() > 1) {
                    out << " (" << doc.frameDurations.front() << " ms each)";
                }
                out << "\n";
                if (!uniform) {
                    out << "durations (ms):";
                    for (uint16_t d : doc.frameDurations) out << " " << d;
                    out << "\n";
                }
                out << "palette: " << doc.palette.size() << " colours";
                if (doc.mode == ColorMode::Indexed) out << ", transparent index " << static_cast<int>(doc.transparentIndex);
                out << "\n";

                out << "layers (" << doc.layers.size() << ", bottom first):\n";
                for (int i = 0; i < static_cast<int>(doc.layers.size()); i++) {
                    out << "  " << std::string(static_cast<size_t>(layerDepth(doc, i)) * 2, ' ') << i << " "
                        << doc.layers[static_cast<size_t>(i)].name << " (" << layerFlagsText(doc, i) << ")\n";
                }
                out << "tags (" << doc.tags.size() << "):\n";
                for (const Tag& t : doc.tags) {
                    out << "  " << t.name << ": frames " << t.from << "-" << t.to << ", " << tagDirectionName(t.direction);
                    if (t.repeat > 0) out << ", repeat " << t.repeat;
                    out << "\n";
                }
                out << "slices (" << doc.slices.size() << "):\n";
                for (const Slice& s : doc.slices) {
                    out << "  " << s.name << ":";
                    if (!s.keys.empty()) out << " " << rectText(s.keys.front().bounds);
                    if (s.keys.size() > 1) out << " (" << s.keys.size() << " keys)";
                    bool ninePatch = false, pivot = false;
                    for (const SliceKey& k : s.keys) {
                        ninePatch = ninePatch || k.center.has_value();
                        pivot = pivot || k.pivot.has_value();
                    }
                    if (ninePatch) out << ", 9-patch";
                    if (pivot) out << ", pivot";
                    out << "\n";
                }
                if (!doc.tilesets.empty()) out << "tilesets: " << doc.tilesets.size() << "\n";
                if (warnings.empty()) {
                    out << "warnings: none\n";
                } else {
                    out << "warnings (" << warnings.size() << "):\n";
                    for (const CodecError& w : warnings) {
                        out << "  " << w.kind << ": " << w.message;
                        if (!w.field.empty()) out << " (" << w.field << ")";
                        out << "\n";
                    }
                }
            }

            int runInfo(int argc, char** argv, const std::string& commandName, std::ostream& out, std::ostream& err) {
                ParsedArgs args;
                std::string error;
                if (!splitArgs(argc, argv, 2, {}, {"--json", "--cels"}, args, error)) {
                    return usageError(commandName, "info", error, printInfoUsage, err);
                }
                if (args.help) {
                    printInfoUsage(commandName, out);
                    return kExitOk;
                }
                if (args.positional.size() != 1) {
                    return usageError(commandName, "info", args.positional.empty() ? "needs a file" : "takes one file", printInfoUsage, err);
                }
                bool json = false, cels = false;
                for (const auto& option : args.options) {
                    json = json || option.first == "--json";
                    cels = cels || option.first == "--cels";
                }
                if (cels && !json) {
                    return usageError(commandName, "info", "--cels needs --json", printInfoUsage, err);
                }
                const std::string prefix = commandName + " aseprite info: ";
                std::vector<CodecError> warnings;
                const std::optional<Document> doc = loadDocument(CommandLine::absoluteArgPath(args.positional[0]), prefix, err, warnings);
                if (!doc) return kExitRead;
                if (json) {
                    // stdout holds exactly the description; the warnings are the caller's to see on stderr
                    out << describe(*doc, cels ? Detail::Cels : Detail::Summary).dump(2, ' ', false, nlohmann::json::error_handler_t::replace) << "\n";
                    printWarnings(err, prefix, warnings);
                } else {
                    printSummary(out, args.positional[0], *doc, warnings);
                }
                return kExitOk;
            }

            // A tag's frames in the order one pass of its playback shows them
            std::vector<int> tagFrames(const Tag& tag, int frameCount) {
                int from = std::clamp(tag.from, 0, frameCount - 1);
                int to = std::clamp(tag.to, 0, frameCount - 1);
                if (from > to) std::swap(from, to);
                std::vector<int> frames;
                switch (tag.direction) {
                    case TagDirection::Forward:
                        for (int f = from; f <= to; f++) frames.push_back(f);
                        break;
                    case TagDirection::Reverse:
                        for (int f = to; f >= from; f--) frames.push_back(f);
                        break;
                    case TagDirection::PingPong:
                        for (int f = from; f <= to; f++) frames.push_back(f);
                        for (int f = to - 1; f > from; f--) frames.push_back(f);
                        break;
                    case TagDirection::PingPongReverse:
                        for (int f = to; f >= from; f--) frames.push_back(f);
                        for (int f = from + 1; f < to; f++) frames.push_back(f);
                        break;
                }
                return frames;
            }

            PixelImage upscale(const PixelImage& src, int scale) {
                if (scale == 1) return src;
                PixelImage dst(src.width() * scale, src.height() * scale);
                std::span<uint8_t> pixels = dst.editPixels();
                const size_t dstWidth = static_cast<size_t>(dst.width());
                for (int y = 0; y < dst.height(); y++) {
                    const uint8_t* row = src.row(y / scale);
                    uint8_t* to = pixels.data() + static_cast<size_t>(y) * dstWidth * 4;
                    for (int x = 0; x < dst.width(); x++) {
                        std::memcpy(to + static_cast<size_t>(x) * 4, row + static_cast<size_t>(x / scale) * 4, 4);
                    }
                }
                return dst;
            }

            std::string layerList(const Document& doc) {
                std::string list;
                for (int i = 0; i < static_cast<int>(doc.layers.size()); i++) {
                    list += (i ? ", " : "") + std::string("\"") + doc.layerPath(i) + "\"";
                }
                return list.empty() ? std::string("none") : list;
            }

            std::string tagList(const Document& doc) {
                std::string list;
                for (size_t i = 0; i < doc.tags.size(); i++) list += (i ? ", " : "") + std::string("\"") + doc.tags[i].name + "\"";
                return list.empty() ? std::string("none") : list;
            }

            int runRender(int argc, char** argv, const std::string& commandName, std::ostream& out, std::ostream& err) {
                ParsedArgs args;
                std::string error;
                if (!splitArgs(argc, argv, 2, {"--frame", "--tag", "--layers", "--sheet", "--scale"},
                               {"--classic-blend", "--legacy-hsl", "--flatten-groups"}, args, error)) {
                    return usageError(commandName, "render", error, printRenderUsage, err);
                }
                if (args.help) {
                    printRenderUsage(commandName, out);
                    return kExitOk;
                }
                if (args.positional.size() != 2) {
                    return usageError(commandName, "render",
                                      args.positional.size() < 2 ? "needs a file and an output PNG" : "takes one file and one output PNG",
                                      printRenderUsage, err);
                }
                std::optional<int> frame;
                std::optional<std::string> tagName;
                std::string layers;
                bool layersGiven = false;
                bool rows = false;
                int scale = 1;
                RenderOptions options;
                for (const auto& [name, value] : args.options) {
                    if (name == "--frame") {
                        int f = 0;
                        if (!parseIntArg(value, 0, kMaxFrames - 1, f)) {
                            return usageError(commandName, "render", "--frame '" + value + "' is not a frame number (0 or more)", printRenderUsage, err);
                        }
                        frame = f;
                    } else if (name == "--tag") {
                        tagName = value;
                    } else if (name == "--layers") {
                        layers = value;
                        layersGiven = true;
                    } else if (name == "--sheet") {
                        if (value != "horizontal" && value != "rows") {
                            return usageError(commandName, "render", "--sheet '" + value + "' is neither horizontal nor rows", printRenderUsage, err);
                        }
                        rows = value == "rows";
                    } else if (name == "--scale") {
                        if (!parseIntArg(value, 1, kMaxScale, scale)) {
                            return usageError(commandName, "render", "--scale '" + value + "' is not a whole number from 1 to 32", printRenderUsage, err);
                        }
                    } else if (name == "--classic-blend") {
                        options.newBlend = false;
                    } else if (name == "--legacy-hsl") {
                        options.legacyHsl = true;
                    } else if (name == "--flatten-groups") {
                        options.groups = GroupMode::Flatten;
                    }
                }
                if (frame && tagName) {
                    return usageError(commandName, "render", "--frame and --tag do not go together", printRenderUsage, err);
                }

                const std::string prefix = commandName + " aseprite render: ";
                std::vector<CodecError> warnings;
                const std::optional<Document> doc = loadDocument(CommandLine::absoluteArgPath(args.positional[0]), prefix, err, warnings);
                if (!doc) return kExitRead;
                printWarnings(err, prefix, warnings);

                if (layersGiven) {
                    size_t start = 0;
                    while (start <= layers.size()) {
                        const size_t comma = layers.find(',', start);
                        const std::string item = layers.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
                        const int index = doc->findLayer(item);
                        if (index < 0) {
                            return usageError(commandName, "render", "no layer '" + item + "'; the layers are " + layerList(*doc), nullptr, err);
                        }
                        options.layers.push_back(index);
                        if (comma == std::string::npos) break;
                        start = comma + 1;
                    }
                }

                std::vector<int> frames;
                if (tagName) {
                    const Tag* tag = nullptr;
                    for (const Tag& t : doc->tags) {
                        if (t.name == *tagName) {
                            tag = &t;
                            break;
                        }
                    }
                    if (!tag) {
                        return usageError(commandName, "render", "no tag '" + *tagName + "'; the tags are " + tagList(*doc), nullptr, err);
                    }
                    frames = tagFrames(*tag, doc->frameCount());
                } else {
                    const int f = frame.value_or(0);
                    if (f >= doc->frameCount()) {
                        return usageError(commandName, "render",
                                          "--frame " + std::to_string(f) + " is past the last frame (" + std::to_string(doc->frameCount() - 1) + ")",
                                          nullptr, err);
                    }
                    frames.push_back(f);
                }

                const int n = static_cast<int>(frames.size());
                const int columns = rows ? static_cast<int>(std::ceil(std::sqrt(static_cast<double>(n)))) : n;
                const int lines = (n + columns - 1) / columns;
                const uint64_t sheetW = static_cast<uint64_t>(columns) * static_cast<uint64_t>(doc->width) * static_cast<uint64_t>(scale);
                const uint64_t sheetH = static_cast<uint64_t>(lines) * static_cast<uint64_t>(doc->height) * static_cast<uint64_t>(scale);
                if (sheetW > static_cast<uint64_t>(PixelImage::kMaxSide) || sheetH > static_cast<uint64_t>(PixelImage::kMaxSide) ||
                    sheetW * sheetH * 4 > kMaxDecodedPixelBytes) {
                    return usageError(commandName, "render",
                                      "the sheet would be " + std::to_string(sheetW) + "x" + std::to_string(sheetH) +
                                      " pixels (at most 65535 a side and 1 GiB); use a smaller --scale or --sheet rows",
                                      nullptr, err);
                }

                PixelImage sheet(columns * doc->width, lines * doc->height);
                for (int i = 0; i < n; i++) {
                    sheet.blit(renderFrame(*doc, frames[static_cast<size_t>(i)], options), (i % columns) * doc->width, (i / columns) * doc->height);
                }
                const PixelImage image = upscale(sheet, scale);
                const std::filesystem::path outPath = CommandLine::absoluteArgPath(args.positional[1]);
                if (!image.savePng(outPath)) {
                    err << prefix << "error: could not write " << outPath.generic_string() << " (check the folder exists and is writable)\n";
                    return kExitFailed;
                }
                out << "wrote " << outPath.generic_string() << " (" << image.width() << "x" << image.height() << ", " << n
                    << (n == 1 ? " frame)\n" : " frames)\n");
                return kExitOk;
            }

            uint16_t le16(std::span<const uint8_t> b, size_t at) {
                return static_cast<uint16_t>(b[at] | (b[at + 1] << 8));
            }

            uint32_t le32(std::span<const uint8_t> b, size_t at) {
                return static_cast<uint32_t>(b[at]) | (static_cast<uint32_t>(b[at + 1]) << 8) | (static_cast<uint32_t>(b[at + 2]) << 16) |
                       (static_cast<uint32_t>(b[at + 3]) << 24);
            }

            std::string chunkTypeText(uint16_t type) {
                char hex[8];
                std::snprintf(hex, sizeof(hex), "0x%04X", static_cast<unsigned>(type));
                const char* name = nullptr;
                switch (type) {
                    case kChunkOldPalette: case kChunkOldPalette64: case kChunkOldPalette64Doc: name = "old palette"; break;
                    case kChunkLayer: name = "layer"; break;
                    case kChunkCel: name = "cel"; break;
                    case kChunkCelExtra: name = "cel extra"; break;
                    case kChunkColorProfile: name = "colour profile"; break;
                    case kChunkExternalFiles: name = "external files"; break;
                    case kChunkMask: name = "mask"; break;
                    case kChunkPath: name = "path"; break;
                    case kChunkTags: name = "tags"; break;
                    case kChunkPalette: name = "palette"; break;
                    case kChunkUserData: name = "user data"; break;
                    case kChunkOldSlices: case kChunkSlice: name = "slice"; break;
                    case kChunkTileset: name = "tileset"; break;
                    default: break;
                }
                return name ? std::string(hex) + " (" + name + ")" : std::string(hex);
            }

            // Where a byte of an .aseprite file is: a header field, a frame header or a chunk.
            // Walks the frames as the reader does, stopping where the bytes stop making sense.
            std::string locateByte(std::span<const uint8_t> bytes, size_t offset) {
                if (offset < 128) {
                    struct Field { size_t from, to; const char* name; };
                    static const Field fields[] = {
                        {0, 4, "file size"}, {4, 6, "magic"}, {6, 8, "frame count"}, {8, 10, "width"}, {10, 12, "height"},
                        {12, 14, "colour depth"}, {14, 18, "flags"}, {18, 20, "speed"}, {28, 29, "transparent index"},
                        {32, 34, "palette size"}, {34, 35, "pixel width"}, {35, 36, "pixel height"}, {36, 40, "grid position"},
                        {40, 44, "grid size"},
                    };
                    for (const Field& f : fields) {
                        if (offset >= f.from && offset < f.to) return std::string("the header's ") + f.name + " field";
                    }
                    return "a reserved byte of the header";
                }
                if (bytes.size() < 128) return "past the end of the input";
                const int frameCount = le16(bytes, 6);
                size_t pos = 128;
                for (int frame = 0; frame < frameCount; frame++) {
                    if (bytes.size() - pos < 16) break;
                    const uint32_t frameSize = le32(bytes, pos);
                    if (frameSize < 16 || frameSize > bytes.size() - pos) break;
                    const size_t frameEnd = pos + frameSize;
                    if (offset < pos + 16) {
                        return "frame " + std::to_string(frame) + "'s header (frame at byte " + std::to_string(pos) + ")";
                    }
                    if (offset < frameEnd) {
                        const uint16_t oldCount = le16(bytes, pos + 6);
                        const uint32_t newCount = le32(bytes, pos + 12);
                        uint32_t chunks = oldCount;
                        if ((oldCount == 0xFFFF && oldCount < newCount) || (oldCount == 0 && newCount > 0)) chunks = newCount;
                        size_t cp = pos + 16;
                        for (uint32_t i = 0; i < chunks && frameEnd - cp >= 6; i++) {
                            const uint32_t chunkSize = le32(bytes, cp);
                            if (chunkSize < 6 || chunkSize > frameEnd - cp) break;
                            if (offset < cp + chunkSize) {
                                return "frame " + std::to_string(frame) + ", chunk " + chunkTypeText(le16(bytes, cp + 4)) + " at byte " +
                                       std::to_string(cp) + " (" + std::to_string(offset - cp) + " bytes into it)";
                            }
                            cp += chunkSize;
                        }
                        return "frame " + std::to_string(frame) + ", after its chunks";
                    }
                    pos = frameEnd;
                }
                return "after the last frame";
            }

            bool readBytes(const std::filesystem::path& path, std::vector<uint8_t>& bytes) {
                std::ifstream in(path, std::ios::binary);
                if (!in) return false;
                bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
                return !in.bad();
            }

            int runResave(int argc, char** argv, const std::string& commandName, std::ostream& out, std::ostream& err) {
                ParsedArgs args;
                std::string error;
                if (!splitArgs(argc, argv, 2, {}, {"--expect-identical"}, args, error)) {
                    return usageError(commandName, "resave", error, printResaveUsage, err);
                }
                if (args.help) {
                    printResaveUsage(commandName, out);
                    return kExitOk;
                }
                if (args.positional.size() != 2) {
                    return usageError(commandName, "resave",
                                      args.positional.size() < 2 ? "needs a file and an output path" : "takes one file and one output path",
                                      printResaveUsage, err);
                }
                const bool expectIdentical = !args.options.empty();
                const std::string prefix = commandName + " aseprite resave: ";
                const std::filesystem::path inPath = CommandLine::absoluteArgPath(args.positional[0]);
                const std::filesystem::path outPath = CommandLine::absoluteArgPath(args.positional[1]);

                std::vector<uint8_t> input;
                if (!readBytes(inPath, input)) {
                    CodecError e;
                    e.kind = "import_failed";
                    e.message = "cannot open " + inPath.generic_string();
                    e.fix = "check that the file exists and can be read";
                    e.field = "path";
                    printProblem(err, prefix, "error", e);
                    return kExitRead;
                }
                ReadResult result = read(input);
                if (!result.ok()) {
                    printProblem(err, prefix, "error", result.error);
                    return kExitRead;
                }
                printWarnings(err, prefix, result.warnings);

                std::vector<uint8_t> output;
                CodecError writeError;
                if (!write(*result.document, output, &writeError)) {
                    printProblem(err, prefix, "error", writeError);
                    return kExitFailed;
                }
                const io::WriteResult written =
                    io::writeFileAtomic(outPath, std::string_view(reinterpret_cast<const char*>(output.data()), output.size()));
                if (!written.ok()) {
                    err << prefix << "error: could not write " << outPath.generic_string() << ": " << written.message << "\n";
                    return kExitFailed;
                }

                const auto mismatch = std::mismatch(input.begin(), input.end(), output.begin(), output.end());
                const bool identical = mismatch.first == input.end() && mismatch.second == output.end();
                if (identical) {
                    out << "wrote " << outPath.generic_string() << " (" << output.size() << " bytes, identical to the input)\n";
                    return kExitOk;
                }
                const size_t at = static_cast<size_t>(mismatch.first - input.begin());
                std::string where = "the outputs differ at byte " + std::to_string(at) + ", " + locateByte(input, at);
                if (input.size() != output.size()) {
                    where += "; the output is " + std::to_string(output.size()) + " bytes, the input " + std::to_string(input.size());
                }
                if (expectIdentical) {
                    err << prefix << "wrote " << outPath.generic_string() << ", which differs from the input: " << where << "\n";
                    return kExitDiffer;
                }
                out << "wrote " << outPath.generic_string() << " (" << output.size() << " bytes; " << where << ")\n";
                return kExitOk;
            }

        }

    }

    void printAsepriteUsage(const std::string& commandName, std::ostream& out) {
        out << "Usage:\n"
            << "  " << commandName << " aseprite info <file> [--json] [--cels]       Summary, or the JSON description.\n"
            << "  " << commandName << " aseprite render <file> <out.png> [options]  A frame or a tag's frames as a PNG.\n"
            << "  " << commandName << " aseprite resave <file> <out> [--expect-identical]  Read and write back.\n\n"
            << "No project needed. Run a subcommand with --help for its options.\n"
            << "Exit code: 0 ok, 1 the output could not be written, 2 usage error, 3 the file could not be\n"
            << "read, 4 resave --expect-identical wrote different bytes.\n";
    }

    int runAsepriteCommand(int argc, char** argv, const std::string& commandName, std::ostream& out, std::ostream& err) {
        using namespace clidetail;
        const std::string sub = argc >= 2 ? argv[1] : "";
        if (sub.empty()) return usageError(commandName, nullptr, "needs a subcommand: info, render or resave", printAsepriteUsage, err);
        if (isHelp(sub) || sub == "help") {
            printAsepriteUsage(commandName, out);
            return kExitOk;
        }
        if (sub == "info") return runInfo(argc, argv, commandName, out, err);
        if (sub == "render") return runRender(argc, argv, commandName, out, err);
        if (sub == "resave") return runResave(argc, argv, commandName, out, err);
        return usageError(commandName, nullptr, "unknown subcommand '" + sub + "' (info, render or resave)", printAsepriteUsage, err);
    }

}
