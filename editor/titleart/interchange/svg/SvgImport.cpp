// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SvgImport.h"

#include "titleart/interchange/TitleImport.h"
#include "titleart/TitleArtIO.h"
#include "util/FileUtils.h"

#include "tinyxml2.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <regex>
#include <set>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::editor;
using titleart::Json;
using tinyxml2::XMLDocument;
using tinyxml2::XMLElement;
using tinyxml2::XMLNode;
namespace fs = std::filesystem;

namespace {

    std::string lower(std::string s) {
        for (char& c : s) c = (char)std::tolower((unsigned char)c);
        return s;
    }

    std::string trim(const std::string& s) {
        size_t a = 0, b = s.size();
        while (a < b && std::isspace((unsigned char)s[a])) a++;
        while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
        return s.substr(a, b - a);
    }

    // the local name of an element ("svg:path" -> "path")
    std::string tagOf(const XMLElement* e) {
        std::string name = e->Name() ? e->Name() : "";
        const size_t colon = name.find(':');
        return colon == std::string::npos ? name : name.substr(colon + 1);
    }

    std::string attr(const XMLElement* e, const char* name) {
        const char* v = e->Attribute(name);
        return v ? v : "";
    }

    // a property of the element's own `style`, else its presentation attribute
    std::string property(const XMLElement* e, const std::string& name) {
        const std::string style = attr(e, "style");
        size_t start = 0;
        while (start < style.size()) {
            size_t end = style.find(';', start);
            if (end == std::string::npos) end = style.size();
            const std::string decl = style.substr(start, end - start);
            const size_t colon = decl.find(':');
            if (colon != std::string::npos && lower(trim(decl.substr(0, colon))) == name) return trim(decl.substr(colon + 1));
            start = end + 1;
        }
        return trim(attr(e, name.c_str()));
    }

    bool isDrawable(const std::string& tag) {
        static const std::set<std::string> tags = {"path", "rect", "circle", "ellipse", "line", "polyline", "polygon", "use", "text", "image"};
        return tags.count(tag) > 0;
    }

    bool isContainer(const std::string& tag) {
        return tag == "g" || tag == "a" || tag == "switch";
    }

    // what every part keeps from the root: definitions that draw nothing where they stand
    bool isDefinition(const std::string& tag) {
        static const std::set<std::string> tags = {"defs", "style", "linearGradient", "radialGradient", "pattern", "clipPath", "mask",
                                                   "filter", "symbol", "marker", "font", "font-face"};
        return tags.count(tag) > 0;
    }

    bool hidden(const XMLElement* e) {
        return lower(property(e, "display")) == "none";
    }

    double number(const std::string& text) {
        return std::strtod(text.c_str(), nullptr);
    }

    void viewportOf(const XMLElement* root, double& width, double& height) {
        const std::string viewBox = attr(root, "viewBox");
        if (!viewBox.empty()) {
            std::string spaced = viewBox;
            std::replace(spaced.begin(), spaced.end(), ',', ' ');
            std::istringstream in(spaced);
            double x = 0, y = 0, w = 0, h = 0;
            if (in >> x >> y >> w >> h && w > 0 && h > 0) {
                width = w;
                height = h;
                return;
            }
        }
        width = number(attr(root, "width"));
        height = number(attr(root, "height"));
    }

    // ---- references ----

    void collectRefs(const XMLElement* e, std::set<std::string>& out) {
        static const std::regex url(R"(url\(\s*['"]?#([^'")\s]+)['"]?\s*\))");
        for (const tinyxml2::XMLAttribute* a = e->FirstAttribute(); a; a = a->Next()) {
            const std::string name = a->Name();
            const std::string value = a->Value();
            if ((name == "href" || name == "xlink:href") && value.size() > 1 && value[0] == '#') out.insert(value.substr(1));
            for (std::sregex_iterator it(value.begin(), value.end(), url), end; it != end; ++it) out.insert((*it)[1].str());
        }
    }

    void collectRefsDeep(const XMLElement* e, std::set<std::string>& out) {
        collectRefs(e, out);
        for (const XMLElement* c = e->FirstChildElement(); c; c = c->NextSiblingElement()) collectRefsDeep(c, out);
    }

    void indexIds(XMLElement* e, std::map<std::string, XMLElement*>& out) {
        const std::string id = attr(e, "id");
        if (!id.empty()) out.emplace(id, e);
        for (XMLElement* c = e->FirstChildElement(); c; c = c->NextSiblingElement()) indexIds(c, out);
    }

