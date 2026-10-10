// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The document half of CompatApi.h (p2-html): checkDocument over DocumentLoader's compile,
// locateElement (data-htmlui-loc), the importers' reference discovery.

#include "htmlui/compat/CompatApi.h"
#include "htmlui/compat/LiteralCompare.h"

#include "htmlui/compat/DocumentLoader.h"
#include "htmlui/compat/RmlLogTranslator.h"
#include "htmlui/compat/SourceFiles.h"

#include "HtmlUI.h"

#include <RmlUi/Core.h>

#include <algorithm>
#include <cstdlib>
#include <set>

using namespace jasperengine;
using namespace jasperengine::htmlui::compat;

namespace {

    bool hasExtension(const std::string& path, const char* extension){
        const size_t length = std::char_traits<char>::length(extension);
        if (path.size() < length)
            return false;
        for (size_t i = 0; i < length; i++){
            char c = path[path.size() - length + i];
            if (c >= 'A' && c <= 'Z')
                c = (char)(c - 'A' + 'a');
            if (c != extension[i])
                return false;
        }
        return true;
    }

    // RmlUi stays initialised for the duration of a verification
    class RmlScope {
    public:
        RmlScope(): acquired(!HtmlUI::isInitialized() && HtmlUI::acquire()) {}
        ~RmlScope(){
            if (acquired)
                HtmlUI::release();
        }
        bool ready() const { return HtmlUI::isInitialized(); }

    private:
        bool acquired;
    };

    // a context nothing is drawn from: RmlUi parses and lays the RML out, its messages are
    // what a load in the engine would log
    class NullRenderInterface : public Rml::RenderInterface {
    public:
        Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex>, Rml::Span<const int>) override { return 1; }
        void RenderGeometry(Rml::CompiledGeometryHandle, Rml::Vector2f, Rml::TextureHandle) override {}
        void ReleaseGeometry(Rml::CompiledGeometryHandle) override {}
        Rml::TextureHandle LoadTexture(Rml::Vector2i& dimensions, const Rml::String&) override{
            dimensions = Rml::Vector2i(1, 1);
            return 1;
        }
        Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte>, Rml::Vector2i) override { return 1; }
        void ReleaseTexture(Rml::TextureHandle) override {}
        void EnableScissorRegion(bool) override {}
        void SetScissorRegion(Rml::Rectanglei) override {}
    };

    std::vector<RmlLogTranslator::LineMapping> mappingsOf(const CompatDocument& compiled){
        std::vector<RmlLogTranslator::LineMapping> mappings;
        for (size_t i = 0; i < compiled.stylesheets.size(); i++){
            const StylesheetOutput& sheet = compiled.stylesheets[i];
            if (sheet.kind == StylesheetInput::Kind::File)
                mappings.push_back(RmlLogTranslator::LineMapping{sheet.virtualPath, 1, &sheet.lines.lines});
            else if (i < compiled.stylesheetLines.size() && compiled.stylesheetLines[i] > 0)
                mappings.push_back(RmlLogTranslator::LineMapping{compiled.sourcePath, compiled.stylesheetLines[i], &sheet.lines.lines});
        }
        if (compiled.hintsLine > 0)
            mappings.push_back(RmlLogTranslator::LineMapping{compiled.sourcePath, compiled.hintsLine, &compiled.hints.lines.lines});
        if (compiled.importantLine > 0)
            mappings.push_back(RmlLogTranslator::LineMapping{compiled.sourcePath, compiled.importantLine, &compiled.important.lines.lines});
        mappings.push_back(RmlLogTranslator::LineMapping{compiled.sourcePath, 1, &compiled.sourceMap.lineRefs});
        return mappings;
    }

    // the data-model names of the RML: the check gives them empty models, as the engine does
    // before a script creates them (no "data model not found" errors)
    std::set<std::string> dataModelNames(const std::string& rml){
        std::set<std::string> names;
        const std::string attribute = "data-model=\"";
        for (size_t at = rml.find(attribute); at != std::string::npos; at = rml.find(attribute, at + 1)){
            const size_t begin = at + attribute.size();
            const size_t end = rml.find('"', begin);
            if (end != std::string::npos && end > begin)
                names.insert(rml.substr(begin, end - begin));
        }
        return names;
    }

    std::vector<Diagnostic> verifyWithRmlUi(const CompatDocument& compiled){
        std::vector<Diagnostic> diagnostics;
        if (Rml::GetSystemInterface() == nullptr)
            return diagnostics;

        NullRenderInterface renderInterface;
        static int counter = 0;
        const std::string name = "htmlui-check-" + std::to_string(++counter);

        RmlLogTranslator::Capture capture;
        Rml::Context* context = Rml::CreateContext(name, Rml::Vector2i(compiled.options.viewport.x, compiled.options.viewport.y), &renderInterface);
        if (!context)
            return diagnostics;
        context->SetDensityIndependentPixelRatio(compiled.options.dpRatio > 0.0f ? compiled.options.dpRatio : 1.0f);
        for (const std::string& model : dataModelNames(compiled.rml))
            context->CreateDataModel(model, nullptr, true);

        // fresh parses of the sheets (the factory caches them by path)
        Rml::Factory::ClearStyleSheetCache();
        if (Rml::ElementDocument* document = context->LoadDocumentFromMemory(compiled.rml, compiled.sourcePath)){
            document->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
            context->Update();
            context->UnloadDocument(document);
            context->Update();
        }
        Rml::RemoveContext(name);
        Rml::ReleaseTextures(&renderInterface);
        Rml::ReleaseRenderManagers();

        const std::vector<RmlLogTranslator::LineMapping> mappings = mappingsOf(compiled);
        for (const RmlLogTranslator::Capture::Message& message : capture.messages()){
            if (message.severity == Severity::Info)
                continue;
            std::optional<Diagnostic> mapped = RmlLogTranslator::translate(message.text, compiled.files,
                                                                           Span<const RmlLogTranslator::LineMapping>(mappings));
            if (!mapped){
                Diagnostic unmapped;
                unmapped.code = codes::RmluiMessage;
                unmapped.severity = Severity::Warning;
                unmapped.message = "RmlUi: " + message.text;
                unmapped.origin = "RmlUi";
                unmapped.file = compiled.sourcePath;
                mapped = unmapped;
            }
            diagnostics.push_back(std::move(*mapped));
        }
        return diagnostics;
    }

}

