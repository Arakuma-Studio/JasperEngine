// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "htmlui/compat/DocumentLoader.h"

#include "htmlui/compat/Hash.h"
#include "htmlui/compat/ImportManifest.h"
#include "htmlui/compat/RmlLogTranslator.h"
#include "htmlui/compat/UrlResolver.h"
#include "htmlui/compat/VirtualFiles.h"
#include "htmlui/compat/css/RcssWriter.h"
#include "htmlui/compat/html/HtmlParser.h"
#include "htmlui/compat/html/HtmlTransform.h"
#include "htmlui/compat/html/RmlSerializer.h"
#include "htmlui/compat/ua/UserAgentStyle.h"

#include "util/FontRegistry.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/Factory.h>
#include <RmlUi/Core/StreamMemory.h>
#include <RmlUi/Core/StyleSheetContainer.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <cstring>
#include <list>
#include <mutex>
#include <unordered_map>

using namespace jasperengine;
using namespace jasperengine::htmlui::compat;

namespace {

    // ---- the cache (§3.12.2) ----

    constexpr size_t kCacheLimit = 64;

    struct CacheEntry {
        std::shared_ptr<const CompatDocument> document;
        uint64_t lastUse = 0;
    };

    struct Cache {
        std::mutex mutex;
        // key: path + options (without the viewport); entries: one per dependency state/viewport
        std::unordered_map<uint64_t, std::vector<CacheEntry>> buckets;
        uint64_t clock = 0;
        size_t size = 0;
    };

    Cache& cache(){
        static Cache instance;
        return instance;
    }

    // VirtualFiles owners of compiles: far from the small numbers other owners (tests) use
    std::atomic<uint64_t> nextOwner{0x4854000000000001ull};

    std::string normalizePath(std::string path){
        for (char& c : path){
            if (c == '\\')
                c = '/';
        }
        if (path.compare(0, 8, "asset://") == 0){
            path.erase(0, 8);
            while (!path.empty() && path[0] == '/')
                path.erase(0, 1);
        }
        // a leading '/' stays: an absolute filesystem path (tests, tools), as RmlUi documents
        // opened by one keep theirs
        return path;
    }

    // content stamps of the cache: checked on every lookup, so 8 bytes at a time (FNV-1a goes
    // byte by byte); not stored anywhere outside the process
    uint64_t contentHash(std::string_view text){
        uint64_t hash = 0x9E3779B97F4A7C15ull ^ (uint64_t)text.size();
        size_t i = 0;
        for (; i + 8 <= text.size(); i += 8){
            uint64_t word;
            std::memcpy(&word, text.data() + i, 8);
            hash = (hash ^ word) * 0xFF51AFD7ED558CCDull;
            hash ^= hash >> 32;
        }
        uint64_t tail = 0;
        std::memcpy(&tail, text.data() + i, text.size() - i);
        hash = (hash ^ tail) * 0xC4CEB9FE1A85EC53ull;
        return hash ^ (hash >> 29);
    }

    uint64_t hashString(uint64_t seed, std::string_view text){
        // the length first, so "ab"+"c" and "a"+"bc" differ
        const uint64_t length = text.size();
        seed = fnv1a64(std::string_view(reinterpret_cast<const char*>(&length), sizeof(length)), seed);
        return fnv1a64(text, seed);
    }

    // everything of the options that changes the output, the viewport apart
    uint64_t optionsHash(const CompatOptions& options){
        uint64_t hash = 0xcbf29ce484222325ull;
        const uint64_t bits = options.fork.bits();
        hash = fnv1a64(std::string_view(reinterpret_cast<const char*>(&bits), sizeof(bits)), hash);
        const float ratio = options.dpRatio;
        hash = fnv1a64(std::string_view(reinterpret_cast<const char*>(&ratio), sizeof(ratio)), hash);
        const uint32_t flags = (options.touchDevice ? 1u : 0u) | (options.reducedMotion ? 2u : 0u) | (options.dropTransitionsForTesting ? 4u : 0u);
        hash = fnv1a64(std::string_view(reinterpret_cast<const char*>(&flags), sizeof(flags)), hash);
        const uint64_t limit = options.diagnosticLimit;
        hash = fnv1a64(std::string_view(reinterpret_cast<const char*>(&limit), sizeof(limit)), hash);
        hash = hashString(hash, options.colorScheme);
        const uint32_t version = DocumentLoader::kVersion;
        return fnv1a64(std::string_view(reinterpret_cast<const char*>(&version), sizeof(version)), hash);
    }

    uint64_t baseKey(const std::string& path, const CompatOptions& options){
        return hashString(optionsHash(options), path);
    }

    uint64_t stampOf(const FileAccess& files, const std::string& path, bool content){
        if (content && VirtualFiles::isVirtual(path)){
            std::shared_ptr<const std::string> text = VirtualFiles::find(path);
            return text ? (contentHash(*text) | 1ull) : 0;
        }
        if (content){
            std::string text;
            if (!readAssetFile(files, path, text))
                return 0;
            return contentHash(text) | 1ull;   // never 0 (missing)
        }
        return assetFileExists(files, path) ? 1 : 0;
    }

