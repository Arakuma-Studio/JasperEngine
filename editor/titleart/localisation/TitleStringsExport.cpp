// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// export_title's `csv` and `xliff` formats (docs/specs/S87-title-localisation-interchange.md R20,
// R22, R23): `<stem>.csv` (several documents in one, with a `file` column) and one
// `<stem>.<code>.xlf` per target language (one <file> per document).

#include "TitleStrings.h"

#include "Project.h"
#include "titleart/interchange/TitleExport.h"

#include <filesystem>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

    std::vector<TitleStringsDocument> documentsOf(const TitleExportContext& ctx) {
        std::vector<TitleStringsDocument> docs;
        const fs::path project = ctx.request.project ? ctx.request.project->getProjectPath() : ctx.request.document.parent_path();
        docs.push_back({ctx.request.relative, &ctx.request.doc});
        for (const auto& [path, doc] : ctx.request.more) {
            std::error_code ec;
            docs.push_back({fs::relative(path, project, ec).generic_string(), &doc});
        }
        return docs;
    }

    bool planCsv(TitleExportContext& ctx) {
        TitleExportContext* c = &ctx;
        ctx.then([c] { return c->write(c->fileName("", "", "csv"), exportTitleStringsCsv(documentsOf(*c), c->request.languages)); });
        return true;
    }

    bool planXliff(TitleExportContext& ctx) {
        TitleExportContext* c = &ctx;
        ctx.then([c] {
            const auto files = exportTitleStringsXliff(documentsOf(*c), c->request.languages);
            if (files.empty()) return c->fail("no_languages", "the title has no translation language to export (edit_title add_language first)");
            for (const auto& [code, text] : files) {
                if (!c->write(c->fileName("", code, "xlf"), text)) return false;
            }
            return true;
        });
        return true;
    }

}

void editor::registerTitleStringsExport() {
    TitleExportFormat csv;
    csv.name = "csv";
    csv.image = false;
    csv.plan = planCsv;
    registerTitleExportFormat(std::move(csv));
    TitleExportFormat xliff;
    xliff.name = "xliff";
    xliff.image = false;
    xliff.plan = planXliff;
    registerTitleExportFormat(std::move(xliff));
}
