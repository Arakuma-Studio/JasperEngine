// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "HtmlUITemplates.h"

#include <fstream>

using namespace jasperengine;

namespace fs = std::filesystem;

std::string editor::HtmlUITemplates::uniqueDocumentName(const fs::path& directory, const std::string& baseName){
    std::string name = baseName;
    for (int counter = 1;; counter++){
        std::error_code error;
        const bool taken = fs::exists(directory / (name + ".html"), error) || fs::exists(directory / (name + ".css"), error);
        if (!taken)
            return name;
        name = baseName + std::to_string(counter);
    }
}

std::string editor::HtmlUITemplates::documentText(const std::string& name){
    // XML-valid markup: phase-1 documents are read by RmlUi as they are
    return
        "<html>\n"
        "<head>\n"
        "    <title>" + name + "</title>\n"
        "    <link type=\"text/css\" href=\"" + name + ".css\"/>\n"
        "</head>\n"
        "<body>\n"
        "    <h1 id=\"title\">" + name + "</h1>\n"
        "    <button id=\"start\">Start</button>\n"
        "</body>\n"
        "</html>\n";
}

std::string editor::HtmlUITemplates::styleSheetText(){
    return
        "/* RmlUi shows elements inline unless a rule says otherwise */\n"
        "body {\n"
        "    display: block;\n"
        "    width: 100%;\n"
        "    height: 100%;\n"
        "    margin: 0px;\n"
        "    font-family: sans-serif;\n"
        "    font-size: 16px;\n"
        "    color: #ffffff;\n"
        "}\n"
        "\n"
        "h1 {\n"
        "    display: block;\n"
        "    margin: 48px 0px 24px 0px;\n"
        "    font-size: 48px;\n"
        "    text-align: center;\n"
        "}\n"
        "\n"
        "button {\n"
        "    display: block;\n"
        "    width: 200px;\n"
        "    margin: 0px auto;\n"
        "    padding: 12px 0px;\n"
        "    border-radius: 8px;\n"
        "    background-color: #2f6fed;\n"
        "    font-size: 24px;\n"
        "    text-align: center;\n"
        "}\n"
        "\n"
        "button:hover {\n"
        "    background-color: #4b84f0;\n"
        "}\n";
}

bool editor::HtmlUITemplates::createDocument(const fs::path& directory, const std::string& name, std::string& error){
    const fs::path html = directory / (name + ".html");
    const fs::path css = directory / (name + ".css");

    std::ofstream cssFile(css, std::ios::trunc | std::ios::binary);
    if (!cssFile){
        error = "could not write " + css.string();
        return false;
    }
    cssFile << styleSheetText();
    cssFile.close();

    std::ofstream htmlFile(html, std::ios::trunc | std::ios::binary);
    if (!htmlFile){
        std::error_code ignored;
        fs::remove(css, ignored);
        error = "could not write " + html.string();
        return false;
    }
    htmlFile << documentText(name);
    htmlFile.close();

    return true;
}
