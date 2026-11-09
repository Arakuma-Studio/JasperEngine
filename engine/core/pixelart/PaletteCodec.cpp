// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Palette.h"

#include "json.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>

using namespace jasperengine;
using namespace jasperengine::pixelart;

// file-local, inside the namespace: a unity build's other sources never see these names
namespace jasperengine::pixelart {
namespace {

    using OJson = nlohmann::ordered_json;

    const char* const kKnownKeys[] = {"format", "version", "name", "author", "description", "source", "columns", "colors",
                                      "names", "locked", "transparent", "ramps", "variants"};

    bool isKnownKey(const std::string& key) {
        for (const char* known : kKnownKeys) {
            if (key == known) return true;
        }
        return false;
    }

    // nlohmann's DOM builder recording where and why the parse stopped, without exceptions (the
    // Web build has no exception catching)
    class SaxParser : public nlohmann::detail::json_sax_dom_parser<OJson> {
    public:
        explicit SaxParser(OJson& root): json_sax_dom_parser<OJson>(root, false) {}

        template<class Exception>
        bool parse_error(std::size_t position, const std::string& lastToken, const Exception& ex) {
            failed = true;
            errorPosition = position;
            message = ex.what();
            return json_sax_dom_parser<OJson>::parse_error(position, lastToken, ex);
        }

        bool failed = false;
        std::size_t errorPosition = 0;
        std::string message;
    };

    std::string compact(const OJson& value) {
        return value.dump(-1, ' ', false, OJson::error_handler_t::replace);
    }

    std::string quote(const std::string& text) {
        return compact(OJson(text));
    }

    // the shortest text that reads back as the same float
    std::string number(float value) {
        if (value == std::floor(value) && std::fabs(value) < 1e9f) {
            char text[32];
            std::snprintf(text, sizeof(text), "%.0f", (double)value);
            return text;
        }
        char text[48];
        for (int precision = 1; precision <= 9; precision++) {
            std::snprintf(text, sizeof(text), "%.*g", precision, (double)value);
            if (std::strtof(text, nullptr) == value) break;
        }
        return text;
    }

    struct Reader {
        Palette& out;
        std::vector<Diagnostic>& diagnostics;

        void error(const std::string& kind, const std::string& field, const std::string& message, const std::string& fix) {
            diagnostics.push_back({Diagnostic::Severity::Error, kind, field, message, fix});
        }

        void warning(const std::string& kind, const std::string& field, const std::string& message, const std::string& fix) {
            diagnostics.push_back({Diagnostic::Severity::Warning, kind, field, message, fix});
        }

        bool wholeNumber(const OJson& value, double& number) {
            if (!value.is_number()) return false;
            number = value.get<double>();
            return std::isfinite(number) && number == std::floor(number);
        }

        void readString(const OJson& root, const char* key, std::string& target) {
            auto it = root.find(key);
            if (it == root.end() || it->is_null()) return;
            if (!it->is_string()) {
                error("invalid_value", key, std::string("\"") + key + "\" must be a string", "write it as a JSON string");
                return;
            }
            target = it->get<std::string>();
        }

        void readSource(const OJson& value) {
            if (value.is_null()) return;
            if (!value.is_object()) {
                error("invalid_value", "source", "\"source\" must be an object {kind, id, url, ...}", "write it as a JSON object, or leave it out");
                return;
            }
            PaletteSource& source = out.source;
            for (auto it = value.begin(); it != value.end(); ++it) {
                const std::string& key = it.key();
                std::string* member = nullptr;
                if (key == "kind") member = &source.kind;
                else if (key == "id") member = &source.id;
                else if (key == "url") member = &source.url;
                else if (key == "fetched") member = &source.fetched;
                else if (key == "file") member = &source.file;
                if (member && it->is_string()) {
                    *member = it->get<std::string>();
                } else {
                    source.extra.push_back({key, compact(*it)});
                }
            }
        }

        void readColors(const OJson& value) {
            if (!value.is_array()) {
                error("invalid_value", "colors", "\"colors\" must be a list of \"#rrggbb\" or \"#rrggbbaa\"", "write the colours as a JSON list of hex strings");
                return;
            }
            for (size_t i = 0; i < value.size(); i++) {
                const std::string field = "colors[" + std::to_string(i) + "]";
                Rgba8 color;
                if (!value[i].is_string() || !parseHex(value[i].get<std::string>(), color)) {
                    error("invalid_value", field, field + " is not a colour: " + compact(value[i]), "write \"#rrggbb\" or \"#rrggbbaa\"");
                    color = Rgba8{0, 0, 0, 0};
                }
                out.colors.push_back(color);
            }
        }

