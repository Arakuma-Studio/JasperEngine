// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "HtmlL10n.h"

#include "Log.h"
#include "Scene.h"
#include "subsystem/UIDocumentSystem.h"
#include "util/FontRegistry.h"

#include "json.hpp"

#include <RmlUi/Core.h>

#include <algorithm>
#include <map>
#include <unordered_set>

using namespace jasperengine;
using namespace jasperengine::htmlui;

namespace {

    constexpr const char* kIdAttribute = "data-l10n-id";
    constexpr const char* kArgsAttribute = "data-l10n-args";
    constexpr const char* kAttrPrefix = "data-l10n-attr-";

    bool isKeyed(const Rml::Element* element) {
        for (const auto& attribute : element->GetAttributes()) {
            if (attribute.first.compare(0, 10, "data-l10n-") == 0 && attribute.first != kArgsAttribute) return true;
        }
        return false;
    }

    class L10nPlugin final : public Rml::Plugin {
    public:
        int GetEventClasses() override { return EVT_DOCUMENT | EVT_ELEMENT; }
        void OnElementCreate(Rml::Element* element) override {
            if (!Localization::isActive() || !isKeyed(element)) return;
            pending.push_back(element);
            keyed.insert(element);
        }
        void OnElementDestroy(Rml::Element* element) override {
            if (keyed.empty() || !keyed.erase(element)) return;
            pending.erase(std::remove(pending.begin(), pending.end(), element), pending.end());
            warned.erase(element);
        }
        void OnDocumentLoad(Rml::ElementDocument*) override { HtmlL10n::update(); }
        void OnDocumentUnload(Rml::ElementDocument* document) override { documents.erase(document); }

        std::vector<Rml::Element*> pending;
        std::unordered_set<Rml::Element*> keyed;
        std::unordered_set<Rml::Element*> warned;
        std::unordered_set<Rml::ElementDocument*> documents;
    };

    struct State {
        L10nPlugin plugin;
        bool installed = false;
        int listener = -1;
        int scanner = -1;
    };

    State& state() {
        static State value;
        return value;
    }

    LocalizationArgs argsOf(const Rml::Element* element) {
        LocalizationArgs args;
        const Rml::Variant* value = element->GetAttribute(kArgsAttribute);
        if (!value) return args;
        const nlohmann::json json = nlohmann::json::parse(value->Get<Rml::String>(), nullptr, false);
        if (!json.is_object()) return args;
        for (auto it = json.begin(); it != json.end(); ++it) {
            if (it->is_string()) args.emplace_back(it.key(), it->get<std::string>());
            else if (it->is_number_integer()) args.emplace_back(it.key(), it->get<int64_t>());
            else if (it->is_number()) args.emplace_back(it.key(), it->get<double>());
            else if (it->is_boolean()) args.emplace_back(it.key(), std::string(it->get<bool>() ? "true" : "false"));
        }
        return args;
    }

    bool hasChildElements(Rml::Element* element) {
        for (int i = 0; i < element->GetNumChildren(); i++) {
            if (!rmlui_dynamic_cast<Rml::ElementText*>(element->GetChild(i))) return true;
        }
        return false;
    }

    std::string textOf(Rml::Element* element) {
        std::string text;
        for (int i = 0; i < element->GetNumChildren(); i++) {
            if (auto* child = rmlui_dynamic_cast<Rml::ElementText*>(element->GetChild(i))) text += child->GetText();
        }
        return text;
    }

    void applyRoot(Rml::ElementDocument* document) {
        const std::string& language = Localization::getLanguage();
        if (language.empty()) return;
        document->SetProperty("-rmlui-language", language);
        // R12: right-to-left languages turn the page; a left-to-right one leaves the author's dir
        if (Localization::isRightToLeft(language)) document->SetProperty("-rmlui-direction", "rtl");
        else document->RemoveProperty("-rmlui-direction");
    }

