// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The live tree patch of HTML documents (S17 R27-R31): a hot reload reconciles the live document
// with the new compile instead of loading it again. compat::reconcileTrees diffs the compile the
// document came from against the new one; this applies the plan to the live elements, found by
// their data-htmlui-loc among the authored ones (WidgetsRuntime::isAuthored: made by the
// document's parse or by an earlier patch, not by data-for, the virtual views, cloneNode or a
// script). Kept elements keep their identity, listeners, Lua handles, form values, scroll, focus
// and animations; what a script changed stays unless the author changed the same thing; nodes
// scripts added stay after the authored node they followed. Then every element's
// data-htmlui-loc is renumbered, the document is bound to the new compile and restyled, the
// scripts whose text changed run again, and onReload fires.

#include "UIDocumentSystem.h"

#include "Scene.h"
#include "Log.h"
#include "object/ui/HtmlUIEventListener.h"
#include "object/ui/HtmlUIScriptInternal.h"
#include "htmlui/HtmlUIContext.h"
#include "htmlui/compat/Diagnostics.h"
#include "htmlui/compat/DocumentLoader.h"
#include "htmlui/compat/TreeReconcile.h"
#include "htmlui/widgets/ItemsViewElement.h"
#include "htmlui/widgets/WidgetsRuntime.h"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/ElementText.h>
#include <RmlUi/Core/Factory.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <unordered_set>

using namespace jasperengine;
using namespace jasperengine::htmlui;
using namespace jasperengine::htmlui::compat;

namespace {

    constexpr const char* kLoc = DocumentLoader::kLocationAttribute;

    int locOf(const Rml::Element* element){
        const Rml::Variant* value = element->GetAttribute(kLoc);
        return value ? std::atoi(value->Get<Rml::String>().c_str()) : -1;
    }

    bool isText(const Rml::Element* element){
        return rmlui_dynamic_cast<const Rml::ElementText*>(element) != nullptr;
    }

    class Patcher {
    public:
        Patcher(Rml::ElementDocument* document, const ReconcilePlan& plan): document(document), plan(plan) {}

        // the authored element of every old loc (the first, for a duplicated one)
        void index(Rml::Element* element){
            if (WidgetsRuntime::isAuthored(element)){
                const int loc = locOf(element);
                if (loc >= 0)
                    live.emplace(loc, element);
            }
            for (int i = 0; i < element->GetNumChildren(); i++)
                index(element->GetChild(i));
        }

        // every data-htmlui-loc of the document to the new compile's numbering (rows, data-for
        // copies and script clones too: their numbers are their templates')
        void renumber(Rml::Element* element){
            const int loc = locOf(element);
            if (loc >= 0){
                auto it = plan.locs.find(loc);
                if (it != plan.locs.end() && it->second != loc)
                    element->SetAttribute(kLoc, std::to_string(it->second));
            }
            for (int i = 0; i < element->GetNumChildren(); i++)
                renumber(element->GetChild(i));
        }

