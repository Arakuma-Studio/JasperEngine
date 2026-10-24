// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "HtmlUITemplates.h"
#include "io/AtomicFile.h"

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
    // standard HTML5: the compatibility layer (phase 2) turns it into what RmlUi reads
    return
        "<!DOCTYPE html>\n"
        "<html lang=\"en\">\n"
        "<head>\n"
        "    <meta charset=\"utf-8\">\n"
        "    <title>" + name + "</title>\n"
        "    <link rel=\"stylesheet\" href=\"" + name + ".css\">\n"
        "</head>\n"
        "<body>\n"
        "    <h1 id=\"title\">" + name + "</h1>\n"
        "    <button id=\"start\">Start</button>\n"
        "</body>\n"
        "</html>\n";
}

std::string editor::HtmlUITemplates::styleSheetText(){
    return
        "/* Standard CSS: the engine rewrites what its renderer needs and reports what it cannot draw */\n"
        "body {\n"
        "    margin: 0;\n"
        "    font-family: sans-serif;\n"
        "    color: #ffffff;\n"
        "}\n"
        "\n"
        "h1 {\n"
        "    margin: 48px 0 24px 0;\n"
        "    font-size: 48px;\n"
        "    text-align: center;\n"
        "}\n"
        "\n"
        "button {\n"
        "    display: block;\n"
        "    width: 200px;\n"
        "    margin: 0 auto;\n"
        "    padding: 12px 0;\n"
        "    border: 0;\n"
        "    border-radius: 8px;\n"
        "    background-color: #2f6fed;\n"
        "    color: #ffffff;\n"
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

    const io::WriteResult cssWritten = io::writeFileAtomic(css, styleSheetText());
    if (!cssWritten.ok()){
        error = "could not write " + css.string() + ": " + io::describeWriteError(cssWritten);
        return false;
    }

    const io::WriteResult htmlWritten = io::writeFileAtomic(html, documentText(name));
    if (!htmlWritten.ok()){
        std::error_code ignored;
        fs::remove(css, ignored);
        error = "could not write " + html.string() + ": " + io::describeWriteError(htmlWritten);
        return false;
    }

    return true;
}