    bool inside(const XMLElement* e, const XMLElement* ancestor) {
        for (const XMLNode* n = e; n; n = n->Parent()) {
            if (n == ancestor) return true;
        }
        return false;
    }

    // ---- colours ----

    std::string normalColor(const std::string& raw, const std::string& current) {
        std::string c = lower(trim(raw));
        if (c == "currentcolor") c = lower(trim(current.empty() ? "black" : current));
        if (c.size() == 4 && c[0] == '#') c = std::string("#") + c[1] + c[1] + c[2] + c[2] + c[3] + c[3];
        std::string compact;
        for (char ch : c) if (!std::isspace((unsigned char)ch)) compact += ch;
        if (compact == "black" || compact == "#000000" || compact == "rgb(0,0,0)") return "black";
        if (compact == "none" || compact == "transparent") return "none";
        return compact;
    }

    struct Paint {
        std::string fill = "black", stroke = "none", color;
    };

    void collectColors(const XMLElement* e, Paint inherited, const std::map<std::string, XMLElement*>& ids, std::set<std::string>& out,
                       int depth) {
        if (depth > 64 || hidden(e)) return;
        const std::string tag = tagOf(e);
        if (isDefinition(tag) && tag != "symbol") return;
        Paint p = inherited;
        if (!property(e, "color").empty()) p.color = property(e, "color");
        if (!property(e, "fill").empty()) p.fill = property(e, "fill");
        if (!property(e, "stroke").empty()) p.stroke = property(e, "stroke");
        if (isDrawable(tag) && tag != "use" && tag != "image") {
            out.insert(normalColor(p.fill, p.color));
            out.insert(normalColor(p.stroke, p.color));
        }
        if (tag == "use") {
            std::string href = attr(e, "href");
            if (href.empty()) href = attr(e, "xlink:href");
            if (href.size() > 1 && href[0] == '#') {
                auto it = ids.find(href.substr(1));
                if (it != ids.end()) collectColors(it->second, p, ids, out, depth + 1);
            }
        }
        for (const XMLElement* c = e->FirstChildElement(); c; c = c->NextSiblingElement()) collectColors(c, p, ids, out, depth + 1);
    }

    // the colours the <style> rules set (they may apply anywhere)
    void styleColors(const XMLElement* e, std::set<std::string>& out) {
        if (tagOf(e) == "style" && e->GetText()) {
            static const std::regex decl(R"((fill|stroke)\s*:\s*([^;}]+))", std::regex::icase);
            const std::string text = e->GetText();
            for (std::sregex_iterator it(text.begin(), text.end(), decl), end; it != end; ++it) out.insert(normalColor((*it)[2].str(), ""));
        }
        for (const XMLElement* c = e->FirstChildElement(); c; c = c->NextSiblingElement()) styleColors(c, out);
    }

    std::string modeOf(const XMLElement* root, const std::map<std::string, XMLElement*>& ids) {
        std::set<std::string> colors;
        Paint start;
        collectColors(root, start, ids, colors, 0);
        styleColors(root, colors);
        std::set<std::string> distinct;
        for (const std::string& c : colors) {
            if (c.rfind("url(", 0) == 0) return "artwork";
            if (c != "none" && c != "black") distinct.insert(c);
        }
        return distinct.size() <= 1 ? "shape" : "artwork";
    }

    // what shape mode cannot draw, once each, in a fixed order
    std::vector<std::string> shapeNotes(const XMLElement* root) {
        std::set<std::string> found;
        std::function<void(const XMLElement*)> walk = [&](const XMLElement* e) {
            const std::string tag = tagOf(e);
            if (tag == "text" || tag == "tspan" || tag == "textPath") found.insert("text");
            if (tag == "image") found.insert("image");
            if (tag == "clipPath" || !attr(e, "clip-path").empty()) found.insert("clipPath");
            if (tag == "mask" || !attr(e, "mask").empty()) found.insert("mask");
            if (tag == "filter" || !attr(e, "filter").empty()) found.insert("filter");
            for (const XMLElement* c = e->FirstChildElement(); c; c = c->NextSiblingElement()) walk(c);
        };
        walk(root);
        std::vector<std::string> out;
        if (found.count("text")) out.push_back("<text> is not drawn in shape mode: convert it to outlines in the authoring tool");
        if (found.count("image")) out.push_back("<image> is not drawn in shape mode");
        if (found.count("clipPath")) out.push_back("clipPath is ignored in shape mode");
        if (found.count("mask")) out.push_back("mask is ignored in shape mode");
        if (found.count("filter")) out.push_back("filter is ignored in shape mode");
        return out;
    }

