// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleArtChecks.h"

#include "titleart/TitleTranslations.h"

#include <algorithm>
#include <memory>

using namespace jasperengine;
using namespace jasperengine::editor;
using titleart::Json;

namespace {

    std::vector<std::unique_ptr<CheckDef>>& definitions() {
        static auto* list = new std::vector<std::unique_ptr<CheckDef>>();
        return *list;
    }

    int severity(TitleCheckStatus status) {
        switch (status) {
            case TitleCheckStatus::Pass: return 0;
            case TitleCheckStatus::Skip: return 0;
            case TitleCheckStatus::Warn: return 1;
            case TitleCheckStatus::Fail: return 2;
        }
        return 0;
    }

    Json rectJson(const titleart::TitleRectI& r) {
        return Json::array({r.x, r.y, r.width, r.height});
    }

}

const char* editor::titleCheckKindName(TitleCheckKind kind) {
    switch (kind) {
        case TitleCheckKind::Document: return "document";
        case TitleCheckKind::Pixels: return "pixels";
        case TitleCheckKind::DocumentPixels: return "document_pixels";
        case TitleCheckKind::Estimate: return "estimate";
    }
    return "document";
}

const char* editor::titleCheckStatusName(TitleCheckStatus status) {
    switch (status) {
        case TitleCheckStatus::Pass: return "PASS";
        case TitleCheckStatus::Warn: return "WARN";
        case TitleCheckStatus::Fail: return "FAIL";
        case TitleCheckStatus::Skip: return "SKIP";
    }
    return "PASS";
}

Json TitleCheckResult::toJson() const {
    Json out = Json::object();
    out["id"] = id;
    out["kind"] = titleCheckKindName(kind);
    out["status"] = titleCheckStatusName(status);
    out["measured"] = measured;
    out["threshold"] = threshold;
    if (!at.empty()) {
        Json crops = Json::array();
        for (const titleart::TitleRectI& r : at) crops.push_back(Json{{"crop", rectJson(r)}});
        out["at"] = crops;
    }
    out["message"] = message;
    if (!fix.is_null()) out["fix"] = fix;
    out["method"] = method;
    if (!language.empty()) out["language"] = language;
    return out;
}

Json TitleCheckReport::toJson() const {
    Json out = Json::object();
    out["verdict"] = verdict == TitleCheckStatus::Skip ? "PASS" : titleCheckStatusName(verdict);
    Json list = Json::array();
    for (const TitleCheckResult& r : checks) list.push_back(r.toJson());
    out["checks"] = list;
    if (!look.empty()) out["look"] = look;
    out["guidance"] = TitleArtChecks::guidance();
    return out;
}

bool TitleArtChecks::registerCheck(CheckDef def) {
    if (def.id.empty() || !def.run || find(def.id)) return false;
    definitions().push_back(std::make_unique<CheckDef>(std::move(def)));
    return true;
}

bool TitleArtChecks::unregisterCheck(const std::string& id) {
    auto& list = definitions();
    auto it = std::find_if(list.begin(), list.end(), [&](const auto& d) { return d->id == id; });
    if (it == list.end()) return false;
    list.erase(it);
    return true;
}

const CheckDef* TitleArtChecks::find(const std::string& id) {
    for (const auto& d : definitions()) {
        if (d->id == id) return d.get();
    }
    return nullptr;
}

std::vector<const CheckDef*> TitleArtChecks::all() {
    std::vector<const CheckDef*> out;
    for (const auto& d : definitions()) out.push_back(d.get());
    return out;
}

bool TitleArtChecks::select(const std::vector<std::string>& names, std::vector<const CheckDef*>& out, std::string& error,
                            std::vector<std::string>& valid) {
    out.clear();
    if (names.empty()) {
        out = all();
        return true;
    }
    for (const std::string& name : names) {
        bool matched = false;
        for (const auto& d : definitions()) {
            if (d->id == name || (!d->group.empty() && d->group == name)) {
                if (std::find(out.begin(), out.end(), d.get()) == out.end()) out.push_back(d.get());
                matched = true;
            }
        }
        if (!matched) {
            valid.clear();
            for (const auto& d : definitions()) {
                valid.push_back(d->id);
                if (!d->group.empty() && std::find(valid.begin(), valid.end(), d->group) == valid.end()) valid.push_back(d->group);
            }
            error = "no check or check group '" + name + "'";
            return false;
        }
    }
    return true;
}

TitleCheckReport TitleArtChecks::run(const std::vector<const CheckDef*>& checks, TitleCheckContext& context) {
    TitleCheckReport report;
    for (const CheckDef* def : checks) {
        const bool needsPixels = def->kind == TitleCheckKind::Pixels || def->kind == TitleCheckKind::DocumentPixels;
        if (needsPixels && !context.render) {
            TitleCheckResult skipped;
            skipped.id = def->id;
            skipped.kind = def->kind;
            skipped.status = TitleCheckStatus::Skip;
            skipped.message = "needs a render (a GPU): not measured";
            skipped.method = "none";
            report.checks.push_back(std::move(skipped));
            continue;
        }
        const size_t first = report.checks.size();
        def->run(context, report.checks);
        for (size_t i = first; i < report.checks.size(); i++) {
            TitleCheckResult& r = report.checks[i];
            if (r.id.empty()) r.id = def->id;
            r.kind = def->kind;
        }
        // S87: once more per translation language, on the document resolved for it
        if (def->perLanguage && titleart::titleIsMultilingual(context.doc.root)) {
            for (const std::string& code : titleCheckLanguages(context)) {
                if (titleart::isSourceLanguage(context.doc.root, code)) continue;
                const titleart::TitleArtDoc resolved = titleart::resolveTitleLanguage(context.doc, code);
                TitleCheckContext language(resolved, context.path);
                language.file = context.file;
                language.assets = context.assets;
                language.options = context.options;
                language.options.language = code;
                language.render = context.render;
                language.source = &context.doc;
                const size_t at = report.checks.size();
                def->run(language, report.checks);
                for (size_t i = at; i < report.checks.size(); i++) {
                    TitleCheckResult& r = report.checks[i];
                    if (r.id.empty()) r.id = def->id;
                    r.kind = def->kind;
                    r.language = code;
                }
            }
        }
    }
    for (const TitleCheckResult& r : report.checks) {
        TitleCheckStatus status = r.status;
        if (status == TitleCheckStatus::Warn && context.options.failOnWarn) status = TitleCheckStatus::Fail;
        if (severity(status) > severity(report.verdict)) report.verdict = status;
        if ((r.status == TitleCheckStatus::Warn || r.status == TitleCheckStatus::Fail) && !r.at.empty()) {
            report.look.push_back(Json{{"crop", rectJson(r.at.front())}, {"why", r.message}});
        }
    }
    return report;
}

std::vector<std::string> editor::titleCheckLanguages(const TitleCheckContext& context) {
    const std::vector<std::string> all = titleart::titleLanguageCodes(context.doc.root);
    if (context.options.languages.empty()) return all;
    std::vector<std::string> out;
    for (const std::string& code : context.options.languages) {
        for (const std::string& a : all) {
            if (titleart::isSourceLanguage(context.doc.root, a) ? titleart::isSourceLanguage(context.doc.root, code) : a == code) out.push_back(a);
        }
    }
    return out;
}

const char* TitleArtChecks::guidance() {
    return "These are measurements; passing them does not mean the title looks good. Show the render to a person.";
}
