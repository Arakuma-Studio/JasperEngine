// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "htmlui/widgets/MarkdownElement.h"

#include "htmlui/widgets/Markdown/MarkdownHtml.h"
#include "htmlui/widgets/WidgetsRuntime.h"
#include "htmlui/compat/DocumentLoader.h"
#include "htmlui/compat/SourceFiles.h"
#include "htmlui/compat/UrlResolver.h"
#include "htmlui/compat/VirtualFiles.h"
#include "HtmlUIDependencies.h"

#include "Log.h"

#include "../../../libs/rmlui/Source/Core/DataExpression.h"
#include "../../../libs/rmlui/Source/Core/DataView.h"
#include "../../../libs/rmlui/Source/Core/DataViewDefault.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Factory.h>

#include <memory>

using namespace jasperengine;
using namespace jasperengine::htmlui;

RMLUI_RTTI_Define(MarkdownElement)

namespace {

    // the compile rendered the element (compat ElementTransform)
    constexpr const char* kCompiledAttribute = "data-htmlui-markdown";

    // data-markdown="expr": the expression's text, rendered as Markdown into the element
    class DataViewMarkdown final : public Rml::DataViewCommon {
    public:
        explicit DataViewMarkdown(Rml::Element* element): Rml::DataViewCommon(element) {}

        bool Update(Rml::DataModel& model) override{
            Rml::Element* element = GetElement();
            if (!element)
                return false;
            Rml::Variant value;
            Rml::DataExpressionInterface expression(&model, element);
            if (!GetExpression().Run(expression, value))
                return false;
            Rml::String markdown = value.Get<Rml::String>();
            if (rendered && markdown == previous)
                return false;
            MarkdownElement::setMarkdown(element, markdown);
            previous = std::move(markdown);
            rendered = true;
            return true;
        }

    private:
        Rml::String previous;
        bool rendered = false;
    };

    struct Instancers {
        MarkdownInstancer element;
        Rml::DataViewInstancerDefault<DataViewMarkdown> view;
    };

    Instancers& instancers(){
        static Instancers value;
        return value;
    }

    // what an <img src> written in the element's document resolves to
    std::string resolveImage(Rml::Element* element, const std::string& url){
        Rml::ElementDocument* document = element ? element->GetOwnerDocument() : nullptr;
        std::shared_ptr<const compat::CompatDocument> compiled = compat::DocumentLoader::compiledOf(document);
        if (!compiled)
            return url;   // RmlUi resolves it against the .rml document
        return compat::UrlResolver().resolve(url, compiled->sourcePath, "image").emitted;
    }

}

MarkdownElement::MarkdownElement(const Rml::String& tag): Rml::Element(tag){
}

std::string MarkdownElement::render(Rml::Element* element, std::string_view markdown, bool* rawHtml){
    markdown::RenderOptions options;
    options.resolveUrl = [element](const std::string& url, bool image) -> std::string {
        return image ? resolveImage(element, url) : url;
    };
    markdown::RenderResult result = markdown::renderToRml(markdown, options);
    if (rawHtml)
        *rawHtml = result.rawHtml;
    return std::move(result.rml);
}

void MarkdownElement::setMarkdown(Rml::Element* element, std::string_view markdown){
    if (!element)
        return;
    element->SetInnerRML(render(element, markdown));
}

void MarkdownElement::OnAttributeChange(const Rml::ElementAttributes& changed){
    Rml::Element::OnAttributeChange(changed);
    if (!changed.count("src"))
        return;
    // the compile rendered the src it was loaded with; a src set later (or an .rml document's)
    // is loaded at the next update, when the element knows its document
    if (HasAttribute(kCompiledAttribute) && !GetOwnerDocument())
        return;
    loadPending = true;
    if (Rml::Context* context = GetContext())
        context->RequestNextUpdate(0);
}

void MarkdownElement::OnUpdate(){
    Rml::Element::OnUpdate();
    if (loadPending){
        loadPending = false;
        loadSource();
    }
}

void MarkdownElement::loadSource(){
    const std::string src = GetAttribute<Rml::String>("src", "");
    if (src.empty())
        return;
    Rml::ElementDocument* document = GetOwnerDocument();
    std::shared_ptr<const compat::CompatDocument> compiled = compat::DocumentLoader::compiledOf(document);
    std::string path;
    if (compiled){
        const compat::ResolvedUrl resolved = compat::UrlResolver().resolve(src, compiled->sourcePath, "markdown");
        if (!resolved.local())
            return;
        path = resolved.assetPath;
    }else{
        path = resolveHtmlUIPath(document ? document->GetSourceURL() : std::string(), src);
    }
    std::string markdown;
    if (!compat::readAssetFile(compat::FileAccess(), path, markdown)){
        Log::warn("[HTML UI] <markdown src=\"%s\">: '%s' could not be read", src.c_str(), path.c_str());
        return;
    }
    compat::stripUtf8Bom(markdown);
    HtmlUIFileLog::recordFile(path);
    setMarkdown(this, markdown);
}

void MarkdownElement::registerInstancers(){
    Rml::Factory::RegisterElementInstancer("markdown", &instancers().element);
    Rml::Factory::RegisterDataViewInstancer(&instancers().view, "markdown", false);
}

Rml::ElementPtr MarkdownInstancer::InstanceElement(Rml::Element*, const Rml::String& tag, const Rml::XMLAttributes&){
    return Rml::ElementPtr(new MarkdownElement(tag));
}

void MarkdownInstancer::ReleaseElement(Rml::Element* element){
    delete element;
}