    std::string print(XMLDocument& doc) {
        tinyxml2::XMLPrinter printer;
        doc.Print(&printer);
        return printer.CStr();
    }

    std::string capitalized(std::string s) {
        if (!s.empty()) s[0] = (char)std::toupper((unsigned char)s[0]);
        return s;
    }

    struct Source {
        XMLElement* element;
        std::vector<XMLElement*> chain;     // the ancestor groups below the root, outermost first
        std::string name;
    };

}

std::string editor::svgAutoMode(const std::string& text) {
    XMLDocument doc;
    if (doc.Parse(text.c_str(), text.size()) != tinyxml2::XML_SUCCESS || !doc.RootElement()) return "artwork";
    std::map<std::string, XMLElement*> ids;
    indexIds(doc.RootElement(), ids);
    return modeOf(doc.RootElement(), ids);
}

SvgSplit editor::splitSvg(const std::string& text, const std::string& split, const std::string& mode) {
    SvgSplit out;
    XMLDocument doc;
    if (doc.Parse(text.c_str(), text.size()) != tinyxml2::XML_SUCCESS) {
        out.error = std::string("not XML: ") + (doc.ErrorStr() ? doc.ErrorStr() : "parse error");
        return out;
    }
    XMLElement* root = doc.RootElement();
    if (!root || tagOf(root) != "svg") {
        out.error = "no <svg> root element";
        return out;
    }
    viewportOf(root, out.width, out.height);
    std::map<std::string, XMLElement*> ids;
    indexIds(root, ids);
    const auto partMode = [&](const XMLElement* scope) { return mode == "auto" ? modeOf(scope, ids) : mode; };
    if (split == "none") {
        SvgPart part;
        part.text = text;
        part.mode = partMode(root);
        if (part.mode == "shape") part.notes = shapeNotes(root);
        out.parts.push_back(std::move(part));
        out.ok = true;
        return out;
    }

    // the parts' sources, in paint order
    std::vector<Source> sources;
    std::map<std::string, int> counters;
    const auto nameFor = [&](const XMLElement* e) {
        const std::string id = attr(e, "id");
        const std::string tag = tagOf(e);
        const std::string label = isContainer(tag) ? "Group" : capitalized(tag);
        const int n = ++counters[label];
        return id.empty() ? label + " " + std::to_string(n) : id;
    };
    std::function<void(XMLElement*, std::vector<XMLElement*>&, int)> walk = [&](XMLElement* parent, std::vector<XMLElement*>& chain, int depth) {
        for (XMLElement* e = parent->FirstChildElement(); e; e = e->NextSiblingElement()) {
            const std::string tag = tagOf(e);
            if (hidden(e) || isDefinition(tag)) continue;
            if (isContainer(tag)) {
                if (split == "groups" && depth == 0) {
                    sources.push_back({e, chain, nameFor(e)});
                } else if (depth < 64) {
                    chain.push_back(e);
                    walk(e, chain, depth + 1);
                    chain.pop_back();
                }
            } else if (isDrawable(tag)) {
                sources.push_back({e, chain, nameFor(e)});
            }
        }
    };
    std::vector<XMLElement*> chain;
    walk(root, chain, 0);
    if (sources.empty()) {
        out.error = "the SVG draws nothing (no visible group or drawable element)";
        return out;
    }

    for (const Source& s : sources) {
        XMLDocument part;
        part.InsertEndChild(part.NewDeclaration());
        XMLElement* proot = part.NewElement(root->Name());
        for (const tinyxml2::XMLAttribute* a = root->FirstAttribute(); a; a = a->Next()) proot->SetAttribute(a->Name(), a->Value());
        part.InsertEndChild(proot);
        std::vector<const XMLElement*> definitions;
        for (XMLElement* c = root->FirstChildElement(); c; c = c->NextSiblingElement()) {
            if (isDefinition(tagOf(c))) {
                proot->InsertEndChild(c->DeepClone(&part));
                definitions.push_back(c);
            }
        }
        // the elements outside the definitions the part references, transitively
        std::set<std::string> wanted;
        collectRefsDeep(s.element, wanted);
        for (const XMLElement* a : s.chain) collectRefs(a, wanted);
        std::vector<const XMLElement*> extra;
        std::set<std::string> seen;
        while (!wanted.empty()) {
            const std::string id = *wanted.begin();
            wanted.erase(wanted.begin());
            if (!seen.insert(id).second) continue;
            auto it = ids.find(id);
            if (it == ids.end()) continue;
            const XMLElement* target = it->second;
            bool covered = inside(target, s.element);
            for (const XMLElement* d : definitions) covered = covered || inside(target, d);
            for (const XMLElement* x : extra) covered = covered || inside(target, x);
            if (covered) continue;
            extra.push_back(target);
            collectRefsDeep(target, wanted);
        }
        if (!extra.empty()) {
            XMLElement* defs = part.NewElement("defs");
            for (const XMLElement* x : extra) defs->InsertEndChild(x->DeepClone(&part));
            proot->InsertEndChild(defs);
        }
        XMLNode* at = proot;
        bool sharedEffects = false;
        for (const XMLElement* a : s.chain) {
            XMLElement* copy = a->ShallowClone(&part)->ToElement();
            copy->DeleteAttribute("id");
            at = at->InsertEndChild(copy);
            if (split == "paths" && (!property(a, "opacity").empty() || !attr(a, "filter").empty() || !attr(a, "mask").empty() ||
                                     !attr(a, "clip-path").empty())) {
                sharedEffects = true;
            }
        }
        at->InsertEndChild(s.element->DeepClone(&part));
        SvgPart p;
        p.name = s.name;
        p.text = print(part);
        p.mode = mode == "auto" ? partMode(part.RootElement()) : mode;
        if (p.mode == "shape") p.notes = shapeNotes(part.RootElement());
        if (sharedEffects) {
            p.notes.push_back("a group's opacity, filter, mask or clip applies to each of its parts alone (split: groups keeps it whole)");
        }
        out.parts.push_back(std::move(p));
    }
    out.ok = true;
    return out;
}

