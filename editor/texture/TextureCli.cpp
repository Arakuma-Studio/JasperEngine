// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TextureCli.h"

#include "Project.h"
#include "texture/TextureDecode.h"
#include "texture/TextureExport.h"
#include "texture/TexturePrepare.h"

#include "io/AtomicFile.h"
#include "stb_image_write.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <set>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

    bool cliValue(int argc, char** argv, int& i, std::string& value, std::string& error){
        if (i + 1 >= argc){
            error = std::string(argv[i]) + " needs a value";
            return false;
        }
        value = argv[++i];
        return true;
    }

    fs::path cliAbsolute(const std::string& path){
        fs::path p(path);
        std::error_code ec;
        if (p.is_relative())
            p = fs::current_path(ec) / p;
        return p.lexically_normal();
    }

    // the texture sources a --paths entry names: a file, or every source under a directory
    bool cliMatches(const std::string& assetPath, const std::string& projectRelative, const std::vector<std::string>& wanted){
        if (wanted.empty())
            return true;
        for (std::string want : wanted){
            while (!want.empty() && want.back() == '/')
                want.pop_back();
            for (const std::string& path : {assetPath, projectRelative}){
                if (path == want || (path.size() > want.size() && path.compare(0, want.size(), want) == 0 && path[want.size()] == '/'))
                    return true;
            }
        }
        return false;
    }

    // RGBA8 / RG8 / R8 / RGBA16F pixels as a PNG: RG8 (normals) gets its Z back, float is clamped
    bool cliWritePng(const fs::path& path, GpuTextureFormat format, int width, int height, const std::vector<uint8_t>& pixels){
        const size_t texels = (size_t)width * (size_t)height;
        std::vector<uint8_t> out;
        int components = 4;
        if (format == GpuTextureFormat::R8){
            out = pixels;
            components = 1;
        }else if (format == GpuTextureFormat::R16){
            // a height map: its high byte (PNG through stb is 8-bit)
            out.resize(texels);
            for (size_t i = 0; i < texels; i++)
                out[i] = pixels[i * 2 + 1];
            components = 1;
        }else if (format == GpuTextureFormat::RG8){
            out.resize(texels * 3);
            for (size_t i = 0; i < texels; i++){
                const float x = pixels[i * 2] / 127.5f - 1.0f, y = pixels[i * 2 + 1] / 127.5f - 1.0f;
                const float z = std::sqrt(std::max(0.0f, 1.0f - x * x - y * y));
                out[i * 3] = pixels[i * 2];
                out[i * 3 + 1] = pixels[i * 2 + 1];
                out[i * 3 + 2] = (uint8_t)std::lround((z * 0.5f + 0.5f) * 255.0f);
            }
            components = 3;
        }else if (format == GpuTextureFormat::RGBA16F){
            if (!TextureDecode::convert(GpuTextureFormat::RGBA16F, GpuTextureFormat::RGBA8, width, height, pixels.data(), pixels.size(), out))
                return false;
        }else if (format == GpuTextureFormat::RGBA8){
            out = pixels;
        }else{
            return false;
        }
        std::string png;
        auto append = [](void* context, void* data, int size){
            static_cast<std::string*>(context)->append(static_cast<const char*>(data), (size_t)size);
        };
        if (!stbi_write_png_to_func(append, &png, width, height, components, out.data(), width * components))
            return false;
        return io::writeFileAtomic(path, png).ok();
    }

}

void TextureCli::printUsage(const std::string& commandName){
    std::cout
        << "Usage:\n"
        << "  " << commandName << " cook-textures --project <dir|project.yaml> [options]\n"
        << "  " << commandName << " cook-textures --decode <in.ktx2> <out.png> [--level N] [--platform-target <format>]\n\n"
        << "Cooks the project's textures into its texture cache (.jasperengine/cache/textures) and prints,\n"
        << "per texture and platform family, the format, size, mips, VRAM, disk, PSNR and cache hit.\n\n"
        << "Options:\n"
        << "  -p, --project <path>        Project directory or project.yaml.\n"
        << "      --platform <family>     desktop (default), mobile, android_etc2, web or all; repeatable.\n"
        << "      --paths <paths...>      Only these texture files or directories (asset- or project-relative).\n"
        << "      --report <out.json>     Write the report as JSON too.\n"
        << "      --out <dir>             Also write what an export ships: <dir>/je-cooked/<family>/ (index.json\n"
        << "                              and the cooked files), for packaging without an export.\n"
        << "      --jobs <N>              Textures cooked at once (default: the worker count).\n"
        << "      --check                 Exit 1 when a cook fails or a texture has a warning-level problem\n"
        << "                              (a reference that does not fit its type, a size block formats cannot take).\n"
        << "      --decode <in> <out>     Decode a KTX2 level to PNG (normal maps get Z back; HDR is clamped).\n"
        << "      --level <N>             The level to decode (0 = the largest).\n"
        << "      --platform-target <f>   Transcode a Basis file to this format first (bc7, bc1, astc_4x4, etc2_rgba8...).\n"
        << "  -h, --help                  Show this help.\n\n"
        << "Exit code: 0 ok, 1 a cook failed (or --check found problems), 2 usage error.\n";
}

