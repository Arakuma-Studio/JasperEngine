// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TransientAssetResolver.h"

#include <filesystem>

using namespace jasperengine;
using namespace jasperengine::editor::titledesigner;
namespace ta = jasperengine::titleart;

void TransientAssetResolver::setBase(const ta::AssetResolver* base) {
    std::lock_guard<std::mutex> guard(lock);
    if (inner != base) changes++;
    inner = base;
}

void TransientAssetResolver::add(const std::string& assetPath, const std::string& absoluteFile) {
    std::lock_guard<std::mutex> guard(lock);
    auto it = files.find(assetPath);
    if (it != files.end() && it->second == absoluteFile) return;
    files[assetPath] = absoluteFile;
    changes++;
}

void TransientAssetResolver::remove(const std::string& assetPath) {
    std::lock_guard<std::mutex> guard(lock);
    if (files.erase(assetPath) > 0) changes++;
}

void TransientAssetResolver::clear() {
    std::lock_guard<std::mutex> guard(lock);
    if (!files.empty()) changes++;
    files.clear();
}

bool TransientAssetResolver::empty() const {
    std::lock_guard<std::mutex> guard(lock);
    return files.empty();
}

uint64_t TransientAssetResolver::revision() const {
    std::lock_guard<std::mutex> guard(lock);
    return changes;
}

ta::ResolvedTitleAsset TransientAssetResolver::resolve(const ta::TitleAssetRef& ref, ta::TitleAssetKind kind) const {
    const ta::AssetResolver* base = nullptr;
    {
        std::lock_guard<std::mutex> guard(lock);
        base = inner;
        // a project asset wins: the transient entry only stands in until the import exists
        if (base) {
            ta::ResolvedTitleAsset found = base->resolve(ref, kind);
            if (found.found) return found;
        }
        auto it = files.find(ref.path);
        if (it != files.end()) {
            ta::ResolvedTitleAsset out;
            out.found = true;
            out.path = it->second;
            out.assetPath = ref.path;
            std::error_code ec;
            const auto time = std::filesystem::last_write_time(std::filesystem::path(it->second), ec);
            out.revision = ec ? 1 : (uint64_t)time.time_since_epoch().count();
            return out;
        }
    }
    return base ? base->resolve(ref, kind) : ta::ResolvedTitleAsset{};
}

std::shared_ptr<FontFace> TransientAssetResolver::loadFont(const ta::ResolvedTitleAsset& asset, int faceIndex, std::string* error) const {
    if (!inner) {
        if (error) *error = "no resolver";
        return nullptr;
    }
    return inner->loadFont(asset, faceIndex, error);
}

bool TransientAssetResolver::readBytes(const ta::ResolvedTitleAsset& asset, std::vector<uint8_t>& out, std::string* error) const {
    if (!inner) {
        if (error) *error = "no resolver";
        return false;
    }
    return inner->readBytes(asset, out, error);
}

bool TransientAssetResolver::loadImage(const ta::ResolvedTitleAsset& asset, ta::TitleImageData& out, std::string* error) const {
    if (!inner) {
        if (error) *error = "no resolver";
        return false;
    }
    return inner->loadImage(asset, out, error);
}

std::shared_ptr<FontFace> TransientAssetResolver::builtinFont() const {
    return inner ? inner->builtinFont() : nullptr;
}

std::vector<ta::ResolvedTitleAsset> TransientAssetResolver::projectFonts() const {
    return inner ? inner->projectFonts() : std::vector<ta::ResolvedTitleAsset>{};
}