namespace {

    bool readFile(const fs::path& path, std::string& out) {
        std::ifstream in(path, std::ios::binary);
        if (!in) return false;
        std::ostringstream buffer;
        buffer << in.rdbuf();
        out = buffer.str();
        return true;
    }

    std::string generic(const fs::path& p) {
        return FileUtils::pathToUtf8(fs::path(p.lexically_normal().generic_string()));
    }

    // `path` under `dir` (no "..")
    bool under(const fs::path& path, const fs::path& dir, fs::path& relative) {
        if (dir.empty()) return false;
        std::error_code ec;
        relative = fs::relative(fs::absolute(path, ec).lexically_normal(), fs::absolute(dir, ec).lexically_normal(), ec);
        if (ec || relative.empty()) return false;
        const std::string first = relative.begin()->string();
        return first != ".." && !relative.is_absolute();
    }

    // Files beside an original inside the assets: a unique name, a file with the same content reused
    class BesideFiles {
    public:
        BesideFiles(fs::path dir, fs::path assets) : directory(std::move(dir)), assetsDir(std::move(assets)) {}

        std::string add(const std::string& name, const std::string& content) {
            const fs::path base(titleSafeFileName(name));
            const std::string stem = base.stem().string(), ext = base.extension().string();
            fs::path target = directory / base;
            for (int n = 2;; n++) {
                std::error_code ec;
                const std::string key = lower(generic(target));
                if (!taken.count(key)) {
                    if (!fs::exists(target, ec)) break;
                    std::string existing;
                    if (readFile(target, existing) && existing == content) {
                        taken.insert(key);
                        return relativeOf(target);
                    }
                }
                target = directory / (stem + "_" + std::to_string(n) + ext);
            }
            taken.insert(lower(generic(target)));
            staged.push_back({target.string(), content});
            return relativeOf(target);
        }

        std::vector<TitleOpFile> staged;

    private:
        fs::path directory, assetsDir;
        std::set<std::string> taken;
        std::string relativeOf(const fs::path& p) const {
            std::error_code ec;
            return generic(fs::relative(p, assetsDir, ec));
        }
    };

