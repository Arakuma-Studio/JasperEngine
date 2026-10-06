// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "FontRegistry.h"

using namespace jasperengine;

void FontRegistry::registerFace(std::shared_ptr<FontFace> face, const std::string& family, int weight, int italic, bool fallback){
    // HTMLUI_SKELETON: p0-font
    (void)face;
    (void)family;
    (void)weight;
    (void)italic;
    (void)fallback;
}

std::shared_ptr<FontFace> FontRegistry::find(const std::string& family, int weight, bool italic){
    // HTMLUI_SKELETON: p0-font
    (void)family;
    (void)weight;
    (void)italic;
    return nullptr;
}

bool FontRegistry::isGenericFamily(const std::string& family){
    // HTMLUI_SKELETON: p0-font
    (void)family;
    return false;
}

const std::vector<std::shared_ptr<FontFace>>& FontRegistry::getFallbackFaces(){
    // HTMLUI_SKELETON: p0-font
    static const std::vector<std::shared_ptr<FontFace>> none;
    return none;
}

std::shared_ptr<FontFace> FontRegistry::getBuiltInLatin(){
    // HTMLUI_SKELETON: p0-font
    return nullptr;
}

std::shared_ptr<FontFace> FontRegistry::getBuiltInArabic(){
    // HTMLUI_SKELETON: p0-font
    return nullptr;
}

std::shared_ptr<FontFace> FontRegistry::getOrLoadFile(const std::string& path, int faceIndex){
    // HTMLUI_SKELETON: p0-font
    (void)path;
    (void)faceIndex;
    return nullptr;
}

void FontRegistry::clear(){
    // HTMLUI_SKELETON: p0-font
}
