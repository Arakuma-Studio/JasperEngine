// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SavedSearchRows.h"

#include "search/AssetQuery.h"

using namespace jasperengine::editor;

namespace {

    const char* keyOf(SavedSearchRows::Field field) {
        switch (field) {
            case SavedSearchRows::Field::Type: return "t";
            case SavedSearchRows::Field::Regex: return "re";
            case SavedSearchRows::Field::Label: return "l";
            case SavedSearchRows::Field::Extension: return "ext";
            case SavedSearchRows::Field::Path: return "path";
            case SavedSearchRows::Field::Name: return "\"";
        }
        return "\"";
    }

    bool fieldOf(const std::string& key, SavedSearchRows::Field& field) {
        using F = SavedSearchRows::Field;
        if (key == "t") field = F::Type;
        else if (key == "re") field = F::Regex;
        else if (key == "l") field = F::Label;
        else if (key == "ext") field = F::Extension;
        else if (key == "path") field = F::Path;
        else if (key == "\"" || key.empty()) field = F::Name;
        else return false;
        return true;
    }

    std::string term(const SavedSearchRows::Row& row) {
        if (row.field == SavedSearchRows::Field::Name) return "\"" + row.value + "\"";
        const std::string value = row.value.find(' ') != std::string::npos ? "\"" + row.value + "\"" : row.value;
        return std::string(keyOf(row.field)) + ":" + value;
    }

}

const char* SavedSearchRows::fieldName(Field field) {
    switch (field) {
        case Field::Type: return "Type";
        case Field::Name: return "Name contains";
        case Field::Regex: return "Regex";
        case Field::Label: return "Label";
        case Field::Extension: return "Extension";
        case Field::Path: return "Path";
    }
    return "Name contains";
}

std::string SavedSearchRows::toQuery() const {
    std::string query;
    for (const Row& row : rows) {
        if (row.value.empty()) continue;
        if (!query.empty()) query += any ? " OR " : " ";
        query += term(row);
    }
    return query;
}

bool SavedSearchRows::fromQuery(const std::string& text, SavedSearchRows& out) {
    out = SavedSearchRows();
    const AssetQuery query = AssetQuery::parse(text);
    const auto& groups = query.groups();
    if (query.empty()) return true;
    auto rowOf = [](const AssetQuery::Term& term, Row& row) {
        Field field;
        if (!fieldOf(term.key, field)) return false;
        // a bare word is a name row only alone: several bare words are one Filter-mode text
        row = {field, term.value};
        return true;
    };
    if (groups.size() == 1) {
        out.any = false;
        for (const AssetQuery::Term& term : groups[0].terms) {
            Row row;
            if (!rowOf(term, row)) return false;
            out.rows.push_back(row);
        }
        // bare words become quoted phrases: only when there is one (several mean every word)
        size_t bare = 0;
        for (const AssetQuery::Term& term : groups[0].terms) bare += term.key.empty() ? 1 : 0;
        return bare <= 1;
    }
    out.any = true;
    for (const AssetQuery::Group& group : groups) {
        if (group.terms.size() != 1) return false;
        Row row;
        if (!rowOf(group.terms[0], row)) return false;
        out.rows.push_back(row);
    }
    return true;
}