bool jasperengine::htmlui::compat::isHtmlDocumentPath(std::string_view path){
    std::string text(path);
    const size_t cut = text.find_first_of("?#");
    if (cut != std::string::npos)
        text = text.substr(0, cut);
    return hasExtension(text, ".html") || hasExtension(text, ".htm");
}

CheckReport jasperengine::htmlui::compat::checkDocument(const std::string& htmlPath, const CheckOptions& options){
    if (hasExtension(htmlPath, ".css") || hasExtension(htmlPath, ".rcss"))
        return checkStylesheet(htmlPath, options);

    CheckReport report;
    std::shared_ptr<const CompatDocument> compiled = DocumentLoader::compile(htmlPath, options.compat, options.files);
    report.diagnostics = compiled->diagnostics;
    report.remoteRefs = compiled->remoteRefs;
    for (const CompatDocument::DependencyStamp& stamp : compiled->dependencyStamps){
        // the files read (a missing document or sheet is not a file of the check)
        if (stamp.hash != 0 && std::find(report.files.begin(), report.files.end(), stamp.path) == report.files.end())
            report.files.push_back(stamp.path);
    }

    if (compiled->compiled && options.verifyWithRmlUi){
        RmlScope rml;
        if (rml.ready()){
            std::vector<Diagnostic> verified = verifyWithRmlUi(*compiled);
            report.diagnostics.insert(report.diagnostics.end(), verified.begin(), verified.end());
            std::vector<Diagnostic> fonts = DocumentLoader::checkFontFamilies(*compiled);
            report.diagnostics.insert(report.diagnostics.end(), fonts.begin(), fonts.end());
        }
    }

    sortDiagnostics(report.diagnostics);
    return report;
}

std::optional<Diagnostic> jasperengine::htmlui::compat::locateElement(const CompatDocument& document, const Rml::Element* element){
    if (!element)
        return std::nullopt;
    const Rml::Variant* attribute = element->GetAttribute(DocumentLoader::kLocationAttribute);
    if (!attribute)
        return std::nullopt;
    const std::string text = attribute->Get<Rml::String>();
    char* end = nullptr;
    const unsigned long index = std::strtoul(text.c_str(), &end, 10);
    if (text.empty() || end == text.c_str() || index >= document.sourceMap.elementRefs.size())
        return std::nullopt;
    const SourceRef ref = document.sourceMap.elementRefs[index];
    if (!ref.valid())
        return std::nullopt;
    Diagnostic location;
    DiagnosticSink sink(document.files);
    sink.locate(location, ref);
    return location;
}

std::vector<RemoteReference> jasperengine::htmlui::compat::collectRemoteReferences(const std::string& htmlPath, const FileAccess& files){
    CheckOptions options;
    options.verifyWithRmlUi = false;
    options.files = files;
    if (hasExtension(htmlPath, ".css") || hasExtension(htmlPath, ".rcss"))
        return checkStylesheet(htmlPath, options).remoteRefs;
    return checkDocument(htmlPath, options).remoteRefs;
}