        void apply(const ReconcilePlan::Element& step, Rml::Element* element){
            if (step.replace){
                replaceElement(element, step.node->rml);
                return;
            }
            for (const auto& [name, value] : step.setAttributes)
                element->SetAttribute(name, value);
            for (const std::string& name : step.removeAttributes)
                element->RemoveAttribute(name);
            for (const std::string& name : step.addClasses)
                element->SetClass(name, true);
            for (const std::string& name : step.removeClasses)
                element->SetClass(name, false);
            for (const auto& [name, value] : step.setStyles){
                if (value.empty() || !element->SetProperty(name, value))
                    element->RemoveProperty(name);
            }
            if (step.content){
                element->SetInnerRML(step.contentRml);
                markChildren(element);
            }
            if (step.leaf)
                return;

            // the authored texts of the element, in order (what the plan's text indices count)
            std::vector<Rml::Element*> texts;
            for (int i = 0; i < element->GetNumChildren(); i++){
                Rml::Element* child = element->GetChild(i);
                if (isText(child) && WidgetsRuntime::isAuthored(child))
                    texts.push_back(child);
            }
            countRuntime(element);

            // what goes first: the removed children (before inserts shift anything)
            std::vector<Rml::Element*> removals;
            for (const ReconcilePlan::Removed& removed : step.removedChildren){
                if (removed.text){
                    if (removed.textIndex >= 0 && removed.textIndex < (int)texts.size())
                        removals.push_back(texts[(size_t)removed.textIndex]);
                }else if (Rml::Element* child = liveOf(removed.loc, element)){
                    removals.push_back(child);
                }
            }
            for (Rml::Element* child : removals)
                removeElement(child);

            Rml::Element* cursor = nullptr;   // the last authored child placed
            for (const ReconcilePlan::Child& child : step.children){
                switch (child.kind){
                    case ReconcilePlan::Child::Kind::Insert:
                        cursor = insertAfter(element, cursor, child.node->rml);
                        break;
                    case ReconcilePlan::Child::Kind::Text:{
                        Rml::Element* text = (child.textIndex >= 0 && child.textIndex < (int)texts.size()) ? texts[(size_t)child.textIndex] : nullptr;
                        if (!text || text->GetParentNode() != element){
                            // a script took it out (textContent): it comes back only when the author changed it
                            if (child.textChanged)
                                cursor = insertAfter(element, cursor, child.node->rml);
                            break;
                        }
                        place(element, text, cursor);
                        if (child.textChanged)
                            static_cast<Rml::ElementText*>(text)->SetText(child.text);
                        cursor = text;
                        break;
                    }
                    case ReconcilePlan::Child::Kind::Element:{
                        const ReconcilePlan::Element& sub = child.element.front();
                        Rml::Element* match = liveOf(sub.oldLoc, element);
                        if (!match){
                            // a script took it out: the author's element comes back when the author changed it
                            if (touched(sub))
                                cursor = insertAfter(element, cursor, sub.node->rml);
                            break;
                        }
                        place(element, match, cursor);
                        Rml::Element* next = match;
                        if (sub.replace)
                            next = replaceElement(match, sub.node->rml);
                        else
                            apply(sub, match);
                        cursor = next;
                        break;
                    }
                }
            }
        }

        int runtimeKept = 0;

    private:
        // the author changed something in the element or below it
        static bool touched(const ReconcilePlan::Element& step){
            if (step.changed() || !step.removedChildren.empty())
                return true;
            for (const ReconcilePlan::Child& child : step.children){
                if (child.kind == ReconcilePlan::Child::Kind::Insert || (child.kind == ReconcilePlan::Child::Kind::Text && child.textChanged) ||
                    (child.kind == ReconcilePlan::Child::Kind::Element && touched(child.element.front())))
                    return true;
            }
            return false;
        }

        Rml::Element* liveOf(int loc, Rml::Element* parent) const{
            auto it = live.find(loc);
            return (it != live.end() && it->second->GetParentNode() == parent) ? it->second : nullptr;
        }

        // the nodes scripts added under an authored element (engine-made ones aside: rows, data-for
        // copies and the like carry a data-htmlui-loc, the views' parts an htmlui- class)
        void countRuntime(Rml::Element* element){
            for (int i = 0; i < element->GetNumChildren(); i++){
                Rml::Element* child = element->GetChild(i);
                if (WidgetsRuntime::isAuthored(child) || isText(child) || child->HasAttribute(kLoc))
                    continue;
                const Rml::String classes = child->GetAttribute<Rml::String>("class", "");
                if (classes.rfind("htmlui-", 0) == 0 || classes.find(" htmlui-") != Rml::String::npos)
                    continue;
                runtimeKept++;
            }
        }

        // the node before which the authored node after `cursor` goes: past the nodes that are not
        // authored after `cursor` (or at the start): they stay after the authored node they followed
        // (a data-for's copies before their template)
        Rml::Element* insertionPoint(Rml::Element* parent, Rml::Element* cursor) const{
            Rml::Element* next = cursor ? cursor->GetNextSibling() : (parent->GetNumChildren() > 0 ? parent->GetChild(0) : nullptr);
            while (next && !WidgetsRuntime::isAuthored(next))
                next = next->GetNextSibling();
            return next;
        }

        // an authored node into its place after `cursor`, when it is not there
        void place(Rml::Element* parent, Rml::Element* node, Rml::Element* cursor){
            Rml::Element* point = insertionPoint(parent, cursor);
            if (point == node)
                return;
            Rml::ElementPtr owned = parent->RemoveChild(node);
            if (!owned)
                return;
            if (point)
                parent->InsertBefore(std::move(owned), point);
            else
                parent->AppendChild(std::move(owned));
        }

