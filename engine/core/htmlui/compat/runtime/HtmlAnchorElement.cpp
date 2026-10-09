// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "htmlui/compat/runtime/HtmlAnchorElement.h"

#include "htmlui/compat/UrlResolver.h"
#include "htmlui/compat/runtime/HtmlFormSupport.h"

#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Input.h>
#include <RmlUi/Core/StringUtilities.h>

#include <algorithm>
#include <cctype>

using namespace jasperengine::htmlui::compat;

namespace {

    // "scheme:" at the start (RFC 3986 scheme characters)
    size_t schemeLength(std::string_view href){
        if (href.empty() || !std::isalpha((unsigned char)href[0]))
            return 0;
        for (size_t i = 1; i < href.size(); i++){
            const char c = href[i];
            if (c == ':')
                return i;
            if (!std::isalnum((unsigned char)c) && c != '+' && c != '-' && c != '.')
                return 0;
        }
        return 0;
    }

    bool isDocumentPath(const std::string& path){
        std::string lower = path;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c){ return (char)std::tolower(c); });
        const auto ends = [&](const char* suffix){
            const size_t n = std::char_traits<char>::length(suffix);
            return lower.size() >= n && lower.compare(lower.size() - n, n, suffix) == 0;
        };
        return ends(".html") || ends(".htm");
    }

}

RMLUI_RTTI_Define(HtmlAnchorElement)

HtmlAnchorElement::HtmlAnchorElement(const Rml::String& tag): Rml::Element(tag) {}

HtmlAnchorElement::~HtmlAnchorElement() = default;

AnchorTarget HtmlAnchorElement::resolve(std::string_view hrefView, std::string_view documentPath){
    AnchorTarget target;
    const std::string href(hrefView);
    target.href = href;
    const size_t first = href.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return target;
    std::string value = href.substr(first, href.find_last_not_of(" \t\r\n") - first + 1);

    if (value[0] == '#'){
        target.kind = AnchorTarget::Kind::Fragment;
        target.fragment = UrlResolver::percentDecode(value.substr(1));
        return target;
    }

    std::string path;
    const size_t scheme = schemeLength(value);
    if (value.compare(0, 2, "//") == 0 || (scheme > 0 && value.compare(0, 8, "asset://") != 0)){
        // http(s), protocol-relative, mailto:, ...: outside the game
        target.kind = AnchorTarget::Kind::External;
        target.path = value;
        return target;
    }

    // the file and its fragment (a query has no meaning for asset files)
    const size_t hash = value.find('#');
    if (hash != std::string::npos){
        target.fragment = UrlResolver::percentDecode(value.substr(hash + 1));
        value = value.substr(0, hash);
    }
    const size_t query = value.find('?');
    if (query != std::string::npos)
        value = value.substr(0, query);

    if (value.compare(0, 8, "asset://") == 0)
        path = UrlResolver::normalizePath(value.substr(8));
    else if (!value.empty() && value[0] == '/')
        path = UrlResolver::normalizePath(value.substr(1));
    else if (value.empty())
        path = std::string(documentPath);
    else
        path = UrlResolver::joinPath(std::string(documentPath), UrlResolver::percentDecode(value));

    // a link into this very document is a fragment link
    if (path == documentPath && !target.fragment.empty()){
        target.kind = AnchorTarget::Kind::Fragment;
        return target;
    }
    target.path = path;
    target.kind = isDocumentPath(path) ? AnchorTarget::Kind::Document : AnchorTarget::Kind::Resource;
    return target;
}

AnchorTarget HtmlAnchorElement::getTarget(){
    if (!HasAttribute("href"))
        return AnchorTarget();
    std::string documentPath;
    if (Rml::ElementDocument* document = GetOwnerDocument())
        documentPath = Rml::StringUtilities::Replace(document->GetSourceURL(), '|', ':');
    return resolve(GetAttribute<Rml::String>("href", ""), documentPath);
}

void HtmlAnchorElement::follow(){
    const AnchorTarget target = getTarget();
    Rml::ElementDocument* document = GetOwnerDocument();
    if (target.kind == AnchorTarget::Kind::None || !document)
        return;

    if (target.kind == AnchorTarget::Kind::Fragment){
        if (target.fragment.empty() || target.fragment == "top"){
            document->SetScrollTop(0.0f);
            document->SetScrollLeft(0.0f);
            if (Rml::Element* body = document->GetNumChildren() > 0 ? document->GetChild(0) : nullptr)
                body->SetScrollTop(0.0f);
            return;
        }
        if (Rml::Element* element = document->GetElementById(target.fragment))
            element->ScrollIntoView(true);
        return;
    }

    Rml::Dictionary parameters;
    parameters["href"] = Rml::Variant(target.href);
    parameters["url"] = Rml::Variant(target.path);
    parameters["fragment"] = Rml::Variant(target.fragment);
    parameters["document"] = Rml::Variant(target.kind == AnchorTarget::Kind::Document);
    parameters["external"] = Rml::Variant(target.kind == AnchorTarget::Kind::External);
    document->DispatchEvent("htmlnavigate", parameters, true, true);
}

void HtmlAnchorElement::ProcessDefaultAction(Rml::Event& event){
    Rml::Element::ProcessDefaultAction(event);
    if (!HasAttribute("href"))
        return;
    if (event == Rml::EventId::Keydown){
        const auto key = (Rml::Input::KeyIdentifier)event.GetParameter<int>("key_identifier", 0);
        if (event.GetTargetElement() == this && (key == Rml::Input::KI_RETURN || key == Rml::Input::KI_NUMPADENTER))
            Click();
        return;
    }
    if (event == Rml::EventId::Click)
        follow();
}
