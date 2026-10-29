// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ZipArchive.h"

#include "miniz.h"

#include <fstream>

using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

    mz_zip_archive* zip(void* archive) {
        return static_cast<mz_zip_archive*>(archive);
    }

    std::string zipError(void* archive) {
        return mz_zip_get_error_string(mz_zip_get_last_error(zip(archive)));
    }

}

std::string ZipReader::checkName(const std::string& name) {
    if (name.empty()) return "empty name";
    if (name.front() == '/' || (name.size() > 1 && name[1] == ':')) return "absolute path";
    if (name.find('\\') != std::string::npos) return "backslash in name";
    size_t start = 0;
    while (start <= name.size()) {
        const size_t end = name.find('/', start);
        const std::string part = name.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (part == "..") return "'..' in name";
        if (part.empty() && end != std::string::npos && end + 1 != name.size()) return "empty part in name";
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return "";
}

ZipReader::ZipReader() : archive(new mz_zip_archive()) {
    mz_zip_zero_struct(zip(archive));
}

ZipReader::~ZipReader() {
    mz_zip_reader_end(zip(archive));
    delete zip(archive);
}

bool ZipReader::open(const fs::path& path, std::string& error) {
    mz_zip_reader_end(zip(archive));
    mz_zip_zero_struct(zip(archive));
    list.clear();
    if (!mz_zip_reader_init_file_v2(zip(archive), path.string().c_str(), 0, 0, 0)) {
        error = "cannot read " + path.string() + " as a zip archive (" + zipError(archive) + ")";
        return false;
    }
    const mz_uint count = mz_zip_reader_get_num_files(zip(archive));
    for (mz_uint i = 0; i < count; i++) {
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(zip(archive), i, &stat)) {
            error = "corrupt entry " + std::to_string(i) + " in " + path.string();
            return false;
        }
        ZipEntryInfo entry;
        entry.name = stat.m_filename;
        entry.size = stat.m_uncomp_size;
        entry.compressedSize = stat.m_comp_size;
        entry.directory = stat.m_is_directory != 0;
        // a Unix "made by" keeps the mode in the high half of the external attributes
        const unsigned madeBy = stat.m_version_made_by >> 8;
        entry.executable = madeBy == 3 && ((stat.m_external_attr >> 16) & 0111) != 0;
        entry.stored = stat.m_method == 0;
        list.push_back(entry);
    }
    return true;
}

bool ZipReader::read(const std::string& name, std::vector<uint8_t>& out, std::string& error) {
    const int index = mz_zip_reader_locate_file(zip(archive), name.c_str(), nullptr, 0);
    if (index < 0) {
        error = "no " + name + " in the archive";
        return false;
    }
    size_t size = 0;
    void* data = mz_zip_reader_extract_to_heap(zip(archive), static_cast<mz_uint>(index), &size, 0);
    if (!data) {
        error = "cannot extract " + name + " (" + zipError(archive) + ")";
        return false;
    }
    out.assign(static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + size);
    mz_free(data);
    return true;
}

bool ZipReader::extractAll(const fs::path& destination, std::string& error) {
    for (const ZipEntryInfo& entry : list) {
        const std::string problem = checkName(entry.name);
        if (!problem.empty()) {
            error = "refused archive entry \"" + entry.name + "\": " + problem;
            return false;
        }
    }
    std::error_code ec;
    fs::create_directories(destination, ec);
    for (size_t i = 0; i < list.size(); i++) {
        const ZipEntryInfo& entry = list[i];
        const fs::path target = destination / fs::u8path(entry.name);
        if (entry.directory) {
            fs::create_directories(target, ec);
            continue;
        }
        fs::create_directories(target.parent_path(), ec);
        if (!mz_zip_reader_extract_to_file(zip(archive), static_cast<mz_uint>(i), target.string().c_str(), 0)) {
            error = "cannot extract " + entry.name + " (" + zipError(archive) + ")";
            return false;
        }
        if (entry.executable) {
            fs::permissions(target, fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec, fs::perm_options::add, ec);
        }
    }
    return true;
}

ZipWriter::ZipWriter() : archive(new mz_zip_archive()) {
    mz_zip_zero_struct(zip(archive));
}

ZipWriter::~ZipWriter() {
    if (opened) mz_zip_writer_end(zip(archive));
    delete zip(archive);
}

bool ZipWriter::open(const fs::path& path, std::string& error) {
    if (!mz_zip_writer_init_file_v2(zip(archive), path.string().c_str(), 0, 0)) {
        error = "cannot write " + path.string() + " (" + zipError(archive) + ")";
        return false;
    }
    opened = true;
    return true;
}

bool ZipWriter::add(const std::string& name, const void* data, size_t size, bool store, std::string& error) {
    const mz_uint level = store ? MZ_NO_COMPRESSION : MZ_DEFAULT_LEVEL;
    if (!mz_zip_writer_add_mem_ex_v2(zip(archive), name.c_str(), data, size, nullptr, 0, level, 0, 0, nullptr, nullptr, 0, nullptr, 0)) {
        error = "cannot add " + name + " (" + zipError(archive) + ")";
        return false;
    }
    return true;
}

bool ZipWriter::addFile(const std::string& name, const fs::path& file, bool store, std::string& error) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        error = "cannot read " + file.string();
        return false;
    }
    const std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return add(name, bytes.data(), bytes.size(), store, error);
}

bool ZipWriter::finish(std::string& error) {
    if (!mz_zip_writer_finalize_archive(zip(archive))) {
        error = std::string("cannot finish the archive (") + zipError(archive) + ")";
        return false;
    }
    mz_zip_writer_end(zip(archive));
    opened = false;
    return true;
}