    bool isCurrent(const CompatDocument& document, const CompatOptions& options){
        if (document.dependsOnViewport && document.options.viewport != options.viewport)
            return false;
        const FileAccess files;
        for (const CompatDocument::DependencyStamp& stamp : document.dependencyStamps){
            if (stampOf(files, stamp.path, stamp.content) != stamp.hash)
                return false;
        }
        return true;
    }

    void dropEntry(const std::shared_ptr<const CompatDocument>& document){
        if (document && document->owner)
            VirtualFiles::removeOwner(document->owner);
    }

    // ---- the compile ----

    void numberElements(HtmlNode& node, std::vector<SourceRef>& refs){
        if (node.kind != HtmlNode::Kind::Element)
            return;
        // RmlUi makes no elements of an inline SVG's content (it is the SVG's text)
        node.loc = (uint32_t)refs.size();
        node.hasLoc = true;
        refs.push_back(node.location());
        if (node.ns == Namespace::Svg)
            return;
        for (std::unique_ptr<HtmlNode>& child : node.children)
            numberElements(*child, refs);
    }

    void splitDeclarations(const std::string& declarations, std::vector<std::pair<std::string, std::string>>& out){
        size_t start = 0;
        for (size_t i = 0; i <= declarations.size(); i++){
            if (i == declarations.size() || declarations[i] == ';'){
                const std::string part = html::trim(declarations.substr(start, i - start));
                const size_t colon = part.find(':');
                if (colon != std::string::npos)
                    out.emplace_back(html::trim(part.substr(0, colon)), html::trim(part.substr(colon + 1)));
                start = i + 1;
            }
        }
    }

    StylesheetOutput writeHints(const std::vector<HintRule>& rules, const std::string& documentPath){
        StylesheetOutput output;
        output.kind = StylesheetInput::Kind::UserAgent;
        output.sourcePath = documentPath;
        if (rules.empty())
            return output;
        RcssWriter writer(RcssWriter::Mode::Sheet);
        for (const HintRule& rule : rules){
            std::vector<std::pair<std::string, std::string>> declarations;
            if (!rule.declarations.own.empty()){
                splitDeclarations(rule.declarations.own, declarations);
                writer.beginRule("." + rule.className, rule.declarations.ref);
                for (const auto& pair : declarations)
                    writer.declaration(pair.first, pair.second, rule.declarations.ref);
                writer.endRule();
            }
            if (!rule.declarations.cells.empty()){
                declarations.clear();
                splitDeclarations(rule.declarations.cells, declarations);
                writer.beginRule("." + rule.className + " td, ." + rule.className + " th", rule.declarations.ref);
                for (const auto& pair : declarations)
                    writer.declaration(pair.first, pair.second, rule.declarations.ref);
                writer.endRule();
            }
        }
        output.rcss = writer.take(output.lines);
        return output;
    }

    void addDependency(CompatDocument& document, const std::string& path){
        if (!path.empty() && std::find(document.dependencies.begin(), document.dependencies.end(), path) == document.dependencies.end())
            document.dependencies.push_back(path);
    }