        void readNames(const OJson& value) {
            if (value.is_null()) return;
            if (!value.is_object()) {
                error("invalid_value", "names", "\"names\" must be an object of index: name", "write {\"3\": \"cape mid\"}");
                return;
            }
            for (auto it = value.begin(); it != value.end(); ++it) {
                const std::string field = "names." + it.key();
                char* end = nullptr;
                const long index = std::strtol(it.key().c_str(), &end, 10);
                if (it.key().empty() || *end != '\0' || index < 0 || index > 100000) {
                    error("invalid_value", field, "colour names are keyed by index: \"" + it.key() + "\" is not one", "use the colour's index as the key, \"3\"");
                    continue;
                }
                if (!it->is_string()) {
                    error("invalid_value", field, field + " must be a string", "write the name as a JSON string");
                    continue;
                }
                out.names[(int)index] = it->get<std::string>();
            }
        }

        bool readIndexList(const OJson& value, const std::string& field, std::vector<uint16_t>& target) {
            if (!value.is_array()) {
                error("invalid_value", field, field + " must be a list of colour indices", "write a JSON list of whole numbers");
                return false;
            }
            for (size_t i = 0; i < value.size(); i++) {
                double number = 0.0;
                if (!wholeNumber(value[i], number) || number < 0.0 || number > 65535.0) {
                    error("invalid_value", field + "[" + std::to_string(i) + "]", "not a colour index: " + compact(value[i]),
                          "use a whole number from 0");
                    continue;
                }
                target.push_back((uint16_t)number);
            }
            return true;
        }

        void readRamps(const OJson& value) {
            if (value.is_null()) return;
            if (!value.is_array()) {
                error("invalid_value", "ramps", "\"ramps\" must be a list of {name, colors, cycle}", "write a JSON list of ramp objects");
                return;
            }
            for (size_t i = 0; i < value.size(); i++) {
                const std::string field = "ramps[" + std::to_string(i) + "]";
                const OJson& item = value[i];
                PaletteRamp ramp;
                if (!item.is_object()) {
                    error("invalid_value", field, field + " must be an object {name, colors, cycle}", "write the ramp as a JSON object");
                    continue;
                }
                auto name = item.find("name");
                if (name != item.end() && name->is_string()) {
                    ramp.name = name->get<std::string>();
                } else if (name != item.end()) {
                    error("invalid_value", field + ".name", "a ramp's name must be a string", "write the name as a JSON string");
                }
                auto colors = item.find("colors");
                if (colors != item.end()) readIndexList(*colors, field + ".colors", ramp.colors);
                auto cycle = item.find("cycle");
                if (cycle != item.end() && !cycle->is_null()) {
                    if (!cycle->is_number() || !std::isfinite(cycle->get<double>())) {
                        error("invalid_value", field + ".cycle", "\"cycle\" must be a number of steps per second", "write a number (0: no cycling)");
                    } else {
                        ramp.cycle = (float)cycle->get<double>();
                    }
                }
                out.ramps.push_back(std::move(ramp));
            }
        }

        void readVariants(const OJson& value) {
            if (value.is_null()) return;
            if (!value.is_array()) {
                error("invalid_value", "variants", "\"variants\" must be a list of {name, colors}", "write a JSON list of variant objects");
                return;
            }
            for (size_t i = 0; i < value.size(); i++) {
                const std::string field = "variants[" + std::to_string(i) + "]";
                const OJson& item = value[i];
                PaletteVariant variant;
                if (!item.is_object()) {
                    error("invalid_value", field, field + " must be an object {name, colors}", "write the variant as a JSON object");
                    continue;
                }
                auto name = item.find("name");
                if (name != item.end() && name->is_string()) {
                    variant.name = name->get<std::string>();
                } else if (name != item.end()) {
                    error("invalid_value", field + ".name", "a variant's name must be a string", "write the name as a JSON string");
                }
                auto colors = item.find("colors");
                if (colors != item.end()) {
                    if (!colors->is_array()) {
                        error("invalid_value", field + ".colors", "a variant's colours must be a list of hex colours and nulls",
                              "write [null, \"#29366f\", ...]");
                    } else {
                        for (size_t c = 0; c < colors->size(); c++) {
                            const OJson& entry = (*colors)[c];
                            if (entry.is_null()) {
                                variant.colors.push_back(std::nullopt);
                                continue;
                            }
                            Rgba8 color;
                            if (!entry.is_string() || !parseHex(entry.get<std::string>(), color)) {
                                error("invalid_value", field + ".colors[" + std::to_string(c) + "]", "not a colour: " + compact(entry),
                                      "write \"#rrggbb\", \"#rrggbbaa\" or null (the base colour)");
                                variant.colors.push_back(std::nullopt);
                                continue;
                            }
                            variant.colors.push_back(color);
                        }
                    }
                }
                out.variants.push_back(std::move(variant));
            }
        }
    };