bool TextureCli::parse(int argc, char** argv, TextureCliOptions& options, std::string& error){
    bool platformGiven = false;
    for (int i = 1; i < argc; i++){
        const std::string arg = argv[i];
        std::string value;
        if (arg == "-h" || arg == "--help"){
            options.help = true;
        }else if (arg == "-p" || arg == "--project"){
            if (!cliValue(argc, argv, i, value, error)) return false;
            options.project = cliAbsolute(value);
            if (options.project.filename() == "project.yaml")
                options.project = options.project.parent_path();
        }else if (arg == "--platform"){
            if (!cliValue(argc, argv, i, value, error)) return false;
            if (!platformGiven)
                options.families.clear();
            platformGiven = true;
            if (value == "all"){
                options.families = {TextureFamily::Desktop, TextureFamily::Mobile, TextureFamily::AndroidEtc2, TextureFamily::Web};
                continue;
            }
            TextureFamily family;
            if (!textureFamilyFromName(value, family)){
                error = "--platform must be desktop, mobile, android_etc2, web or all (got '" + value + "')";
                return false;
            }
            if (std::find(options.families.begin(), options.families.end(), family) == options.families.end())
                options.families.push_back(family);
        }else if (arg == "--paths"){
            while (i + 1 < argc && std::strncmp(argv[i + 1], "--", 2) != 0)
                options.paths.push_back(fs::path(argv[++i]).generic_string());
            if (options.paths.empty()){
                error = "--paths needs at least one path";
                return false;
            }
        }else if (arg == "--report"){
            if (!cliValue(argc, argv, i, value, error)) return false;
            options.report = cliAbsolute(value);
        }else if (arg == "--out"){
            if (!cliValue(argc, argv, i, value, error)) return false;
            options.out = cliAbsolute(value);
        }else if (arg == "--jobs"){
            if (!cliValue(argc, argv, i, value, error)) return false;
            try { options.jobs = std::stoi(value); } catch (...) { options.jobs = -1; }
            if (options.jobs < 1){
                error = "--jobs must be a positive number";
                return false;
            }
        }else if (arg == "--check"){
            options.check = true;
        }else if (arg == "--decode"){
            if (i + 2 >= argc){
                error = "--decode needs an input .ktx2 and an output .png";
                return false;
            }
            options.decodeIn = cliAbsolute(argv[++i]);
            options.decodeOut = cliAbsolute(argv[++i]);
        }else if (arg == "--level"){
            if (!cliValue(argc, argv, i, value, error)) return false;
            try { options.level = std::stoi(value); } catch (...) { options.level = -1; }
            if (options.level < 0){
                error = "--level must be 0 or more";
                return false;
            }
        }else if (arg == "--platform-target"){
            if (!cliValue(argc, argv, i, options.target, error)) return false;
            GpuTextureFormat format;
            if (!gpuFormatFromName(options.target, format)){
                error = "--platform-target: '" + options.target + "' is not a texture format";
                return false;
            }
        }else{
            error = "Unknown option: " + arg;
            return false;
        }
    }
    if (options.help)
        return true;
    if (options.decodeIn.empty() && options.project.empty()){
        error = "--project (or --decode) is required";
        return false;
    }
    return true;
}

int TextureCli::decode(const TextureCliOptions& options){
    std::ifstream in(options.decodeIn, std::ios::binary);
    if (!in){
        std::cerr << "cook-textures --decode: cannot read " << options.decodeIn.string() << "\n";
        return 1;
    }
    const std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::string error;
    GpuTextureFormat format = GpuTextureFormat::RGBA8;
    int width = 0, height = 0, faces = 1;
    std::vector<uint8_t> pixels;
    if (!options.target.empty()){
        GpuTextureFormat target;
        gpuFormatFromName(options.target, target);
        std::vector<std::vector<uint8_t>> levels;
        if (!transcodeKtx2(file, target, levels, width, height, error)){
            std::cerr << "cook-textures --decode: " << error << "\n";
            return 1;
        }
        const size_t level = std::min((size_t)options.level, levels.size() - 1);
        const int w = mipDimension(width, (int)level), h = mipDimension(height, (int)level);
        registerEditorTextureCodecs();
        if (!TextureDecode::decode(target, w, h, levels[level].data(), levels[level].size(), pixels)){
            std::cerr << "cook-textures --decode: " << gpuFormatName(target) << " cannot be decoded on the CPU\n";
            return 1;
        }
        format = TextureDecode::decodedFormat(target);
        width = w;
        height = h;
    }else if (!decodeKtx2ForPreview(file, (uint32_t)options.level, format, width, height, faces, pixels, error)){
        std::cerr << "cook-textures --decode: " << error << "\n";
        return 1;
    }
    // a cube's faces are stacked: the PNG is one face tall per face
    if (!cliWritePng(options.decodeOut, format, width, height * std::max(1, faces), pixels)){
        std::cerr << "cook-textures --decode: cannot write " << options.decodeOut.string() << "\n";
        return 1;
    }
    std::cout << options.decodeOut.string() << ": " << width << "x" << height << " " << gpuFormatName(format) << "\n";
    return 0;
}