    std::shared_ptr<CompatDocument> compileText(std::shared_ptr<CompatDocument> document, std::string text,
                                                const CompatOptions& options, const FileAccess& access){
        SourceFiles& files = document->files;
        auto PROF_T = std::chrono::steady_clock::now(); auto PROF = [&PROF_T](const char* what){ if (std::getenv("JASPER_COMPAT_PROFILE")){ auto now = std::chrono::steady_clock::now(); std::printf("[prof] %s %.3f\n", what, std::chrono::duration<double, std::milli>(now - PROF_T).count()); PROF_T = now; } };
        DiagnosticSink sink(files, options.diagnosticLimit);
        const std::string& path = document->sourcePath;

        stripUtf8Bom(text);
        const std::vector<uint32_t> invalid = sanitizeUtf8(text);
        const uint32_t file = files.add(path, std::move(text));
        if (!invalid.empty()){
            sink.report(codes::InvalidUtf8, SourceRef{file, invalid[0], 3},
                        std::to_string(invalid.size()) + " invalid UTF-8 sequence(s) replaced by U+FFFD", "save the file as UTF-8");
        }

        // a check outside a loaded project reads the manifest through its file access
        ImportManifest ownManifest;
        const ImportManifest* manifest = nullptr;
        if (access.read){
            ownManifest = ImportManifest::load(access);
            manifest = &ownManifest;
        }
        UrlResolver urls(access, manifest);

        HtmlDocument parsed = HtmlParser::parseDocument(file, files.text(file), &sink);
        PROF("PARSE");

        TransformContext context(options, files, sink, urls);
        context.file = file;
        context.path = path;
        context.root = parsed.root.get();
        context.fileAccess = access;
        runTransforms(context);
        PROF("TRANSFORM");

        document->title = context.title;
        document->owner = nextOwner.fetch_add(1);
        document->virtualDirectory = std::string(VirtualFiles::kPrefix) + "doc/" + toHex(document->owner) + "/";

        // data-htmlui-loc: the document in pre-order, then each template's content
        numberElements(*context.root, document->sourceMap.elementRefs);
        for (TemplateSource& source : context.templates){
            for (std::unique_ptr<HtmlNode>& child : source.content->children)
                numberElements(*child, document->sourceMap.elementRefs);
        }

        // ---- the CSS session: author sheets in tree order, then the style="" attributes ----
        std::vector<StylesheetInput> inputs;
        std::vector<size_t> sheetOfInput;   // index into context.stylesheets (or SIZE_MAX for attributes)
        for (size_t i = 0; i < context.stylesheets.size(); i++){
            const StylesheetSource& source = context.stylesheets[i];
            StylesheetInput input;
            input.media = source.media;
            if (source.kind == StylesheetSource::Kind::File){
                std::string css;
                if (!readAssetFile(access, source.path, css))
                    continue;   // reported by the resolver (missing file); creating it reloads the document
                stripUtf8Bom(css);
                const std::vector<uint32_t> bad = sanitizeUtf8(css);
                const uint32_t cssFile = files.add(source.path, std::move(css), source.ref);
                if (!bad.empty()){
                    sink.report(codes::InvalidUtf8, SourceRef{cssFile, bad[0], 3},
                                std::to_string(bad.size()) + " invalid UTF-8 sequence(s) replaced by U+FFFD", "save the file as UTF-8");
                }
                input.kind = StylesheetInput::Kind::File;
                input.file = cssFile;
                input.text = files.text(cssFile);
                document->dependencyStamps.push_back({source.path, contentHash(files.text(cssFile)) | 1ull, true});
            }else{
                input.kind = StylesheetInput::Kind::Inline;
                input.file = file;
                input.text = source.text;
                input.baseOffset = source.baseOffset;
            }
            inputs.push_back(input);
            sheetOfInput.push_back(i);
        }
        const size_t authorInputs = inputs.size();
        std::vector<HtmlNode*> styledElements;
        for (const StyleAttributeSource& attribute : context.styleAttributes){
            StylesheetInput input;
            input.kind = StylesheetInput::Kind::StyleAttribute;
            input.file = file;
            input.text = attribute.text;
            input.baseOffset = attribute.baseOffset;
            input.elementLoc = attribute.element->loc;
            inputs.push_back(input);
            styledElements.push_back(attribute.element);
        }

        RewriteContext rewriteContext;
        rewriteContext.files = access;
        rewriteContext.manifest = manifest;
        rewriteContext.virtualDirectory = document->virtualDirectory;
        rewriteContext.documentPath = path;
        rewriteContext.rootId = context.rootId;
        rewriteContext.rootDirection = context.rootDirection;

        RewriteResult result;
        PROF("PRECSS");
        {
            CssRewriter rewriter(options, files, sink, rewriteContext);
            result = rewriter.rewrite(Span<const StylesheetInput>(inputs));
        }

        for (size_t i = 0; i < result.sheets.size() && i < inputs.size(); i++){
            StylesheetOutput& output = result.sheets[i];
            if (i < authorInputs){
                if (output.kind == StylesheetInput::Kind::File){
                    if (output.virtualPath.empty())
                        output.virtualPath = document->virtualDirectory + output.sourcePath + ".rcss";
                    VirtualFiles::add(output.virtualPath, output.rcss, document->owner);
                    document->virtualPaths.push_back(output.virtualPath);
                }
                document->stylesheets.push_back(std::move(output));
            }else{
                HtmlNode* element = styledElements[i - authorInputs];
                element->hasRewrittenStyle = true;
                element->rewrittenStyle = html::trim(output.rcss);
                document->styleAttributes.emplace_back(element->loc, element->rewrittenStyle);
            }
        }
        document->important = std::move(result.importantSheet);
        PROF("CSS");
        document->important.kind = StylesheetInput::Kind::Inline;
        document->hints = writeHints(context.hintRules, path);

        // ---- RML ----
        const std::shared_ptr<const UserAgentStyle::Sheet> userAgent = UserAgentStyle::get(options.fork);
        document->userAgentPath = userAgent ? userAgent->virtualPath : UserAgentStyle::virtualPath(options.fork);

        RmlSerializer::Document rml;
        rml.title = context.title;
        rml.root = context.root;
        rml.body = context.body;
        {
            RmlSerializer::HeadItem item;
            item.kind = RmlSerializer::HeadItem::Kind::Link;
            item.href = "/" + document->userAgentPath;
            item.attributes = " origin=\"user-agent\"";
            rml.head.push_back(item);
        }
        const size_t hintsItem = rml.head.size();
        if (!document->hints.rcss.empty()){
            RmlSerializer::HeadItem item;
            item.kind = RmlSerializer::HeadItem::Kind::Style;
            item.rcss = document->hints.rcss;
            rml.head.push_back(item);
        }
        std::vector<size_t> sheetItems;
        for (size_t i = 0; i < document->stylesheets.size(); i++){
            const StylesheetOutput& sheet = document->stylesheets[i];
            RmlSerializer::HeadItem item;
            item.ref = context.stylesheets[sheetOfInput[i]].ref;
            if (sheet.kind == StylesheetInput::Kind::File){
                item.kind = RmlSerializer::HeadItem::Kind::Link;
                item.href = "/" + VirtualFiles::normalize(sheet.virtualPath);
            }else{
                item.kind = RmlSerializer::HeadItem::Kind::Style;
                item.rcss = sheet.rcss;
            }
            sheetItems.push_back(rml.head.size());
            rml.head.push_back(item);
        }
        const size_t importantItem = rml.head.size();
        if (!document->important.rcss.empty()){
            RmlSerializer::HeadItem item;
            item.kind = RmlSerializer::HeadItem::Kind::Style;
            item.rcss = document->important.rcss;
            rml.head.push_back(item);
        }

        RmlSerializer::Output output = RmlSerializer::writeDocument(rml);
        PROF("SERIALIZE");
        document->rml = std::move(output.rml);
        document->sourceMap.lineRefs = std::move(output.lineRefs);
        if (!document->hints.rcss.empty())
            document->hintsLine = rml.head[hintsItem].contentLine;
        for (size_t i = 0; i < sheetItems.size(); i++)
            document->stylesheetLines.push_back(rml.head[sheetItems[i]].contentLine);
        if (!document->important.rcss.empty())
            document->importantLine = rml.head[importantItem].contentLine;

        // template contents: RML fragments (their style="" were rewritten with the document)
        for (TemplateSource& source : context.templates){
            HtmlTemplate rmlTemplate;
            rmlTemplate.id = source.id;
            rmlTemplate.ref = source.ref;
            rmlTemplate.rml = RmlSerializer::writeFragment(*source.content).rml;
            document->templates.push_back(std::move(rmlTemplate));
        }

        document->scripts = std::move(context.scripts);

        // data: URIs are content-addressed: the same path always holds the same bytes, so they
        // live for the process (a document still drawing one never loses it)
        for (auto& pair : context.virtualFiles)
            VirtualFiles::add(pair.first, pair.second, 0);
        for (auto& pair : result.virtualFiles)
            VirtualFiles::add(pair.first, pair.second, 0);

        // ---- dependencies, references ----
        for (const std::string& dependency : context.dependencies)
            addDependency(*document, dependency);
        for (const std::string& dependency : result.dependencies)
            addDependency(*document, dependency);
        for (const DocumentScript& script : document->scripts){
            if (!script.srcPath.empty())
                document->dependencyStamps.push_back({script.srcPath, contentHash(script.code) | 1ull, true});
        }
        document->remoteRefs = std::move(context.remoteRefs);
        for (RemoteReference& reference : result.remoteRefs){
            bool known = false;
            for (const RemoteReference& existing : document->remoteRefs){
                if (existing.url == reference.url && existing.kind == reference.kind)
                    known = true;
            }
            if (!known)
                document->remoteRefs.push_back(std::move(reference));
        }
        if (!document->remoteRefs.empty())
            addDependency(*document, ImportManifest::kPath);

        // stamps of what the output depends on: contents of the files read, existence of the rest
        for (const std::string& dependency : document->dependencies){
            if (dependency == path)
                continue;
            bool stamped = false;
            for (const CompatDocument::DependencyStamp& stamp : document->dependencyStamps){
                if (stamp.path == dependency)
                    stamped = true;
            }
            if (stamped)
                continue;
            const bool content = dependency == ImportManifest::kPath || files.find(dependency) != 0;
            document->dependencyStamps.push_back({dependency, stampOf(access, dependency, content), content});
        }

        document->dependsOnViewport = result.dependsOnViewport || context.dependsOnViewport;
        document->animationFillNames = std::move(result.animationFillNames);
        document->fontFamilies = std::move(result.fontFamilies);
        document->declaredFontFamilies = std::move(result.declaredFontFamilies);
        document->diagnostics = sink.take();
        PROF("REST");
        document->compiled = true;

        uint64_t key = baseKey(path, options);
        for (const CompatDocument::DependencyStamp& stamp : document->dependencyStamps){
            key = hashString(key, stamp.path);
            key = fnv1a64(std::string_view(reinterpret_cast<const char*>(&stamp.hash), sizeof(stamp.hash)), key);
        }
        if (document->dependsOnViewport){
            const int size[2] = {options.viewport.x, options.viewport.y};
            key = fnv1a64(std::string_view(reinterpret_cast<const char*>(size), sizeof(size)), key);
        }
        document->key = key;
        return document;
    }