    std::string hexList(const std::vector<Rgba8>& colors) {
        std::string text;
        for (size_t i = 0; i < colors.size(); i++) {
            if (i) text += ", ";
            text += quote(toHex(colors[i]));
        }
        return text;
    }

    std::string indexList(const std::vector<uint16_t>& indices) {
        std::string text = "[";
        for (size_t i = 0; i < indices.size(); i++) {
            if (i) text += ", ";
            text += std::to_string(indices[i]);
        }
        return text + "]";
    }

}
}

bool PaletteCodec::read(std::string_view json, Palette& out, std::vector<Diagnostic>& diagnostics) {
    out = Palette();
    Reader reader{out, diagnostics};
    const size_t firstDiagnostic = diagnostics.size();

    OJson root;
    SaxParser sax(root);
    const std::string text(json);
    OJson::sax_parse(text, &sax);
    if (sax.failed) {
        int line = 1, column = 1;
        const size_t offset = sax.errorPosition > 0 ? sax.errorPosition - 1 : 0;
        for (size_t i = 0; i < offset && i < text.size(); i++) {
            if (text[i] == '\n') {
                line++;
                column = 1;
            } else {
                column++;
            }
        }
        const size_t at = sax.message.find("syntax error");
        reader.error("parse_error", "", "not valid JSON at line " + std::to_string(line) + ", column " + std::to_string(column) + ": " +
                     (at != std::string::npos ? sax.message.substr(at) : sax.message), "fix the JSON syntax at that position");
        return false;
    }
    if (!root.is_object()) {
        reader.error("parse_error", "", "not a palette: the top level must be a JSON object", "write a .palette object {format, version, colors, ...}");
        return false;
    }
    auto format = root.find("format");
    if (format == root.end() || !format->is_string() || format->get<std::string>() != Palette::kFormat) {
        reader.error("parse_error", "format", std::string("not a palette: \"format\" must be \"") + Palette::kFormat + "\"",
                     std::string("set \"format\": \"") + Palette::kFormat + "\"");
        return false;
    }
    auto version = root.find("version");
    double versionNumber = 0.0;
    if (version == root.end() || !reader.wholeNumber(*version, versionNumber) || versionNumber < 1.0) {
        reader.error("parse_error", "version", "\"version\" must be a whole number from 1", "set \"version\": 1");
        return false;
    }
    if (versionNumber > Palette::kVersion) {
        out.readOnly = true;
        reader.warning("newer_version", "version",
                       "this palette is version " + std::to_string((long long)versionNumber) + "; this editor reads version " +
                       std::to_string(Palette::kVersion) + ", so it is read-only",
                       "edit it with the newer editor that wrote it, or save a copy");
    }

    reader.readString(root, "name", out.name);
    reader.readString(root, "author", out.author);
    reader.readString(root, "description", out.description);
    if (auto it = root.find("source"); it != root.end()) reader.readSource(*it);
    if (auto it = root.find("columns"); it != root.end() && !it->is_null()) {
        double columns = 0.0;
        if (!reader.wholeNumber(*it, columns)) {
            reader.error("invalid_value", "columns", "\"columns\" must be a whole number from 1 to 64", "set \"columns\": 8");
        } else if (columns < 1.0 || columns > Palette::kMaxColumns) {
            out.columns = (int)std::clamp(columns, 1.0, (double)Palette::kMaxColumns);
            reader.warning("invalid_value", "columns", "\"columns\" is " + std::to_string((long long)columns) + "; it shows as " +
                           std::to_string(out.columns), "use a whole number from 1 to 64");
        } else {
            out.columns = (int)columns;
        }
    }
    if (auto it = root.find("colors"); it != root.end()) {
        reader.readColors(*it);
    } else {
        reader.error("invalid_value", "colors", "a palette needs \"colors\": 1 to 256 hex colours", "add \"colors\": [\"#000000\", ...]");
    }
    if (auto it = root.find("names"); it != root.end()) reader.readNames(*it);
    if (auto it = root.find("locked"); it != root.end() && !it->is_null()) reader.readIndexList(*it, "locked", out.locked);
    if (auto it = root.find("transparent"); it != root.end() && !it->is_null()) {
        double transparent = 0.0;
        if (!reader.wholeNumber(*it, transparent) || transparent < -1.0 || transparent > 65535.0) {
            reader.error("invalid_value", "transparent", "\"transparent\" must be a colour index, or -1 for none", "set a whole number from -1");
        } else {
            out.transparent = (int)transparent;
        }
    }
    if (auto it = root.find("ramps"); it != root.end()) reader.readRamps(*it);
    if (auto it = root.find("variants"); it != root.end()) reader.readVariants(*it);

    std::string unknownNames;
    for (auto it = root.begin(); it != root.end(); ++it) {
        if (isKnownKey(it.key())) continue;
        out.unknown.push_back({it.key(), compact(*it)});
        unknownNames += (unknownNames.empty() ? "" : ", ") + it.key();
    }
    if (!unknownNames.empty()) {
        reader.warning("unknown_keys", "", "unknown keys kept as they are (written back on save): " + unknownNames,
                       "nothing to do; they are kept for the tool that wrote them");
    }

    // the semantic checks run on what was read (a missing "colors" is reported already)
    if (root.contains("colors")) validate(out, diagnostics);
    return !hasErrors(std::vector<Diagnostic>(diagnostics.begin() + (std::ptrdiff_t)firstDiagnostic, diagnostics.end()));
}

