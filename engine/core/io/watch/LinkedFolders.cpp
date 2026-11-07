// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "LinkedFolders.h"

namespace fs = std::filesystem;

namespace {

    bool isLinkStatus(const fs::file_status& status) {
#ifdef _MSC_VER
        return fs::is_symlink(status) || status.type() == fs::file_type::junction;
#else
        return fs::is_symlink(status);
#endif
    }

}

bool jasperengine::watch::isLink(const fs::directory_entry& entry) {
    std::error_code ec;
    return isLinkStatus(entry.symlink_status(ec)) && !ec;
}

bool jasperengine::watch::isLink(const fs::path& path) {
    std::error_code ec;
    return isLinkStatus(fs::symlink_status(path, ec)) && !ec;
}

fs::path jasperengine::watch::realPathOf(const fs::path& path) {
    std::error_code ec;
    fs::path real = fs::canonical(path, ec);
    return ec ? fs::path() : real;
}

bool jasperengine::watch::isAtOrBelowReal(const fs::path& path, const fs::path& dir) {
    const fs::path relative = path.lexically_relative(dir);
    return !relative.empty() && *relative.begin() != "..";
}

jasperengine::watch::LinkFollower::LinkFollower(const fs::path& root) : realRoot(realPathOf(root)) {
}

fs::path jasperengine::watch::LinkFollower::enter(const fs::path& path) {
    const fs::path real = realPathOf(path);
    std::error_code ec;
    if (real.empty() || realRoot.empty() || !fs::is_directory(real, ec)) return fs::path();
    if (isAtOrBelowReal(real, realRoot) || isAtOrBelowReal(realRoot, real)) return fs::path();
    return entered.insert(real).second ? real : fs::path();
}

jasperengine::watch::LinkedDirectoryIterator::LinkedDirectoryIterator(const fs::path& rootIn, fs::directory_options optionsIn, std::error_code& ec)
    : options(optionsIn), root(rootIn) {
    fs::directory_iterator first(root, options, ec);
    if (ec || first == fs::directory_iterator()) return;
    stack.push_back(std::move(first));
    recursionPending = true;
}

jasperengine::watch::LinkedDirectoryIterator& jasperengine::watch::LinkedDirectoryIterator::increment(std::error_code& ec) {
    ec.clear();
    if (stack.empty()) return *this;
    if (recursionPending && stack.size() < MAX_DEPTH) {
        const fs::directory_entry& entry = *stack.back();
        std::error_code typeError;
        bool descend = false;
        if (isLink(entry)) {
            if (entry.is_directory(typeError)) {
                if (follower.empty()) follower.emplace_back(root);
                descend = !follower.front().enter(entry.path()).empty();
            }
        } else {
            descend = entry.is_directory(typeError);
        }
        if (descend) {
            std::error_code openError;
            fs::directory_iterator inside(entry.path(), options, openError);
            if (openError) {
                // as the standard iterator: skipped when permission denied is (the iterator
                // swallowed it already), reported otherwise, and the walk stops
                ec = openError;
                stack.clear();
                return *this;
            }
            if (inside != fs::directory_iterator()) {
                stack.push_back(std::move(inside));
                recursionPending = true;
                return *this;
            }
        }
    }
    // on to the next entry; a folder that ended was entered at its parent's current entry
    while (true) {
        stack.back().increment(ec);
        if (ec) {
            stack.clear();
            return *this;
        }
        if (stack.back() != fs::directory_iterator()) break;
        stack.pop_back();
        if (stack.empty()) return *this;
    }
    recursionPending = true;
    return *this;
}