    std::shared_ptr<CompatDocument> compileFile(const std::string& htmlPath, const CompatOptions& options, const FileAccess& access){
        std::shared_ptr<CompatDocument> document = std::make_shared<CompatDocument>();
        document->sourcePath = normalizePath(htmlPath);
        document->options = options;
        document->dependencies.push_back(document->sourcePath);

        std::string text;
        if (!readAssetFile(access, document->sourcePath, text)){
            DiagnosticSink sink(document->files, options.diagnosticLimit);
            Diagnostic missing;
            missing.code = codes::FileNotFound;
            missing.severity = Severity::Error;
            missing.message = "document '" + document->sourcePath + "' not found";
            missing.file = document->sourcePath;
            missing.subject = document->sourcePath;
            sink.report(missing);
            document->diagnostics = sink.take();
            document->dependencyStamps.push_back({document->sourcePath, 0, true});
            return document;
        }
        document->dependencyStamps.push_back({document->sourcePath, contentHash(text) | 1ull, true});
        return compileText(document, std::move(text), options, access);
    }

    // ---- loaded documents ----

    struct Binding {
        std::shared_ptr<const CompatDocument> compiled;
        uint64_t translatorHandle = 0;
        std::vector<Diagnostic> diagnostics;   // compile + runtime
    };