bool PaletteCodec::validate(const Palette& palette, std::vector<Diagnostic>& diagnostics) {
    bool ok = true;
    auto error = [&](const std::string& kind, const std::string& field, const std::string& message, const std::string& fix) {
        diagnostics.push_back({Diagnostic::Severity::Error, kind, field, message, fix});
        ok = false;
    };
    auto warning = [&](const std::string& kind, const std::string& field, const std::string& message, const std::string& fix) {
        diagnostics.push_back({Diagnostic::Severity::Warning, kind, field, message, fix});
    };
    const int count = palette.count();
    auto range = [&]() { return "0 to " + std::to_string(std::max(0, count - 1)); };

    if (count == 0) {
        error("invalid_value", "colors", "a palette needs at least one colour", "add a colour");
    } else if (count > Palette::kMaxColors) {
        error("too_many_colors", "colors", "the palette has " + std::to_string(count) + " colours; a palette holds at most 256",
              "Reduce Colours… or Import the first 256");
    }
    if (palette.columns < 1 || palette.columns > Palette::kMaxColumns) {
        error("invalid_value", "columns", "\"columns\" must be from 1 to 64", "set \"columns\": 8");
    }

    // exact duplicates make Palette Swap ambiguous: the first index wins
    for (int i = 0; i < count && count <= Palette::kMaxColors; i++) {
        for (int j = 0; j < i; j++) {
            if (palette.colors[i] == palette.colors[j]) {
                warning("duplicate_color", "colors[" + std::to_string(i) + "]",
                        "colour " + std::to_string(i) + " (" + toHex(palette.colors[i]) + ") repeats colour " + std::to_string(j) +
                        "; Palette Swap maps it to " + std::to_string(j),
                        "change one of them, or remove the duplicate");
                break;
            }
        }
    }
    for (const auto& [index, text] : palette.names) {
        if (index >= count) {
            warning("index_out_of_range", "names." + std::to_string(index), "a name for colour " + std::to_string(index) +
                    ", which the palette does not have (" + range() + ")", "remove the name or add the colour");
        }
    }
    for (size_t i = 0; i < palette.locked.size(); i++) {
        if (palette.locked[i] >= count) {
            error("index_out_of_range", "locked[" + std::to_string(i) + "]", "locked colour " + std::to_string(palette.locked[i]) +
                  " is not in the palette (" + range() + ")", "lock an index from " + range());
        }
    }
    if (palette.transparent < -1 || palette.transparent >= count) {
        error("index_out_of_range", "transparent", "the transparent index " + std::to_string(palette.transparent) +
              " is not in the palette (" + range() + ")", "use an index from " + range() + ", or -1 for none");
    }

    if ((int)palette.ramps.size() > Palette::kMaxRamps) {
        error("invalid_value", "ramps", "the palette has " + std::to_string(palette.ramps.size()) + " ramps; at most 64",
              "merge or remove ramps");
    }
    std::set<std::string> rampNames;
    int cycling = 0;
    std::vector<int> cycleOwner(std::max(0, count), -1);
    for (size_t r = 0; r < palette.ramps.size(); r++) {
        const PaletteRamp& ramp = palette.ramps[r];
        const std::string field = "ramps[" + std::to_string(r) + "]";
        if (ramp.name.empty()) {
            error("invalid_value", field + ".name", "ramp " + std::to_string(r) + " has no name", "give the ramp a name");
        } else if (!rampNames.insert(ramp.name).second) {
            error("duplicate_name", field + ".name", "two ramps are called '" + ramp.name + "'", "rename one of them");
        }
        if (ramp.colors.empty()) {
            error("invalid_value", field + ".colors", "ramp '" + ramp.name + "' has no colours", "add colour indices to the ramp, or remove it");
        }
        for (size_t c = 0; c < ramp.colors.size(); c++) {
            if (ramp.colors[c] >= count) {
                error("index_out_of_range", field + ".colors[" + std::to_string(c) + "]", "ramp '" + ramp.name + "' uses colour " +
                      std::to_string(ramp.colors[c]) + ", which the palette does not have (" + range() + ")", "use an index from " + range());
            }
        }
        if (!std::isfinite(ramp.cycle)) {
            error("invalid_value", field + ".cycle", "the cycle speed must be a number", "set a number of steps per second (0: none)");
        } else if (ramp.cycle != 0.0f) {
            cycling++;
            // colour cycling moves each index along its ramp: an index in two cycling ramps (or
            // twice in one) would have two places to be
            for (size_t c = 0; c < ramp.colors.size(); c++) {
                const int index = ramp.colors[c];
                if (index >= count) continue;
                if (cycleOwner[index] >= 0) {
                    const PaletteRamp& other = palette.ramps[cycleOwner[index]];
                    error("cycle_overlap", field + ".colors[" + std::to_string(c) + "]",
                          "colour " + std::to_string(index) + " is in cycling ramp '" + other.name + "'" +
                          (cycleOwner[index] == (int)r ? " twice" : " and in cycling ramp '" + ramp.name + "'"),
                          "cycling ramps cannot share colours: give each its own colours, or stop one cycling (cycle 0)");
                } else {
                    cycleOwner[index] = (int)r;
                }
            }
        }
    }
    if (cycling > Palette::kMaxCyclingRamps) {
        error("invalid_value", "ramps", std::to_string(cycling) + " ramps cycle; Palette Cycling runs at most 8",
              "stop cycling the others (cycle 0)");
    }

    if ((int)palette.variants.size() > Palette::kMaxVariants) {
        error("invalid_value", "variants", "the palette has " + std::to_string(palette.variants.size()) + " variants; at most 15",
              "remove variants");
    }
    std::set<std::string> variantNames;
    for (size_t v = 0; v < palette.variants.size(); v++) {
        const PaletteVariant& variant = palette.variants[v];
        const std::string field = "variants[" + std::to_string(v) + "]";
        if (variant.name.empty()) {
            error("invalid_value", field + ".name", "variant " + std::to_string(v) + " has no name", "give the variant a name");
        } else if (!variantNames.insert(variant.name).second) {
            error("duplicate_name", field + ".name", "two variants are called '" + variant.name + "'", "rename one of them");
        }
        if ((int)variant.colors.size() > count) {
            error("variant_too_long", field + ".colors", "variant '" + variant.name + "' has " + std::to_string(variant.colors.size()) +
                  " colours; the palette has " + std::to_string(count), "remove the extra colours (a variant is aligned with the palette's colours)");
        }
    }
    return ok;
}