    fs::path resolveSource(const TitleOpsOptions& options, const std::string& file) {
        const fs::path given = FileUtils::pathFromUtf8(file);
        if (given.is_absolute()) return given.lexically_normal();
        std::error_code ec;
        for (const std::string& base : {options.projectPath, options.assetsPath}) {
            if (base.empty()) continue;
            const fs::path candidate = (fs::path(base) / given).lexically_normal();
            if (fs::exists(candidate, ec)) return candidate;
        }
        return options.projectPath.empty() ? given : (fs::path(options.projectPath) / given).lexically_normal();
    }

    std::string str(const Json& object, const char* key) {
        auto it = object.find(key);
        return it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
    }

}

bool editor::importSvgOp(TitleOpContext& ctx, const TitleJson& op) {
    std::string file;
    if (!ctx.requireString(op, "file", file)) return false;
    const std::string split = op.contains("split") ? str(op, "split") : "none";
    const std::string mode = op.contains("mode") ? str(op, "mode") : "auto";
    if (split != "none" && split != "groups" && split != "paths") {
        return ctx.fail("bad_value", "`split` is none, groups or paths", "split", {"none", "groups", "paths"});
    }
    if (mode != "auto" && mode != "shape" && mode != "artwork") {
        return ctx.fail("bad_value", "`mode` is auto, shape or artwork", "mode", {"auto", "shape", "artwork"});
    }
    if (!ctx.options || ctx.options->documentPath.empty() || ctx.options->assetsPath.empty()) {
        return ctx.fail("no_document", "import_svg writes its files beside the document: edit a .titleart of a project", "file");
    }
    const TitleOpsOptions& options = *ctx.options;
    const fs::path source = resolveSource(options, file);
    std::string text;
    if (!readFile(source, text)) return ctx.fail("file_not_found", "cannot read '" + file + "'", "file");
    const SvgSplit parts = splitSvg(text, split, mode);
    if (!parts.ok) return ctx.fail("bad_svg", "'" + file + "': " + parts.error, "file");

    // where the original and the parts live
    const fs::path assets = fs::path(options.assetsPath);
    fs::path relative;
    std::string originalRef;
    std::function<std::string(const std::string&, const std::string&)> addPart;
    TitleImportAssets copies(fs::path(options.documentPath), assets);
    BesideFiles beside(source.parent_path(), assets);
    if (under(source, assets, relative)) {
        originalRef = generic(relative);
        addPart = [&](const std::string& name, const std::string& content) { return beside.add(name, content); };
    } else {
        originalRef = copies.add(source.filename().string(), text);
        addPart = [&](const std::string& name, const std::string& content) { return copies.add(name, content); };
    }
    const std::string stem = source.stem().string();
    const std::string layerName = op.contains("name") ? str(op, "name") : stem;
    const double size = std::max(1.0, std::max(parts.width, parts.height) > 0 ? std::max(parts.width, parts.height) : 200.0);

    std::vector<std::string> layerIds;
    TitleJson notes = TitleJson::array();
    const auto addVector = [&](const std::string& name, const std::string& ref, const std::string& partMode, const TitleJson& parent) -> bool {
        TitleJson add = {{"op", "add_layer"}, {"type", "vector"}, {"name", name},
                         {"values", {{"svg", {{"path", ref}, {"guid", ""}}}, {"size", size}, {"mode", partMode}}}};
        if (!parent.is_null()) add["parent"] = parent;
        if (!TitleArtOps::runCoreOp(ctx, add)) return false;
        layerIds.push_back(ctx.result.newIds[std::to_string(ctx.opIndex)]["layer"].get<std::string>());
        return true;
    };
    const auto noteFor = [&](const std::string& layerId, const std::vector<std::string>& what) {
        for (const std::string& w : what) {
            TitleImportNote n;
            n.layer = ctx.layerPath(layerId);
            n.field = "svg";
            n.what = w;
            n.action = w.rfind("a group's", 0) == 0 ? "approximated" : "dropped";
            notes.push_back(n.toJson());
        }
    };
    const TitleJson parent = op.contains("parent") ? op["parent"] : TitleJson();
    if (split == "none") {
        if (!addVector(layerName, originalRef, parts.parts.front().mode, parent)) return false;
        noteFor(layerIds.back(), parts.parts.front().notes);
    } else {
        TitleJson group = {{"op", "add_layer"}, {"type", "group"}, {"name", layerName}};
        if (!parent.is_null()) group["parent"] = parent;
        if (!TitleArtOps::runCoreOp(ctx, group)) return false;
        const std::string groupId = ctx.result.newIds[std::to_string(ctx.opIndex)]["layer"].get<std::string>();
        for (const SvgPart& p : parts.parts) {
            const std::string ref = addPart(stem + "_" + p.name + ".svg", p.text);
            if (!addVector(p.name, ref, p.mode, "id:" + groupId)) return false;
            noteFor(layerIds.back(), p.notes);
        }
        layerIds.insert(layerIds.begin(), groupId);
    }
    TitleJson files = TitleJson::array();
    std::vector<TitleOpFile> written = copies.take();
    written.insert(written.end(), beside.staged.begin(), beside.staged.end());
    for (const TitleOpFile& f : written) {
        files.push_back(f.path);
        ctx.result.files.push_back(f);
    }
    TitleJson& ids = ctx.result.newIds[std::to_string(ctx.opIndex)];
    ids["layer"] = layerIds.front();
    ids["layers"] = layerIds;
    ctx.result.reports[std::to_string(ctx.opIndex)] = {{"kind", "svg"}, {"files", files}, {"notes", notes}, {"layers", layerIds}};
    for (const TitleJson& n : notes) ctx.note("import_note", n.value("layer", std::string()) + ": " + n.value("what", std::string()), "file");
    ctx.undoName = titleImportUndoName("svg", source.filename().string());
    return true;
}