    std::unordered_map<const Rml::ElementDocument*, std::shared_ptr<Binding>>& bindings(){
        static std::unordered_map<const Rml::ElementDocument*, std::shared_ptr<Binding>> map;
        return map;
    }

    const std::vector<Diagnostic>& noDiagnostics(){
        static const std::vector<Diagnostic> none;
        return none;
    }

    // removes the binding when RmlUi destroys the document
    class BindingListener : public Rml::EventListener {
    public:
        explicit BindingListener(const Rml::ElementDocument* document): document(document) {}
        void ProcessEvent(Rml::Event&) override {}
        void OnDetach(Rml::Element*) override{
            auto& map = bindings();
            auto it = map.find(document);
            if (it != map.end()){
                if (it->second->translatorHandle)
                    RmlLogTranslator::remove(it->second->translatorHandle);
                map.erase(it);
            }
            delete this;
        }

    private:
        const Rml::ElementDocument* document;
    };

    uint64_t registerBinding(const std::shared_ptr<Binding>& binding){
        std::weak_ptr<Binding> weak = binding;
        return DocumentLoader::registerDocument(binding->compiled, [weak](const Diagnostic& diagnostic){
            if (std::shared_ptr<Binding> live = weak.lock())
                live->diagnostics.push_back(diagnostic);
        });
    }

    std::shared_ptr<Rml::StyleSheetContainer> inlineSheet(const std::string& rcss, const std::string& sourceUrl, uint32_t line){
        auto sheet = std::make_shared<Rml::StyleSheetContainer>();
        Rml::StreamMemory stream(reinterpret_cast<const Rml::byte*>(rcss.data()), rcss.size());
        stream.SetSourceURL(sourceUrl);
        if (!sheet->LoadStyleSheetContainer(&stream, (int)(line > 0 ? line : 1)))
            return nullptr;
        return sheet;
    }

}

// ---- compile ----

std::shared_ptr<const CompatDocument> DocumentLoader::compile(const std::string& htmlPath, const CompatOptions& options){
    const std::string path = normalizePath(htmlPath);
    const uint64_t bucketKey = baseKey(path, options);

    Cache& store = cache();
    {
        std::lock_guard<std::mutex> lock(store.mutex);
        auto it = store.buckets.find(bucketKey);
        if (it != store.buckets.end()){
            for (CacheEntry& entry : it->second){
                if (entry.document->sourcePath == path && isCurrent(*entry.document, options)){
                    entry.lastUse = ++store.clock;
                    return entry.document;
                }
            }
        }
    }

    std::shared_ptr<const CompatDocument> compiled = compileFile(path, options, FileAccess());
    if (!compiled->compiled)
        return compiled;   // a missing file is not cached: creating it must load

    std::vector<std::shared_ptr<const CompatDocument>> dropped;
    {
        std::lock_guard<std::mutex> lock(store.mutex);
        std::vector<CacheEntry>& bucket = store.buckets[bucketKey];
        // what this compile replaces (another state of the same document and viewport)
        for (auto it = bucket.begin(); it != bucket.end();){
            if (it->document->sourcePath == path && (!it->document->dependsOnViewport || it->document->options.viewport == options.viewport) &&
                it->document->key != compiled->key){
                dropped.push_back(it->document);
                it = bucket.erase(it);
                store.size--;
            }else{
                ++it;
            }
        }
        bucket.push_back(CacheEntry{compiled, ++store.clock});
        store.size++;
        // least recently used beyond the limit
        while (store.size > kCacheLimit){
            uint64_t oldest = UINT64_MAX;
            std::vector<CacheEntry>* oldestBucket = nullptr;
            size_t oldestIndex = 0;
            for (auto& pair : store.buckets){
                for (size_t i = 0; i < pair.second.size(); i++){
                    if (pair.second[i].lastUse < oldest){
                        oldest = pair.second[i].lastUse;
                        oldestBucket = &pair.second;
                        oldestIndex = i;
                    }
                }
            }
            if (!oldestBucket)
                break;
            dropped.push_back((*oldestBucket)[oldestIndex].document);
            oldestBucket->erase(oldestBucket->begin() + (std::ptrdiff_t)oldestIndex);
            store.size--;
        }
    }
    for (const std::shared_ptr<const CompatDocument>& document : dropped)
        dropEntry(document);
    return compiled;
}

