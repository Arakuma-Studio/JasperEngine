// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef XMLUTILS_H
#define XMLUTILS_H

#include "Export.h"
#include "tinyxml2.h"
#include <string>

namespace jasperengine {
    class JASPERENGINE_API XMLUtils {
    private:
        static tinyxml2::XMLElement* getNodeForKey(const char* XMLFilePath, const char* rootName, const char* key, tinyxml2::XMLElement** rootNode, tinyxml2::XMLDocument* doc);
        // Replaces the file atomically (io::writeFileAtomic); false and an error logged on failure
        static bool saveDocument(const char* XMLFilePath, tinyxml2::XMLDocument& doc);

    public:
        static void setValueForKey(const char* XMLFilePath, const char* rootName, const char* key, const char* value);
        static std::string getValueForKey(const char* XMLFilePath, const char* rootName, const char* key);
        static void removeKey(const char* XMLFilePath, const char* rootName, const char *key);
    };
}


#endif //XMLUTILS_H