std::string PaletteCodec::write(const Palette& palette) {
    std::vector<std::string> members;
    members.push_back(std::string("\"format\": ") + quote(Palette::kFormat));
    members.push_back("\"version\": " + std::to_string(Palette::kVersion));
    members.push_back("\"name\": " + quote(palette.name));
    members.push_back("\"author\": " + quote(palette.author));
    members.push_back("\"description\": " + quote(palette.description));
    if (!palette.source.empty()) {
        const PaletteSource& source = palette.source;
        std::vector<std::string> parts;
        if (!source.kind.empty()) parts.push_back("\"kind\": " + quote(source.kind));
        if (!source.id.empty()) parts.push_back("\"id\": " + quote(source.id));
        if (!source.url.empty()) parts.push_back("\"url\": " + quote(source.url));
        if (!source.fetched.empty()) parts.push_back("\"fetched\": " + quote(source.fetched));
        if (!source.file.empty()) parts.push_back("\"file\": " + quote(source.file));
        for (const auto& [key, value] : source.extra) parts.push_back(quote(key) + ": " + value);
        std::string text = "\"source\": {";
        for (size_t i = 0; i < parts.size(); i++) text += (i ? ", " : "") + parts[i];
        members.push_back(text + "}");
    }
    members.push_back("\"columns\": " + std::to_string(palette.columns));

    // `columns` colours to a line once they do not fit on one
    const int columns = std::max(1, palette.columns);
    if ((int)palette.colors.size() <= columns) {
        members.push_back("\"colors\": [" + hexList(palette.colors) + "]");
    } else {
        std::string text = "\"colors\": [\n";
        for (size_t start = 0; start < palette.colors.size(); start += (size_t)columns) {
            const size_t end = std::min(palette.colors.size(), start + (size_t)columns);
            text += "    " + hexList(std::vector<Rgba8>(palette.colors.begin() + (std::ptrdiff_t)start, palette.colors.begin() + (std::ptrdiff_t)end));
            text += end < palette.colors.size() ? ",\n" : "\n";
        }
        members.push_back(text + "  ]");
    }

    if (!palette.names.empty()) {
        std::string text = "\"names\": {";
        bool first = true;
        for (const auto& [index, name] : palette.names) {
            text += (first ? "" : ", ") + quote(std::to_string(index)) + ": " + quote(name);
            first = false;
        }
        members.push_back(text + "}");
    }
    if (!palette.locked.empty()) members.push_back("\"locked\": " + indexList(palette.locked));
    if (palette.transparent >= 0) members.push_back("\"transparent\": " + std::to_string(palette.transparent));
    if (!palette.ramps.empty()) {
        std::string text = "\"ramps\": [\n";
        for (size_t i = 0; i < palette.ramps.size(); i++) {
            const PaletteRamp& ramp = palette.ramps[i];
            text += "    {\"name\": " + quote(ramp.name) + ", \"colors\": " + indexList(ramp.colors);
            if (ramp.cycle != 0.0f) text += ", \"cycle\": " + number(ramp.cycle);
            text += i + 1 < palette.ramps.size() ? "},\n" : "}\n";
        }
        members.push_back(text + "  ]");
    }
    if (!palette.variants.empty()) {
        std::string text = "\"variants\": [\n";
        for (size_t i = 0; i < palette.variants.size(); i++) {
            const PaletteVariant& variant = palette.variants[i];
            size_t length = variant.colors.size();
            while (length > 0 && !variant.colors[length - 1].has_value()) length--;
            text += "    {\"name\": " + quote(variant.name) + ", \"colors\": [";
            for (size_t c = 0; c < length; c++) {
                if (c) text += ", ";
                text += variant.colors[c].has_value() ? quote(toHex(*variant.colors[c])) : std::string("null");
            }
            text += i + 1 < palette.variants.size() ? "]},\n" : "]}\n";
        }
        members.push_back(text + "  ]");
    }
    for (const auto& [key, value] : palette.unknown) {
        if (isKnownKey(key)) continue;
        members.push_back(quote(key) + ": " + (value.empty() ? std::string("null") : value));
    }

    std::string text = "{\n";
    for (size_t i = 0; i < members.size(); i++) {
        text += "  " + members[i] + (i + 1 < members.size() ? ",\n" : "\n");
    }
    return text + "}\n";
}