std::shared_ptr<const CompatDocument> DocumentLoader::compile(const std::string& htmlPath, const CompatOptions& options, const FileAccess& files){
    if (!files.read && !files.exists)
        return compile(htmlPath, options);
    return compileFile(htmlPath, options, files);
}

std::shared_ptr<const CompatDocument> DocumentLoader::compileString(std::string_view html, const std::string& virtualPath,
                                                                    const CompatOptions& options){
    std::shared_ptr<CompatDocument> document = std::make_shared<CompatDocument>();
    document->sourcePath = normalizePath(virtualPath);
    document->options = options;
    document->dependencies.push_back(document->sourcePath);
    return compileText(document, std::string(html), options, FileAccess());
}

void DocumentLoader::invalidate(const std::string& assetPath){
    const std::string path = normalizePath(assetPath);
    std::vector<std::shared_ptr<const CompatDocument>> dropped;
    {
        Cache& store = cache();
        std::lock_guard<std::mutex> lock(store.mutex);
        for (auto& pair : store.buckets){
            for (auto it = pair.second.begin(); it != pair.second.end();){
                const std::vector<std::string>& dependencies = it->document->dependencies;
                if (std::find(dependencies.begin(), dependencies.end(), path) != dependencies.end()){
                    dropped.push_back(it->document);
                    it = pair.second.erase(it);
                    store.size--;
                }else{
                    ++it;
                }
            }
        }
    }
    for (const std::shared_ptr<const CompatDocument>& document : dropped)
        dropEntry(document);
    if (path == ImportManifest::kPath)
        ImportManifest::notifyFileChanged(path);
    if (Rml::GetSystemInterface())
        Rml::Factory::ClearStyleSheetCache();
}

bool DocumentLoader::sameRml(const CompatDocument& a, const CompatDocument& b){
    if (a.rml.size() != b.rml.size() + a.virtualDirectory.size() - b.virtualDirectory.size())
        return false;
    // walk both, skipping each one's own virtual directory where it appears
    size_t i = 0, j = 0;
    while (i < a.rml.size() && j < b.rml.size()){
        if (!a.virtualDirectory.empty() && a.rml.compare(i, a.virtualDirectory.size(), a.virtualDirectory) == 0 &&
            !b.virtualDirectory.empty() && b.rml.compare(j, b.virtualDirectory.size(), b.virtualDirectory) == 0){
            i += a.virtualDirectory.size();
            j += b.virtualDirectory.size();
            continue;
        }
        if (a.rml[i] != b.rml[j])
            return false;
        i++;
        j++;
    }
    return i == a.rml.size() && j == b.rml.size();
}

void DocumentLoader::clearCache(){
    std::vector<std::shared_ptr<const CompatDocument>> dropped;
    {
        Cache& store = cache();
        std::lock_guard<std::mutex> lock(store.mutex);
        for (auto& pair : store.buckets){
            for (CacheEntry& entry : pair.second)
                dropped.push_back(entry.document);
        }
        store.buckets.clear();
        store.size = 0;
    }
    for (const std::shared_ptr<const CompatDocument>& document : dropped)
        dropEntry(document);
}

size_t DocumentLoader::cacheSize(){
    Cache& store = cache();
    std::lock_guard<std::mutex> lock(store.mutex);
    return store.size;
}

// ---- load ----

uint64_t DocumentLoader::registerDocument(const std::shared_ptr<const CompatDocument>& compiled, std::function<void(const Diagnostic&)> sink){
    if (!compiled)
        return 0;
    RmlLogTranslator::Registration registration;
    registration.key = compiled->key;
    registration.owner = compiled;
    registration.files = &compiled->files;
    registration.sink = std::move(sink);
    for (size_t i = 0; i < compiled->stylesheets.size(); i++){
        const StylesheetOutput& sheet = compiled->stylesheets[i];
        if (sheet.kind == StylesheetInput::Kind::File){
            registration.mappings.push_back(RmlLogTranslator::LineMapping{sheet.virtualPath, 1, &sheet.lines.lines});
        }else if (i < compiled->stylesheetLines.size() && compiled->stylesheetLines[i] > 0){
            registration.mappings.push_back(RmlLogTranslator::LineMapping{compiled->sourcePath, compiled->stylesheetLines[i], &sheet.lines.lines});
        }
    }
    if (compiled->hintsLine > 0)
        registration.mappings.push_back(RmlLogTranslator::LineMapping{compiled->sourcePath, compiled->hintsLine, &compiled->hints.lines.lines});
    if (compiled->importantLine > 0)
        registration.mappings.push_back(RmlLogTranslator::LineMapping{compiled->sourcePath, compiled->importantLine, &compiled->important.lines.lines});
    registration.mappings.push_back(RmlLogTranslator::LineMapping{compiled->sourcePath, 1, &compiled->sourceMap.lineRefs});
    return RmlLogTranslator::add(std::move(registration));
}

void DocumentLoader::unregisterDocument(uint64_t handle){
    if (handle)
        RmlLogTranslator::remove(handle);
}