namespace {

    bool sniffSvg(const std::string& head, const std::string& extension) {
        const std::string text = lower(head);
        const size_t svg = text.find("<svg");
        if (svg == std::string::npos) return false;
        // an XML (or bare) document whose first element is <svg>
        const std::string before = text.substr(0, svg);
        return extension == ".svg" || before.find('<') == std::string::npos || before.find("<?xml") != std::string::npos ||
               before.find("<!--") != std::string::npos || before.find("<!doctype") != std::string::npos;
    }

    bool importSvgDocument(const TitleCreateRequest& request, TitleCreateResult& out, std::string& code, std::string& error) {
        std::string text;
        if (!readFile(fs::path(request.sourcePath), text)) {
            code = "file_not_found";
            error = "cannot read '" + request.sourcePath + "'";
            return false;
        }
        const SvgSplit probe = splitSvg(text, "none");
        if (!probe.ok) {
            code = "bad_svg";
            error = probe.error;
            return false;
        }
        titleart::NewTitleArtOptions options;
        options.width = std::clamp((int)std::ceil(probe.width > 0 ? probe.width : 512.0), titleart::TITLE_MIN_CANVAS_SIDE, titleart::TITLE_MAX_CANVAS_SIDE);
        options.height = std::clamp((int)std::ceil(probe.height > 0 ? probe.height : 512.0), titleart::TITLE_MIN_CANVAS_SIDE, titleart::TITLE_MAX_CANVAS_SIDE);
        titleart::TitleArtDoc doc = titleart::makeNewTitleArt(options);
        doc.root["layers"] = Json::array();
        doc.root["exposed"] = Json::array();
        doc.root["name"] = fs::path(request.sourcePath).stem().string();
        TitleOpsOptions opsOptions;
        opsOptions.documentPath = request.documentPath;
        opsOptions.projectPath = request.projectPath;
        opsOptions.assetsPath = request.assetsPath;
        const TitleOpsResult result = TitleArtOps::apply(doc, Json{{"op", "import_svg"}, {"file", request.sourcePath}}, opsOptions);
        if (!result.ok) {
            code = result.problems.empty() ? "import_failed" : result.problems.front().code;
            error = result.error();
            return false;
        }
        out.doc = result.doc;
        out.files = result.files;
        if (result.reports.contains("0")) out.notes = result.reports["0"].value("notes", Json::array());
        Json lines = Json::array();
        for (const Json& n : out.notes) {
            TitleImportNote note;
            note.layer = n.value("layer", std::string());
            note.field = n.value("field", std::string());
            note.what = n.value("what", std::string());
            note.action = n.value("action", std::string());
            lines.push_back(note.toLine());
        }
        out.doc.root["import_notes"] = lines;
        return true;
    }

}

void editor::registerSvgInterchange() {
    TitleArtOps::registerOp("import_svg", importSvgOp);
    TitleArtOps::registerCreateImporter("svg", sniffSvg, importSvgDocument);
}