        // the new nodes of `rml` before `point` (appended when null); the last of them
        Rml::Element* insertBefore(Rml::Element* parent, Rml::Element* point, const std::string& rml){
            if (rml.empty())
                return nullptr;
            // parsed in place (the data model and the document are there for the new elements),
            // then moved out of the holder
            Rml::XMLAttributes attributes;
            Rml::ElementPtr created = Rml::Factory::InstanceElement(parent, "div", "div", attributes);
            if (!created)
                return nullptr;
            Rml::Element* holder = point ? parent->InsertBefore(std::move(created), point) : parent->AppendChild(std::move(created));
            Rml::Factory::InstanceElementText(holder, rml);
            Rml::Element* last = nullptr;
            while (holder->GetNumChildren() > 0){
                Rml::ElementPtr child = holder->RemoveChild(holder->GetChild(0));
                last = parent->InsertBefore(std::move(child), holder);
                WidgetsRuntime::markAuthored(last);
            }
            parent->RemoveChild(holder);
            return last;
        }

        Rml::Element* insertAfter(Rml::Element* parent, Rml::Element* cursor, const std::string& rml){
            Rml::Element* last = insertBefore(parent, insertionPoint(parent, cursor), rml);
            return last ? last : cursor;
        }

        // the copies a data-for made of `element` (before it, same tag and number, not authored)
        static std::vector<Rml::Element*> dataForCopies(Rml::Element* element){
            std::vector<Rml::Element*> copies;
            if (!element->HasAttribute("data-for"))
                return copies;
            const int loc = locOf(element);
            for (Rml::Element* each = element->GetPreviousSibling(); each; each = each->GetPreviousSibling()){
                if (WidgetsRuntime::isAuthored(each) || each->GetTagName() != element->GetTagName() || locOf(each) != loc)
                    break;
                copies.push_back(each);
            }
            return copies;
        }

        void removeElement(Rml::Element* element){
            Rml::Element* parent = element->GetParentNode();
            if (!parent)
                return;
            for (Rml::Element* copy : dataForCopies(element))
                parent->RemoveChild(copy);
            parent->RemoveChild(element);
        }

        Rml::Element* replaceElement(Rml::Element* element, const std::string& rml){
            Rml::Element* parent = element->GetParentNode();
            if (!parent)
                return nullptr;
            for (Rml::Element* copy : dataForCopies(element))
                parent->RemoveChild(copy);
            Rml::Element* last = insertBefore(parent, element, rml);
            parent->RemoveChild(element);
            return last;
        }

        void markChildren(Rml::Element* element){
            for (int i = 0; i < element->GetNumChildren(); i++)
                WidgetsRuntime::markAuthored(element->GetChild(i));
        }

        Rml::ElementDocument* document;
        const ReconcilePlan& plan;
        std::unordered_map<int, Rml::Element*> live;
    };

    // the virtual views read their templates again (the content of a changed one makes their rows anew)
    void refreshViews(Rml::Element* element, const ReconcilePlan& plan){
        if (auto* view = rmlui_dynamic_cast<ItemsViewElement*>(element)){
            view->onDocumentPatched(plan.changedTemplates);
            return;
        }
        for (int i = 0; i < element->GetNumChildren(); i++)
            refreshViews(element->GetChild(i), plan);
    }

    // the two compiles give the document the same style sheets (an edit of the markup only): the
    // live document keeps its sheets and its computed styles
    bool sameSheets(const CompatDocument& a, const CompatDocument& b){
        if (a.userAgentPath != b.userAgentPath || a.hints.rcss != b.hints.rcss || a.important.rcss != b.important.rcss ||
            a.stylesheets.size() != b.stylesheets.size())
            return false;
        for (size_t i = 0; i < a.stylesheets.size(); i++){
            const StylesheetOutput& x = a.stylesheets[i];
            const StylesheetOutput& y = b.stylesheets[i];
            // (a file's virtual path names its compile: the same file, the same RCSS)
            if (x.kind != y.kind || x.sourcePath != y.sourcePath || x.rcss != y.rcss)
                return false;
        }
        return true;
    }