Rml::ElementDocument* DocumentLoader::load(Rml::Context* context, const std::string& htmlPath, const CompatOptions& options,
                                           std::shared_ptr<const CompatDocument>* out){
    std::shared_ptr<const CompatDocument> compiled = compile(htmlPath, options);
    if (out)
        *out = compiled;
    if (!compiled->compiled || !context){
        logDiagnostics(compiled->sourcePath, compiled->diagnostics, options.verboseDiagnostics);
        return nullptr;
    }

    // RmlUi's messages while it parses the RML and the sheets map back from now on
    std::shared_ptr<Binding> binding = std::make_shared<Binding>();
    binding->compiled = compiled;
    binding->translatorHandle = registerBinding(binding);

    Rml::ElementDocument* document = context->LoadDocumentFromMemory(compiled->rml, compiled->sourcePath);
    if (!document){
        unregisterDocument(binding->translatorHandle);
        logDiagnostics(compiled->sourcePath, compiled->diagnostics, options.verboseDiagnostics);
        return nullptr;
    }

    // the compile's diagnostics, the mapped RmlUi messages of the load, the font check
    std::vector<Diagnostic> runtime = std::move(binding->diagnostics);
    binding->diagnostics = compiled->diagnostics;
    std::vector<Diagnostic> fonts = checkFontFamilies(*compiled);
    binding->diagnostics.insert(binding->diagnostics.end(), runtime.begin(), runtime.end());
    binding->diagnostics.insert(binding->diagnostics.end(), fonts.begin(), fonts.end());

    std::vector<Diagnostic> logged = compiled->diagnostics;
    logged.insert(logged.end(), fonts.begin(), fonts.end());
    logDiagnostics(compiled->sourcePath, logged, options.verboseDiagnostics);

    bindings()[document] = binding;
    document->AddEventListener(Rml::EventId::Unload, new BindingListener(document));
    return document;
}

std::shared_ptr<const CompatDocument> DocumentLoader::compiledOf(const Rml::ElementDocument* document){
    auto it = bindings().find(document);
    return (it != bindings().end()) ? it->second->compiled : nullptr;
}

const std::vector<Diagnostic>& DocumentLoader::diagnosticsOf(const Rml::ElementDocument* document){
    auto it = bindings().find(document);
    return (it != bindings().end()) ? it->second->diagnostics : noDiagnostics();
}

void DocumentLoader::addRuntimeDiagnostic(const Rml::ElementDocument* document, const Diagnostic& diagnostic){
    auto it = bindings().find(document);
    if (it != bindings().end())
        it->second->diagnostics.push_back(diagnostic);
}

// ---- restyle ----

void DocumentLoader::restyle(Rml::ElementDocument* document, const CompatDocument& compiled, const CompatOptions&){
    if (!document)
        return;

    // the head's sheets in order, as ElementDocument::ProcessHeader combines them
    std::shared_ptr<Rml::StyleSheetContainer> combined;
    auto addFile = [&combined](const std::string& path){
        std::shared_ptr<Rml::StyleSheetContainer> sheet = Rml::Factory::InstanceStyleSheetFile("/" + VirtualFiles::normalize(path));
        if (!sheet)
            return;
        if (combined)
            combined->MergeStyleSheetContainer(*sheet);
        else
            combined = sheet->CombineStyleSheetContainer(Rml::StyleSheetContainer());
    };
    auto addInline = [&combined, &compiled](const std::string& rcss, uint32_t line){
        std::shared_ptr<Rml::StyleSheetContainer> sheet = inlineSheet(rcss, compiled.sourcePath, line);
        if (!sheet)
            return;
        if (combined)
            combined->MergeStyleSheetContainer(*sheet);
        else
            combined = sheet;
    };

    addFile(compiled.userAgentPath);
    if (!compiled.hints.rcss.empty())
        addInline(compiled.hints.rcss, compiled.hintsLine);
    for (size_t i = 0; i < compiled.stylesheets.size(); i++){
        const StylesheetOutput& sheet = compiled.stylesheets[i];
        if (sheet.kind == StylesheetInput::Kind::File)
            addFile(sheet.virtualPath);
        else
            addInline(sheet.rcss, i < compiled.stylesheetLines.size() ? compiled.stylesheetLines[i] : 1);
    }
    if (!compiled.important.rcss.empty())
        addInline(compiled.important.rcss, compiled.importantLine);

    if (combined)
        document->SetStyleSheetContainer(std::move(combined));
}

void DocumentLoader::restyle(Rml::ElementDocument* document, const std::shared_ptr<const CompatDocument>& compiled,
                             const CompatOptions& options){
    if (!document || !compiled)
        return;
    auto it = bindings().find(document);
    if (it != bindings().end()){
        std::shared_ptr<Binding> binding = it->second;
        unregisterDocument(binding->translatorHandle);
        binding->compiled = compiled;
        binding->translatorHandle = registerBinding(binding);
    }
    restyle(document, *compiled, options);
}

