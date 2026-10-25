// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "FontPool.h"

#include "Engine.h"
#include "Log.h"
#include "io/FileData.h"

#include <filesystem>

#include <vector>

using namespace jasperengine;

fonts_t& FontPool::getMap(){
    //To prevent similar problem of static init fiasco but on deinitialization
    //https://isocpp.org/wiki/faq/ctors#static-init-order-on-first-use
    static fonts_t* map = new fonts_t();
    return *map;
};

std::map<std::string, FontSource>& FontPool::getSources(){
    static auto* sources = new std::map<std::string, FontSource>();
    return *sources;
}

std::map<std::string, uint32_t>& FontPool::getGenerations(){
    static auto* generations = new std::map<std::string, uint32_t>();
    return *generations;
}

void FontPool::replace(const std::string& id, std::shared_ptr<STBText> font){
    if (!font)
        return;
    getMap()[id] = std::move(font);
    getGenerations()[id]++;
}

uint32_t FontPool::generation(const std::string& id){
    auto it = getGenerations().find(id);
    return it == getGenerations().end() ? 0 : it->second;
}

std::vector<std::string> FontPool::findIdsUsingFile(const std::string& systemPath){
    const std::string wanted = std::filesystem::path(systemPath).lexically_normal().generic_string();
    auto matches = [&](const std::string& path){
        return !path.empty() && std::filesystem::path(FileData::getSystemPath(path)).lexically_normal().generic_string() == wanted;
    };
    std::vector<std::string> ids;
    for (const auto& [id, source] : getSources()){
        bool uses = matches(source.fontpath);
        for (const std::string& fallback : source.fallbackPaths)
            uses = uses || matches(fallback);
        if (uses)
            ids.push_back(id);
    }
    return ids;
}

bool FontPool::getSource(const std::string& id, FontSource& out){
    auto it = getSources().find(id);
    if (it == getSources().end())
        return false;
    out = it->second;
    return true;
}

std::shared_ptr<STBText> FontPool::decode(const FontSource& source){
    auto font = std::make_shared<STBText>();
    if (!font->load(source.fontpath, source.fallbackPaths, source.fontSize))
        return nullptr;
    return font;
}

std::shared_ptr<STBText> FontPool::get(const std::string& id){
	return getMap()[id];
}

std::shared_ptr<STBText> FontPool::get(const std::string& id, const std::string& fontpath, const std::vector<std::string>& fallbackPaths, unsigned int fontSize){
	auto& shared = getMap()[id];

	if (shared.use_count() > 0){
		return shared;
	}

	const auto resource = std::make_shared<STBText>();
    resource->load(fontpath, fallbackPaths, fontSize);
	getSources()[id] = FontSource{fontpath, fallbackPaths, fontSize};

	shared = resource;

	return resource;
}

void FontPool::remove(const std::string& id){
	auto& map = getMap();
	auto it = map.find(id);
	if (it != map.end() && it->second.use_count() <= 1)
		map.erase(it);
}

void FontPool::clear(){
	getMap().clear();
}

void FontPool::clearUnused(){
	std::vector<std::string> ids;
	ids.reserve(getMap().size());
	for (const auto& [id, _] : getMap()) {
		ids.push_back(id);
	}
	for (const auto& id : ids) {
		remove(id);
	}
}
