// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "XMLUtils.h"
#include "Log.h"
#include "io/File.h"
#include "io/FileData.h"
#include "io/AtomicFile.h"
#include "System.h"

using namespace jasperengine;

bool XMLUtils::saveDocument(const char* XMLFilePath, tinyxml2::XMLDocument& doc){
    tinyxml2::XMLPrinter printer;
    doc.Print(&printer);
    // CStrSize counts the terminating null
    const std::string_view bytes(printer.CStr(), printer.CStrSize() > 0 ? (size_t)printer.CStrSize() - 1 : 0);

    // a crash or a full disk mid-save keeps the previous settings readable
    const std::string systemPath = FileData::getSystemPath(XMLFilePath);
    const io::WriteResult result = io::writeFileAtomic(systemPath, bytes);
    if (!result.ok()) {
        Log::error("Can't save XML file '%s': %s", systemPath.c_str(), io::describeWriteError(result).c_str());
        return false;
    }
    System::instance().syncFileSystem();
    return true;
}

tinyxml2::XMLElement* XMLUtils::getNodeForKey(const char* XMLFilePath, const char* rootName, const char* key, tinyxml2::XMLElement** rootNode, tinyxml2::XMLDocument* doc){
    *rootNode = NULL;

    File file;

    std::string xmlBuffer = "";
    if (file.open(XMLFilePath) == FileErrors::FILEDATA_OK)
        xmlBuffer = file.readString();

    if (!xmlBuffer.empty()) {
        doc->Parse(xmlBuffer.c_str(), xmlBuffer.size());
        *rootNode = doc->RootElement();
    }

    if (!(*rootNode)) {
        if (!doc->FirstChild()) {
            tinyxml2::XMLDeclaration *xmlDeclaration = doc->NewDeclaration(NULL);
            if (xmlDeclaration) {
                doc->LinkEndChild(xmlDeclaration);
            }
        }

        tinyxml2::XMLElement *rootElement = doc->NewElement(rootName);
        if (!rootElement)
            return nullptr;

        doc->LinkEndChild(rootElement);
        *rootNode = rootElement;
    }

    return (*rootNode)->FirstChildElement(key);
}

std::string XMLUtils::getValueForKey(const char* XMLFilePath, const char* rootName, const char *key){
    if (!key) {
        Log::error("Can't get value in XML (%s): Key is invalid", XMLFilePath);
        return "";
    }

    tinyxml2::XMLDocument doc;

    //They are deleted when doc is deleted
    tinyxml2::XMLElement* node;
    tinyxml2::XMLElement* rootNode;

    node = XMLUtils::getNodeForKey(XMLFilePath, rootName, key, &rootNode, &doc);
    if (node && node->FirstChild()){
        return std::string(node->FirstChild()->Value());
    }

    return "";
}

void XMLUtils::setValueForKey(const char* XMLFilePath, const char* rootName, const char* key, const char* value) {
    if (!key)
        Log::error("Can't set value in XML (%s): Key is invalid", XMLFilePath);

    if (!value)
        Log::error("Can't set value in XML (%s): Value is invalid", XMLFilePath);

    if (key && value) {
        tinyxml2::XMLDocument doc;

        //They are deleted when doc is deleted
        tinyxml2::XMLElement *rootNode;
        tinyxml2::XMLElement *node;

        node = getNodeForKey(XMLFilePath, rootName, key, &rootNode, &doc);

        if (node) {
            if (node->FirstChild()) {
                node->FirstChild()->SetValue(value);
            } else {
                tinyxml2::XMLText *content = doc.NewText(value);
                node->LinkEndChild(content);
            }
        } else {
            if (rootNode) {
                tinyxml2::XMLElement *tmpNode = doc.NewElement(key);
                rootNode->LinkEndChild(tmpNode);
                tinyxml2::XMLText *content = doc.NewText(value);
                tmpNode->LinkEndChild(content);
            }
        }

        saveDocument(XMLFilePath, doc);
    }
}

void XMLUtils::removeKey(const char* XMLFilePath, const char* rootName, const char *key){
    if (!key) {
        Log::error("Can't delete value in XML (%s): Key is invalid", XMLFilePath);

    } else {

        tinyxml2::XMLDocument doc;

        //They are deleted when doc is deleted
        tinyxml2::XMLElement *rootNode;
        tinyxml2::XMLElement *node;

        node = getNodeForKey(XMLFilePath, rootName, key, &rootNode, &doc);

        if (node){
            doc.DeleteNode(node);

            saveDocument(XMLFilePath, doc);
        }

    }
}