    void translateElement(Rml::Element* element) {
        State& s = state();
        const std::string ref = HtmlL10n::refOf(element);
        const Rml::String key = element->GetAttribute<Rml::String>(kIdAttribute, "");
        if (!key.empty()) {
            if (hasChildElements(element)) {
                if (s.plugin.warned.insert(element).second) {
                    Log::warn("[HTML UI] HTMLUI-LOC-0001: %s has data-l10n-id=\"%s\" and child elements: left as written", ref.c_str(), key.c_str());
                }
            } else {
                std::string text;
                if (Localization::translate(key, argsOf(element), text, ref) && text != textOf(element)) {
                    element->SetInnerRML(Rml::StringUtilities::EncodeRml(text));
                }
            }
        }
        for (const auto& attribute : element->GetAttributes()) {
            if (attribute.first.compare(0, 15, kAttrPrefix) != 0 || attribute.first.size() == 15) continue;
            std::string text;
            if (Localization::translate(attribute.second.Get<Rml::String>(), argsOf(element), text, ref)) {
                const std::string name = attribute.first.substr(15);
                if (element->GetAttribute<Rml::String>(name, "") != text) element->SetAttribute(name, text);
            }
        }
        if (Rml::ElementDocument* document = element->GetOwnerDocument()) {
            if (s.plugin.documents.insert(document).second) applyRoot(document);
        }
    }

    // R14: localization.fonts as the language faces of the font registry (once per manifest)
    void registerLanguageFonts() {
        static std::map<std::string, std::vector<std::string>> registered;
        const auto& fonts = Localization::getManifest().fonts;
        if (fonts == registered) return;
        registered = fonts;
        FontRegistry::clearLanguageFaces();
        for (const auto& [language, paths] : fonts) {
            for (const std::string& path : paths) {
                std::string error;
                if (std::shared_ptr<FontFace> face = FontRegistry::getOrLoadFile(path, 0, &error)) FontRegistry::registerLanguageFace(language, face);
                else Log::warn("[HTML UI] the %s font %s does not load: %s", language.c_str(), path.c_str(), error.c_str());
            }
        }
    }

    void translateAll() {
        registerLanguageFonts();
        State& s = state();
        s.plugin.pending.clear();
        const std::vector<Rml::Element*> elements(s.plugin.keyed.begin(), s.plugin.keyed.end());
        for (Rml::Element* element : elements) if (s.plugin.keyed.count(element)) translateElement(element);
        for (Rml::ElementDocument* document : s.plugin.documents) applyRoot(document);
    }