    // JASPER_HTMLUI_PATCH_PROFILE=1: the time of each step of a patch on the log
    struct PatchProfile {
        bool on = std::getenv("JASPER_HTMLUI_PATCH_PROFILE") != nullptr;
        std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
        std::string text;
        void step(const char* name){
            if (!on)
                return;
            const auto now = std::chrono::steady_clock::now();
            char buffer[64];
            std::snprintf(buffer, sizeof(buffer), " %s %.2f", name, std::chrono::duration<double, std::milli>(now - last).count());
            text += buffer;
            last = now;
        }
    };

    bool parseTemplates(const CompatDocument& compiled, std::vector<std::vector<RmlNode>>& out, std::string& error){
        out.clear();
        for (const HtmlTemplate& entry : compiled.templates){
            out.emplace_back();
            if (!parseRmlFragment(entry.rml, out.back(), &error))
                return false;
        }
        return true;
    }

}

bool UIDocumentSystem::patchDocument(Entity entity, UIReloadReport& report){
    Rml::ElementDocument* document = getDocument(entity);
    std::shared_ptr<const CompatDocument> compiled = DocumentLoader::compiledOf(document);
    if (!document || !compiled){
        report.fallbackReason = "not an HTML document";
        return false;
    }
    std::shared_ptr<const CompatDocument> fresh = DocumentLoader::compile(compiled->sourcePath, compiled->options);
    if (!fresh->compiled){
        // a missing file unloads the document, as before (the rebuild)
        report.fallbackReason = "the file could not be read";
        return false;
    }
    logDiagnostics(fresh->sourcePath, fresh->diagnostics, fresh->options.verboseDiagnostics);

    // the trees (parsed back from the RML both compiles wrote)
    RmlNode oldRoot, newRoot;
    std::vector<std::vector<RmlNode>> oldTemplates, newTemplates;
    std::string error;
    if (!parseRmlElement(compiled->rml, "body", oldRoot, &error) || !parseRmlElement(fresh->rml, "body", newRoot, &error) ||
        !parseTemplates(*compiled, oldTemplates, error) || !parseTemplates(*fresh, newTemplates, error)){
        report.fallbackReason = "the compiled RML could not be read: " + error;
        return false;
    }

    const auto start = std::chrono::steady_clock::now();
    PatchProfile profile;
    ReconcilePlan plan = reconcileTrees(oldRoot, newRoot, oldTemplates, newTemplates);
    profile.step("reconcile");
    if (plan.root.replace){
        report.fallbackReason = "the document root changed (" + plan.root.reason + ")";
        return false;
    }

    report.mode = "patch";
    report.file = compiled->sourcePath;
    const bool patched = reloadInPlace(entity, [&](){
        Patcher patcher(document, plan);
        patcher.index(document);
        patcher.renumber(document);
        profile.step("index");
        patcher.apply(plan.root, document);
        report.runtimeKept = patcher.runtimeKept;
        profile.step("apply");
        // bound to the new compile, its sheets (the inline-style rules are by the new numbers);
        // the same sheets stay
        const bool sheets = !sameSheets(*compiled, *fresh);
        DocumentLoader::rebind(document, fresh, fresh->options, sheets);
        profile.step(sheets ? "restyle" : "rebind");
        refreshViews(document, plan);
        profile.step("views");
        return true;
    });
    if (!patched){
        report.fallbackReason = "the document is gone";
        return false;
    }

    // R30: the scripts whose text changed run again (their listeners first removed), a deleted
    // one loses its listeners; the others keep theirs and do not run
    if (compatScriptsRun()){
        const size_t count = std::max(compiled->scripts.size(), fresh->scripts.size());
        for (size_t index = 0; index < count; index++){
            const bool same = index < compiled->scripts.size() && index < fresh->scripts.size() &&
                              compiled->scripts[index].code == fresh->scripts[index].code;
            if (same)
                continue;
            for (HtmlUIEventListener* listener : HtmlUIEventListener::listenersOfScript(document, (int)index))
                listener->remove();
            if (index < fresh->scripts.size()){
                compatRunScript(entity, document, *fresh, index);
                report.scriptsRerun++;
            }
        }
    }

    report.kept = plan.kept;
    report.updated = plan.updated;
    report.inserted = plan.inserted;
    report.removed = plan.removed;
    report.replaced = plan.replaced;
    report.causes = plan.causes;
    report.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    profile.step("scripts");
    if (profile.on)
        Log::print("[HTML UI] patch profile (ms):%s", profile.text.c_str());
    finishReload(entity, report);

    documentContext(entity)->update();
    return true;
}
