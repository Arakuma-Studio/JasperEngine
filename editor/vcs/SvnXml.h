// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The XML svn writes with --xml (status, info, log, propget): elements, attributes, text and
// the five entities plus character references. Not a general XML parser: no DTD, no namespaces,
// no CDATA (svn writes none).

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace jasperengine::editor::vcs {

    struct XmlElement {
        std::string name;
        std::vector<std::pair<std::string, std::string>> attributes;
        std::string text;                     // the text directly inside it, entities decoded
        std::vector<XmlElement> children;

        // "" when absent
        const std::string& attribute(std::string_view key) const;
        // the first child of that name (null when none)
        const XmlElement* child(std::string_view childName) const;
        // the text of the first child of that name ("" when none)
        const std::string& childText(std::string_view childName) const;
    };

    // The document's root element; false with *error on malformed input
    bool parseXml(std::string_view text, XmlElement& root, std::string* error = nullptr);

}