    bool hasLetter(const std::string& text) {
        return std::any_of(text.begin(), text.end(), [](char c) {
            const unsigned char u = static_cast<unsigned char>(c);
            return u >= 0x80 || (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z');
        });
    }

    // literal text: text nodes with a letter, outside keyed elements and translate="no"
    void collectLiteral(Rml::Element* element, bool skipped, std::vector<std::pair<std::string, std::string>>& out) {
        if (auto* text = rmlui_dynamic_cast<Rml::ElementText*>(element)) {
            const std::string value = text->GetText();
            if (!skipped && hasLetter(value) && !Localization::wasProduced(value)) {
                Rml::Element* parent = element->GetParentNode();
                out.emplace_back(parent ? HtmlL10n::refOf(parent) : std::string(), value);
            }
            return;
        }
        const bool skip = skipped || element->HasAttribute(kIdAttribute) ||
                          element->GetAttribute<Rml::String>("translate", "") == "no" || element->GetTagName() == "script" ||
                          element->GetTagName() == "style";
        for (int i = 0; i < element->GetNumChildren(); i++) collectLiteral(element->GetChild(i), skip, out);
    }

}

void HtmlL10n::install() {
    State& s = state();
    if (s.installed) return;
    Rml::RegisterPlugin(&s.plugin);
    s.listener = Localization::addListener([](const std::string&) { translateAll(); });
    s.scanner = Localization::addScanner([](Localization::ScanReport& report) {
        std::unordered_set<Rml::ElementDocument*> documents;
        for (Rml::Element* element : state().plugin.keyed) {
            if (Rml::ElementDocument* document = element->GetOwnerDocument()) documents.insert(document);
        }
        for (Rml::ElementDocument* document : documents) {
            for (const auto& [where, text] : HtmlL10n::report(document).literal) report.literal.push_back({where, text});
        }
    });
    s.installed = true;
}

void HtmlL10n::uninstall() {
    State& s = state();
    if (!s.installed) return;
    Localization::removeListener(s.listener);
    Localization::removeScanner(s.scanner);
    s.plugin.pending.clear();
    s.plugin.keyed.clear();
    s.plugin.warned.clear();
    s.plugin.documents.clear();
    s.installed = false;
}

void HtmlL10n::update() {
    State& s = state();
    if (s.plugin.pending.empty()) return;
    // translating may make elements (a text node): take the list first
    std::vector<Rml::Element*> elements;
    elements.swap(s.plugin.pending);
    for (Rml::Element* element : elements) if (s.plugin.keyed.count(element)) translateElement(element);
}

void HtmlL10n::translateDocument(Rml::ElementDocument* document) {
    if (!document || !Localization::isActive()) return;
    State& s = state();
    Rml::ElementList found;
    document->QuerySelectorAll(found, "*");
    for (Rml::Element* element : found) {
        if (isKeyed(element)) s.plugin.keyed.insert(element);
    }
    for (Rml::Element* element : found) if (s.plugin.keyed.count(element)) translateElement(element);
    applyRoot(document);
}

void HtmlL10n::setLocalized(Rml::Element* element, const std::string& key, const std::string& argsJson) {
    if (!element) return;
    element->SetAttribute(kIdAttribute, key);
    if (argsJson.empty()) element->RemoveAttribute(kArgsAttribute);
    else element->SetAttribute(kArgsAttribute, argsJson);
    state().plugin.keyed.insert(element);
    translateElement(element);
}

HtmlL10n::DocumentReport HtmlL10n::report(Rml::ElementDocument* document) {
    DocumentReport out;
    out.language = Localization::getLanguage();
    if (!document) return out;
    std::vector<std::pair<std::string, std::string>> keyed;     // {key, ref}
    for (Rml::Element* element : state().plugin.keyed) {
        if (element->GetOwnerDocument() != document) continue;
        out.keyed++;
        const std::string ref = refOf(element);
        keyed.emplace_back(element->GetAttribute<Rml::String>(kIdAttribute, ""), ref);
        for (const auto& attribute : element->GetAttributes()) {
            if (attribute.first.compare(0, 15, kAttrPrefix) == 0) keyed.emplace_back(attribute.second.Get<Rml::String>(), ref);
        }
    }
    for (const Localization::Miss& miss : Localization::getMisses()) {
        if (std::any_of(keyed.begin(), keyed.end(), [&](const auto& k) { return k.first == miss.key && k.second == miss.where; })) {
            out.missing.push_back(miss);
        }
    }
    collectLiteral(document, false, out.literal);
    return out;
}

HtmlL10n::DocumentReport HtmlL10n::reportOf(Scene* scene, Entity entity) {
    Rml::ElementDocument* document = scene ? scene->getSystem<UIDocumentSystem>()->getDocument(entity) : nullptr;
    if (document && Localization::isActive()) translateDocument(document);
    return report(document);
}

std::string HtmlL10n::refOf(Rml::Element* element) {
    std::vector<std::string> steps;
    for (Rml::Element* walk = element; walk; walk = walk->GetParentNode()) {
        if (!walk->GetId().empty()) {
            steps.push_back("#" + walk->GetId());
            break;
        }
        Rml::Element* parent = walk->GetParentNode();
        if (!parent || rmlui_dynamic_cast<Rml::ElementDocument*>(walk)) {
            steps.push_back("html");
            break;
        }
        int index = 1;
        for (int i = 0; i < parent->GetNumChildren(); i++) {
            Rml::Element* sibling = parent->GetChild(i);
            if (sibling == walk) break;
            if (!rmlui_dynamic_cast<Rml::ElementText*>(sibling)) index++;
        }
        steps.push_back(walk->GetTagName() + ":nth-child(" + std::to_string(index) + ")");
    }
    std::string ref;
    for (auto it = steps.rbegin(); it != steps.rend(); ++it) ref += (ref.empty() ? "" : " > ") + *it;
    return ref;
}