std::shared_ptr<const CompatDocument> DocumentLoader::restyleForViewport(Rml::ElementDocument* document,
                                                                          const std::shared_ptr<const CompatDocument>& compiled,
                                                                          const CompatOptions& options){
    if (!document || !compiled || !compiled->dependsOnViewport || compiled->options.viewport == options.viewport)
        return compiled;

    std::shared_ptr<const CompatDocument> fresh = compile(compiled->sourcePath, options);
    if (!fresh->compiled)
        return compiled;

    // RmlUi's messages about the new sheets map to the new compile
    restyle(document, fresh, options);

    // style="" values evaluated for the viewport (vmin, vmax): in place, by location
    std::unordered_map<uint32_t, const std::string*> previous;
    for (const auto& pair : compiled->styleAttributes)
        previous[pair.first] = &pair.second;
    for (const auto& pair : fresh->styleAttributes){
        auto found = previous.find(pair.first);
        if (found != previous.end() && *found->second == pair.second)
            continue;
        const std::string selector = std::string("[") + kLocationAttribute + "=\"" + std::to_string(pair.first) + "\"]";
        if (Rml::Element* element = document->QuerySelector(selector))
            element->SetAttribute("style", pair.second);
    }
    return fresh;
}

// ---- fragments ----

std::string DocumentLoader::fragmentToRml(std::string_view html, const std::string& chunkName, std::vector<Diagnostic>& diagnostics){
    FragmentOptions options;
    return fragmentToRml(html, chunkName, diagnostics, options);
}

std::string DocumentLoader::fragmentToRml(std::string_view html, const std::string& chunkName, std::vector<Diagnostic>& diagnostics,
                                          const FragmentOptions& options){
    SourceFiles files;
    const uint32_t file = files.add(chunkName, std::string(html));
    DiagnosticSink sink(files, options.compat.diagnosticLimit);
    UrlResolver urls;

    HtmlDocument parsed = HtmlParser::parseFragment(file, files.text(file), 0, &sink, options.contextTag);
    TransformContext context(options.compat, files, sink, urls);
    context.file = file;
    context.path = options.documentPath.empty() ? chunkName : normalizePath(options.documentPath);
    context.root = parsed.root.get();
    context.fragment = true;
    runTransforms(context);

    for (TemplateSource& source : context.templates){
        sink.report(codes::ElementMapped, source.ref,
                    "a <template> inserted at runtime has no template.content (templates are collected when the document loads)",
                    "put the template in the document", "", "template");
    }
    for (const StyleAttributeSource& attribute : context.styleAttributes){
        attribute.element->hasRewrittenStyle = true;
        attribute.element->rewrittenStyle = html::trim(CssRewriter::rewriteDeclarations(attribute.text, &sink, options.compat));
    }
    for (auto& pair : context.virtualFiles)
        VirtualFiles::add(pair.first, pair.second, 0);

    std::string rml = RmlSerializer::writeFragment(*parsed.root).rml;
    std::vector<Diagnostic> found = sink.take();
    diagnostics.insert(diagnostics.end(), found.begin(), found.end());
    return rml;
}

// ---- post-load ----

void DocumentLoader::applyAutofocus(Rml::ElementDocument* document){
    if (!document)
        return;
    if (Rml::Element* element = document->QuerySelector("[autofocus]"))
        element->Focus(true);
}

std::vector<Diagnostic> DocumentLoader::checkFontFamilies(const CompatDocument& compiled){
    std::vector<Diagnostic> diagnostics;
    std::vector<std::string> reported;
    auto declared = [&compiled](const std::string& family){
        for (const std::string& name : compiled.declaredFontFamilies){
            if (html::equalsIgnoreCase(html::trim(name), family))
                return true;
        }
        return false;
    };
    for (const std::string& list : compiled.fontFamilies){
        size_t start = 0;
        for (size_t i = 0; i <= list.size(); i++){
            if (i < list.size() && list[i] != ',')
                continue;
            std::string family = html::trim(list.substr(start, i - start));
            start = i + 1;
            if (family.size() >= 2 && (family.front() == '"' || family.front() == '\'') && family.back() == family.front())
                family = family.substr(1, family.size() - 2);
            family = html::trim(family);
            if (family.empty() || family.find("var(") != std::string::npos)
                continue;
            const std::string lower = html::toLower(family);
            if (lower == "inherit" || lower == "initial" || lower == "unset" || lower == "revert")
                continue;
            if (FontRegistry::isGenericFamily(family) || declared(family) || FontRegistry::hasFamily(family))
                continue;
            if (std::find(reported.begin(), reported.end(), lower) != reported.end())
                continue;
            reported.push_back(lower);
            Diagnostic diagnostic;
            diagnostic.code = codes::UnknownFontFamily;
            diagnostic.severity = Severity::Info;
            diagnostic.message = "font family '" + family + "' is not loaded and not generic; the next family of the list (or the default font) is used";
            diagnostic.suggestion = "declare it with @font-face, or import it (Google Fonts) with the editor";
            diagnostic.subject = family;
            diagnostic.file = compiled.sourcePath;
            diagnostics.push_back(diagnostic);
        }
    }
    return diagnostics;
}

bool DocumentLoader::isCompatDocument(const Rml::Element* element){
    if (!element)
        return false;
    const Rml::ElementDocument* document = const_cast<Rml::Element*>(element)->GetOwnerDocument();
    return document && document->GetId() == kRootId;
}
