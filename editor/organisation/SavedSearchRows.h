// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The structured editor of a saved search (S25 R26): rows of Type, Name contains, Regex,
// Label, Extension and Path with an Any/All toggle. It reads and writes the R20 grammar, so a
// query typed in the search box and one built here evaluate the same way: All is one group of
// terms, Any one group per row joined with " OR ". A query the rows cannot express (ref:,
// missing:, a key of S29's, OR groups of several terms) opens as raw text instead.
// The filter kinds follow OpenEngine's smart folders (Include/UI/SmartFolder/SmartFolder.h:
// type, name, regex, tag; AND/OR).

#include <string>
#include <vector>

namespace jasperengine::editor {

    struct SavedSearchRows {
        enum class Field { Type, Name, Regex, Label, Extension, Path };
        struct Row {
            Field field = Field::Name;
            std::string value;

            bool operator==(const Row& other) const { return field == other.field && value == other.value; }
        };
        bool any = false;           // Any (OR) or All (AND)
        std::vector<Row> rows;

        bool operator==(const SavedSearchRows& other) const { return any == other.any && rows == other.rows; }

        static const char* fieldName(Field field);   // "Type", "Name contains", ...
        // The query the rows stand for
        std::string toQuery() const;
        // The rows of a query; false when they cannot express it
        static bool fromQuery(const std::string& query, SavedSearchRows& rows);
    };

}