int TextureCli::cook(Project& project, const TextureCliOptions& options){
    std::map<std::string, TextureCookInputs> all;
    std::string error;
    if (!TextureExport::gather(project, all, error)){
        std::cerr << "cook-textures: " << error << "\n";
        return 1;
    }
    std::map<std::string, TextureCookInputs> inputs;
    for (auto& [path, in] : all)
        if (cliMatches(path, in.projectRelative, options.paths))
            inputs.emplace(path, std::move(in));
    if (!options.paths.empty() && inputs.empty()){
        std::cerr << "cook-textures: no texture source matches --paths\n";
        return 1;
    }
    const TextureExportResult result = TextureExport::cookForExport(project, inputs, options.families, options.out, options.jobs,
        [](int done, int total, const std::string& path){
            std::cerr << "[" << done << "/" << total << "] " << path << "\n";
        }, nullptr);
    std::cout << TextureCookReport::toText(result.rows);
    if (!options.report.empty()){
        if (!io::writeFileAtomic(options.report, TextureCookReport::toJson(result.rows).dump(1) + "\n").ok()){
            std::cerr << "cook-textures: cannot write " << options.report.string() << "\n";
            return 1;
        }
    }
    if (!result.ok){
        std::cerr << "cook-textures: " << result.error << "\n";
        return 1;
    }
    if (options.check){
        const std::vector<std::string> problems = TextureCookReport::problems(result.rows);
        for (const std::string& problem : problems)
            std::cerr << "problem: " << problem << "\n";
        if (!problems.empty())
            return 1;
    }
    return 0;
}

std::vector<ProjectChecks::Finding> TextureCli::check(Project& project, const std::vector<std::string>& files, size_t& checked){
    std::vector<ProjectChecks::Finding> findings;
    checked = 0;
    const TextureProjectSettings settings = textureProjectSettingsOf(project);
    std::vector<TextureFamily> families = {TextureFamily::Desktop, TextureFamily::Mobile, TextureFamily::Web};
    if (settings.androidEtc2Fallback)
        families.push_back(TextureFamily::AndroidEtc2);
    TextureCookCache local;
    TextureCookQueue& queue = TextureCookQueue::get();
    const bool shared = queue.getProject() == &project;
    TextureCookCache& cache = shared ? queue.getCache() : local;
    if (!shared)
        local.open(project.getProjectInternalPath() / "cache" / "textures" / "v1", settings.globalCacheDir,
                   (uint64_t)std::max(settings.cacheSizeMB, 64) << 20);
    for (const std::string& path : projectTextureSources(project, true)){
        const std::string projectRelative = project.normalizeToProjectRelative(project.getAssetsPath() / path).generic_string();
        if (!files.empty() && std::find(files.begin(), files.end(), projectRelative) == files.end())
            continue;
        checked++;
        TextureCookInputs in;
        if (!gatherTextureInputs(project, path, in)){
            std::string reason;
            TextureImportSettings ignored;
            TextureImportSettings::load(in.absolute.string(), ignored, &reason);
            findings.push_back({"textures", projectRelative, 0, 0, "error", "texture import settings: " + reason});
            continue;
        }
        if (in.inference.sourceOnly)
            continue;
        for (const std::string& conflict : TextureTypeInference::conflicts(in.settings.resolvedType(), in.inference))
            findings.push_back({"textures", projectRelative, 0, 0, "warning", conflict});
        std::set<std::string> seen;
        for (TextureFamily family : families){
            TextureCookPlan plan;
            std::string error;
            if (!TextureCooker::plan(cache, makeTextureCookRequest(in, settings, family), plan, error)){
                findings.push_back({"textures", projectRelative, 0, 0, "error", error});
                break;
            }
            for (const std::string& problem : plan.choice.problems)
                if (seen.insert(problem).second)
                    findings.push_back({"textures", projectRelative, 0, 0, "warning", problem});
        }
    }
    cache.saveSources();
    if (!shared)
        local.close();
    return findings;
}
