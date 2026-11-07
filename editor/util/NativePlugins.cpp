// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "NativePlugins.h"

#include "asset/AssetTypes.h"
#include "util/FileUtils.h"
#include "util/SafeWrite.h"
#include "Out.h"

#include "tinyxml2.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iterator>
#include <regex>
#include <set>

using namespace jasperengine::editor;

namespace {

    namespace fs = std::filesystem;

    std::string lower(std::string text){
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    }

    std::string extensionOf(const std::string& path){
        const size_t slash = path.find_last_of('/');
        const size_t dot = path.find_last_of('.');
        if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return std::string();
        return lower(path.substr(dot));
    }

    // "macos/Ads.framework" -> "macos"
    std::string nativeParentOf(const std::string& path){
        const size_t slash = path.find_last_of('/');
        return slash == std::string::npos ? std::string() : path.substr(0, slash);
    }

    std::string nativeNameOf(const std::string& path){
        const size_t slash = path.find_last_of('/');
        return slash == std::string::npos ? path : path.substr(slash + 1);
    }

    std::string stemOf(const std::string& path){
        const size_t slash = path.find_last_of('/');
        std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
        const size_t dot = name.find_last_of('.');
        return dot == std::string::npos ? name : name.substr(0, dot);
    }

    bool isBundle(const fs::path& path){
        const std::string ext = lower(path.extension().string());
        return ext == ".framework" || ext == ".xcframework" || ext == ".bundle";
    }

    // the sources are read for listings only: a huge file is skipped; line comments go (a
    // comment's example code is no method), "://" in URLs stays
    std::string readSmallFile(const fs::path& path){
        std::error_code ec;
        const auto size = fs::file_size(path, ec);
        if (ec || size > (1u << 20)) return std::string();
        std::ifstream in(path, std::ios::binary);
        std::string text(std::istreambuf_iterator<char>(in), (std::istreambuf_iterator<char>()));
        size_t at = 0;
        while ((at = text.find("//", at)) != std::string::npos){
            if (at > 0 && text[at - 1] == ':'){
                at += 2;
                continue;
            }
            const size_t end = text.find('\n', at);
            text.erase(at, end == std::string::npos ? std::string::npos : end - at);
        }
        return text;
    }

    std::vector<std::string> matches(const std::string& text, const std::regex& pattern){
        std::vector<std::string> out;
        for (auto it = std::sregex_iterator(text.begin(), text.end(), pattern); it != std::sregex_iterator(); ++it){
            for (size_t group = 1; group < it->size(); group++){
                if ((*it)[group].matched){
                    out.push_back((*it)[group].str());
                    break;
                }
            }
        }
        return out;
    }

    bool isIdentifier(const std::string& name){
        if (name.empty() || !(std::isalpha((unsigned char)name[0]) || name[0] == '_')) return false;
        for (char c : name){
            if (!(std::isalnum((unsigned char)c) || c == '_')) return false;
        }
        return true;
    }

    bool isPackageName(const std::string& package){
        if (package.empty()) return false;
        size_t start = 0;
        while (true){
            const size_t dot = package.find('.', start);
            const std::string part = package.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
            if (!isIdentifier(part)) return false;
            if (dot == std::string::npos) return true;
            start = dot + 1;
        }
    }

    // one entry of an .xcframework's Info.plist AvailableLibraries
    struct NativeXcframeworkLibrary {
        std::string identifier, library, binary, headers, platform, variant;
    };

    std::vector<NativeXcframeworkLibrary> nativeXcframeworkLibraries(const fs::path& xcframework){
        std::vector<NativeXcframeworkLibrary> out;
        tinyxml2::XMLDocument doc;
        const auto plistPath = (xcframework / "Info.plist").generic_u8string();
        if (doc.LoadFile(std::string(plistPath.begin(), plistPath.end()).c_str()) != tinyxml2::XML_SUCCESS) return out;
        const tinyxml2::XMLElement* dict = doc.FirstChildElement("plist") ? doc.FirstChildElement("plist")->FirstChildElement("dict") : nullptr;
        auto valueOf = [](const tinyxml2::XMLElement* d, const char* key) -> const tinyxml2::XMLElement* {
            for (const tinyxml2::XMLElement* e = d ? d->FirstChildElement("key") : nullptr; e; e = e->NextSiblingElement("key")){
                if (e->GetText() && std::string(e->GetText()) == key) return e->NextSiblingElement();
            }
            return nullptr;
        };
        const tinyxml2::XMLElement* libraries = valueOf(dict, "AvailableLibraries");
        for (const tinyxml2::XMLElement* entry = libraries ? libraries->FirstChildElement("dict") : nullptr; entry; entry = entry->NextSiblingElement("dict")){
            auto text = [&](const char* key){
                const tinyxml2::XMLElement* e = valueOf(entry, key);
                return e && e->GetText() ? std::string(e->GetText()) : std::string();
            };
            NativeXcframeworkLibrary library{text("LibraryIdentifier"), text("LibraryPath"), text("BinaryPath"), text("HeadersPath"),
                                       text("SupportedPlatform"), text("SupportedPlatformVariant")};
            if (!library.identifier.empty() && !library.library.empty()) out.push_back(library);
        }
        return out;
    }

    const std::set<std::string>& javaKeywords(){
        static const std::set<std::string> words = {
            "abstract", "assert", "boolean", "break", "byte", "case", "catch", "char", "class", "const", "continue",
            "default", "do", "double", "else", "enum", "extends", "final", "finally", "float", "for", "goto", "if",
            "implements", "import", "instanceof", "int", "interface", "long", "native", "new", "package", "private",
            "protected", "public", "return", "short", "static", "strictfp", "super", "switch", "synchronized", "this",
            "throw", "throws", "transient", "try", "void", "volatile", "while", "true", "false", "null",
            // Kotlin hard keywords, Swift / Objective-C words a method cannot be
            "fun", "object", "val", "var", "when", "typealias", "is", "in", "as", "func", "self", "id", "nil",
        };
        return words;
    }

}

fs::path NativePlugins::relativeRoot(){
    return fs::path("plugins") / "native";
}

fs::path NativePlugins::root(const fs::path& projectPath){
    return projectPath / relativeRoot();
}

const std::vector<std::string>& NativePlugins::platformNames(){
    static const std::vector<std::string> names = {"android", "apple", "ios", "macos", "web", "cpp"};
    return names;
}

bool NativePlugins::Scan::has(const std::string& platform) const{
    auto it = files.find(platform);
    return it != files.end() && !it->second.empty();
}

std::vector<std::string> NativePlugins::Scan::filesWith(const std::string& platform, const std::vector<std::string>& extensions) const{
    std::vector<std::string> out;
    auto it = files.find(platform);
    if (it == files.end()) return out;
    for (const std::string& file : it->second){
        const std::string ext = extensionOf(file);
        if (std::find(extensions.begin(), extensions.end(), ext) != extensions.end()) out.push_back(file);
    }
    return out;
}

size_t NativePlugins::Scan::count(const std::string& platform) const{
    auto it = files.find(platform);
    return it == files.end() ? 0 : it->second.size();
}

NativePlugins::Scan NativePlugins::scan(const fs::path& projectPath){
    Scan result;
    result.root = root(projectPath);
    std::error_code ec;
    result.exists = fs::is_directory(result.root, ec);
    if (!result.exists) return result;
    for (const std::string& platform : platformNames()){
        const fs::path dir = result.root / platform;
        if (!fs::is_directory(dir, ec)) continue;
        std::vector<std::string>& list = result.files[platform];
        for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
             !ec && it != fs::recursive_directory_iterator(); it.increment(ec)){
            const bool directory = it->is_directory(ec);
            if (isSkippedEntry(it->path(), directory)){
                if (directory) it.disable_recursion_pending();
                continue;
            }
            const std::string relative = it->path().lexically_relative(dir).generic_string();
            if (directory){
                // an SDK bundle is one entry
                if (isBundle(it->path())){
                    list.push_back(relative);
                    it.disable_recursion_pending();
                }
                continue;
            }
            if (it->is_regular_file(ec)) list.push_back(relative);
        }
        std::sort(list.begin(), list.end());
    }
    return result;
}

std::vector<NativePlugins::PluginInfo> NativePlugins::listPlugins(const Scan& scan){
    std::map<std::string, PluginInfo> plugins;
    std::map<std::string, std::set<std::string>> methods;
    auto add = [&](const std::string& name, const std::string& platform, const std::string& file, const std::vector<std::string>& found) {
        if (!isIdentifier(name)) return;
        PluginInfo& info = plugins[name];
        info.name = name;
        info.files[platform].push_back(platform + "/" + file);
        for (const std::string& method : found){
            if (method != "onJasperInit" && method != "jasperInit" && method != "register") methods[name].insert(method);
        }
    };

    static const std::regex javaMethod(R"(public\s+static\s+(?:final\s+|synchronized\s+)*[\w<>\[\],.?\s]+?\s+(\w+)\s*\()");
    static const std::regex kotlinMethod(R"(@JvmStatic\s+(?:(?:public|internal|suspend|inline)\s+)*fun\s+(\w+)\s*\()");
    static const std::regex objcMethod(R"((?:^|\n)\s*\+\s*\([^)]*\)\s*(\w+))");
    static const std::regex swiftClass(R"(@objc\s*\(\s*(\w+)\s*\))");
    static const std::regex swiftMethod(R"(@objc\s+(?:public\s+|open\s+)?(?:static|class)\s+func\s+(\w+))");
    static const std::regex jsRegister(R"(JasperNative\.register\(\s*["'](\w+)["'])");
    static const std::regex jsMethod(R"((?:^|\n)\s*(?:async\s+)?(\w+)\s*(?::\s*(?:async\s+)?(?:function\b|\()|\([^)\n]*\)\s*\{))");
    static const std::regex cppPlugin(R"(JASPERENGINE_NATIVE_PLUGIN\s*\(\s*(\w+)\s*\))");
    static const std::regex cppMethod(R"re(\.\s*(?:method|asyncMethod)\s*\(\s*"(\w+)")re");

    for (const auto& [platform, files] : scan.files){
        for (const std::string& file : files){
            const std::string ext = extensionOf(file);
            const fs::path path = scan.root / platform / file;
            if (platform == "android"){
                if (ext == ".java") add(stemOf(file), platform, file, matches(readSmallFile(path), javaMethod));
                else if (ext == ".kt") add(stemOf(file), platform, file, matches(readSmallFile(path), kotlinMethod));
                else if (ext == ".aar" || ext == ".jar") continue;
            } else if (platform == "apple" || platform == "ios" || platform == "macos"){
                if (ext == ".m" || ext == ".mm"){
                    add(stemOf(file), platform, file, matches(readSmallFile(path), objcMethod));
                } else if (ext == ".swift"){
                    const std::string text = readSmallFile(path);
                    const std::vector<std::string> names = matches(text, swiftClass);
                    add(names.empty() ? stemOf(file) : names.front(), platform, file, matches(text, swiftMethod));
                }
            } else if (platform == "web"){
                if (ext != ".js") continue;
                const std::string text = readSmallFile(path);
                const std::vector<std::string> names = matches(text, jsRegister);
                for (const std::string& name : names) add(name, platform, file, matches(text, jsMethod));
            } else if (platform == "cpp"){
                if (ext != ".cpp" && ext != ".cc" && ext != ".cxx" && ext != ".mm") continue;
                const std::string text = readSmallFile(path);
                for (const std::string& name : matches(text, cppPlugin)) add(name, platform, file, matches(text, cppMethod));
            }
        }
    }

    std::vector<PluginInfo> out;
    for (auto& [name, info] : plugins){
        static const std::set<std::string> notMethods = {"if", "for", "while", "switch", "return", "function", "catch"};
        for (const std::string& method : methods[name]){
            if (!notMethods.count(method)) info.methods.push_back(method);
        }
        out.push_back(std::move(info));
    }
    return out;
}

std::vector<std::string> NativePlugins::listMocks(const fs::path& luaDir){
    std::vector<std::string> mocks;
    std::error_code ec;
    const fs::path dir = luaDir / "native_mocks";
    if (!fs::is_directory(dir, ec)) return mocks;
    for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)){
        if (it->is_regular_file(ec) && lower(it->path().extension().string()) == ".lua") mocks.push_back(it->path().stem().string());
    }
    std::sort(mocks.begin(), mocks.end());
    return mocks;
}

std::map<std::string, std::string> NativePlugins::platformOnlyPlugins(const std::vector<PluginInfo>& plugins, const std::string& running){
    std::map<std::string, std::string> out;
    for (const PluginInfo& plugin : plugins){
        if (plugin.files.count("cpp")) continue;
        const bool android = plugin.files.count("android") != 0;
        const bool apple = plugin.files.count("apple") != 0;
        const bool ios = plugin.files.count("ios") != 0;
        const bool macos = plugin.files.count("macos") != 0;
        const bool web = plugin.files.count("web") != 0;
        // a CMake macOS build compiles no Swift: a Swift-only Apple plugin does not run there
        auto compiledByCMake = [&plugin](const char* folder) {
            auto it = plugin.files.find(folder);
            if (it == plugin.files.end()) return false;
            for (const std::string& file : it->second){
                if (extensionOf(file) != ".swift") return true;
            }
            return false;
        };
        const bool runsHere = (running == "android" && android) || (running == "web" && web) ||
                              (running == "macos-cmake" && (compiledByCMake("apple") || compiledByCMake("macos"))) ||
                              (running == "ios" && (apple || ios)) || (running == "macos" && (apple || macos));
        if (runsHere) continue;
        std::vector<std::string> parts;
        if (android) parts.push_back("Java/Kotlin plugins run only on Android");
        if (apple || ios || macos){
            const char* where = (apple || (ios && macos)) ? "iOS and macOS" : (ios ? "iOS" : "macOS");
            parts.push_back(std::string("Objective-C/Swift plugins run only on ") + where);
        }
        if (web) parts.push_back("JavaScript plugins run only on the Web");
        if (parts.empty()) continue;
        std::string text;
        for (const std::string& part : parts){
            if (!text.empty()) text += "; ";
            text += part;
        }
        out[plugin.name] = text;
    }
    return out;
}

std::string NativePlugins::editorPlatform(){
#if defined(__APPLE__)
    // the Play library is a CMake build
    return "macos-cmake";
#else
    return "desktop";
#endif
}

void NativePlugins::warnStaleObjectiveC(const fs::path& projectPath){
#if defined(__APPLE__)
    static std::map<std::string, fs::file_time_type> loaded;   // file -> its time at the first Play
    static bool warned = false;
    const Scan s = scan(projectPath);
    for (const char* folder : {"apple", "macos"}){
        for (const std::string& file : s.filesWith(folder, {".m", ".mm"})){
            const fs::path path = s.root / folder / file;
            std::error_code ec;
            const fs::file_time_type time = fs::last_write_time(path, ec);
            if (ec) continue;
            auto [it, first] = loaded.emplace(path.string(), time);
            if (!first && it->second != time && !warned){
                warned = true;
                Out::warning("Native plugins: %s changed since the first Play of this editor session. macOS keeps the "
                             "Objective-C classes it loaded first: restart the editor to run the new code.", path.filename().string().c_str());
            }
        }
    }
#else
    (void)projectPath;
#endif
}

std::string NativePlugins::platformOf(const fs::path& projectPath, const fs::path& path){
    const fs::path relative = path.lexically_normal().lexically_relative(root(projectPath).lexically_normal());
    if (relative.empty() || relative.is_absolute()) return std::string();
    auto first = relative.begin();
    if (first == relative.end()) return std::string();
    const std::string top = first->string();
    if (top == "." || top == "..") return std::string();
    const auto& names = platformNames();
    return std::find(names.begin(), names.end(), top) != names.end() ? top : std::string();
}

bool NativePlugins::isEditorSidecar(const fs::path& path){
    return AssetTypes::isMetaFile(path);
}

bool NativePlugins::isSkippedEntry(const fs::path& path, bool directory){
    const std::string name = path.filename().string();
    if (name.empty() || name[0] == '.') return true;
    return directory ? name == "build" : isEditorSidecar(path);
}

std::vector<NativePlugins::MacosSdk> NativePlugins::macosSdks(const Scan& scan){
    std::vector<MacosSdk> sdks;
    for (const char* folder : {"apple", "macos"}){
        auto files = scan.files.find(folder);
        if (files == scan.files.end()) continue;
        for (const std::string& file : files->second){
            const std::string ext = extensionOf(file);
            MacosSdk sdk;
            sdk.path = std::string(folder) + "/" + file;
            sdk.library = sdk.path;
            const fs::path absolute = scan.root / FileUtils::pathFromUtf8(sdk.path);
            if (ext == ".bundle"){
                sdk.resource = true;
            } else if (ext == ".a"){
                // linked only
            } else if (ext == ".framework"){
                const fs::path binary = frameworkBinary(absolute);
                sdk.embedded = !binary.empty() && !isStaticArchive(binary);
            } else if (ext == ".xcframework"){
                // an iOS-only SDK in apple/ has nothing for macOS
                fs::path headers;
                const fs::path library = xcframeworkLibrary(absolute, "macos", &headers);
                if (library.empty()) continue;
                sdk.library = sdk.path + "/" + FileUtils::pathToGenericUtf8(library);
                if (!headers.empty()) sdk.headers = sdk.path + "/" + FileUtils::pathToGenericUtf8(headers);
                const fs::path libraryPath = absolute / library;
                const fs::path binary = library.extension() == ".framework" ? frameworkBinary(libraryPath) : libraryPath;
                sdk.embedded = !binary.empty() && fs::is_regular_file(binary) && !isStaticArchive(binary);
            } else {
                continue;
            }
            sdks.push_back(sdk);
        }
    }
    return sdks;
}

namespace {

    // the folders of the frameworks (their -F search paths), one each, in order
    std::vector<std::string> nativeFrameworkDirs(const std::vector<NativePlugins::MacosSdk>& sdks, const std::string& rootExpression){
        std::vector<std::string> dirs;
        for (const NativePlugins::MacosSdk& sdk : sdks){
            if (extensionOf(sdk.library) != ".framework") continue;
            const std::string dir = rootExpression + "/" + nativeParentOf(sdk.library);
            if (std::find(dirs.begin(), dirs.end(), dir) == dirs.end()) dirs.push_back(dir);
        }
        return dirs;
    }

}

std::string NativePlugins::macosCMake(const Scan& scan, const std::string& target, const std::string& rootExpression,
                                      const std::vector<std::string>& frameworks, const std::string& includeDir, bool executable){
    std::vector<std::string> sources;
    for (const char* folder : {"apple", "macos"}){
        for (const std::string& file : scan.filesWith(folder, {".m", ".mm", ".c", ".cpp", ".cc", ".cxx"})){
            sources.push_back(rootExpression + "/" + folder + "/" + file);
        }
    }
    const bool anyApple = scan.has("apple") || scan.has("macos");
    if (!anyApple) return std::string();
    std::string s;
    s += "\n# Objective-C native plugins (plugins/native/apple and macos), macOS only\n";
    s += "if(APPLE AND NOT IOS)\n";
    // main.cpp's "unavailable" list of a CMake build (no Swift here)
    s += "    target_compile_definitions(" + target + " PRIVATE JASPERENGINE_MACOS_CMAKE)\n";
    if (!sources.empty()){
        s += "    enable_language(OBJC OBJCXX)\n";
        s += "    set(JASPERENGINE_NATIVE_PLUGIN_SOURCES\n";
        for (const std::string& source : sources) s += "        \"" + source + "\"\n";
        s += "    )\n";
        s += "    target_sources(" + target + " PRIVATE ${JASPERENGINE_NATIVE_PLUGIN_SOURCES})\n";
        s += "    set_source_files_properties(${JASPERENGINE_NATIVE_PLUGIN_SOURCES} PROPERTIES COMPILE_FLAGS \"-fobjc-arc\")\n";
        s += "    target_include_directories(" + target + " PRIVATE \"" + includeDir + "\")\n";
        s += "    target_link_libraries(" + target + " PRIVATE \"-framework Foundation\" \"-framework AppKit\"";
        for (std::string framework : frameworks){
            framework.erase(0, framework.find_first_not_of(" \t"));
            framework.erase(framework.find_last_not_of(" \t") + 1);
            const bool weak = !framework.empty() && framework.back() == '?';
            if (weak) framework.pop_back();
            if (framework.empty() || framework.find_first_of("\"\\ /;$") != std::string::npos) continue;
            s += weak ? " \"-weak_framework " + framework + "\"" : " \"-framework " + framework + "\"";
        }
        s += ")\n";
    }

    // the SDKs, as the Xcode target links, embeds and copies them; a name CMake would split or
    // expand is left out
    std::vector<MacosSdk> sdks;
    for (const MacosSdk& sdk : macosSdks(scan)){
        if (sdk.library.find_first_of("\"\\;$") == std::string::npos) sdks.push_back(sdk);
    }
    if (!sdks.empty()){
        s += "    # their SDKs (plugins/native/apple and macos), as the Xcode project takes them\n";
        const std::vector<std::string> frameworkDirs = nativeFrameworkDirs(sdks, rootExpression);
        for (const std::string& dir : frameworkDirs){
            s += "    target_compile_options(" + target + " PRIVATE \"-F" + dir + "\")\n";
            s += "    target_link_options(" + target + " PRIVATE \"-F" + dir + "\")\n";
        }
        std::string links;
        for (const MacosSdk& sdk : sdks){
            if (sdk.resource) continue;
            const std::string ext = extensionOf(sdk.library);
            if (!sdk.headers.empty()) s += "    target_include_directories(" + target + " PRIVATE \"" + rootExpression + "/" + sdk.headers + "\")\n";
            if (ext == ".framework") links += " \"-framework " + stemOf(sdk.library) + "\"";
            else links += " \"" + rootExpression + "/" + sdk.library + "\"";
        }
        if (!links.empty()) s += "    target_link_libraries(" + target + " PRIVATE" + links + ")\n";
        const bool anyEmbedded = std::any_of(sdks.begin(), sdks.end(), [](const MacosSdk& sdk) { return sdk.embedded; });
        if (!executable){
            // a library (the editor's Play): the dynamic ones load from where they are
            std::vector<std::string> runpaths;
            for (const MacosSdk& sdk : sdks){
                const std::string dir = rootExpression + "/" + nativeParentOf(sdk.library);
                if (sdk.embedded && std::find(runpaths.begin(), runpaths.end(), dir) == runpaths.end()) runpaths.push_back(dir);
            }
            for (const std::string& runpath : runpaths){
                s += "    target_link_options(" + target + " PRIVATE \"-Wl,-rpath," + runpath + "\")\n";
            }
        } else if (anyEmbedded || std::any_of(sdks.begin(), sdks.end(), [](const MacosSdk& sdk) { return sdk.resource; })){
            s += "    get_target_property(JASPERENGINE_NATIVE_APP " + target + " MACOSX_BUNDLE)\n";
            s += "    if(JASPERENGINE_NATIVE_APP)\n";
            s += "        # a .app (the Xcode generator): Contents/Frameworks and Contents/Resources, as Xcode's phases put them\n";
            s += "        set(JASPERENGINE_NATIVE_FRAMEWORKS_DIR \"$<TARGET_BUNDLE_CONTENT_DIR:" + target + ">/Frameworks\")\n";
            s += "        set(JASPERENGINE_NATIVE_RESOURCES_DIR \"$<TARGET_BUNDLE_CONTENT_DIR:" + target + ">/Resources\")\n";
            s += "        set(JASPERENGINE_NATIVE_RPATH \"@executable_path/../Frameworks\")\n";
            s += "    else()\n";
            s += "        # an unbundled executable: Frameworks/ beside it and the resource bundles beside it, where\n";
            s += "        # NSBundle.mainBundle looks\n";
            s += "        set(JASPERENGINE_NATIVE_FRAMEWORKS_DIR \"$<TARGET_FILE_DIR:" + target + ">/Frameworks\")\n";
            s += "        set(JASPERENGINE_NATIVE_RESOURCES_DIR \"$<TARGET_FILE_DIR:" + target + ">\")\n";
            s += "        set(JASPERENGINE_NATIVE_RPATH \"@executable_path/Frameworks\")\n";
            s += "    endif()\n";
            // a linker flag: every generator passes it (Xcode keeps its own LD_RUNPATH_SEARCH_PATHS)
            if (anyEmbedded) s += "    target_link_options(" + target + " PRIVATE \"-Wl,-rpath,${JASPERENGINE_NATIVE_RPATH}\")\n";
            if (anyEmbedded){
                // Xcode's Embed Frameworks with CodeSignOnCopy and RemoveHeadersOnCopy: a hardened,
                // team-signed app loads another team's framework only once it carries the app's
                // signature. ditto keeps a framework's Versions/Current links.
                s += "    file(WRITE \"${CMAKE_CURRENT_BINARY_DIR}/jasperengine-embed-framework.sh\" [==[set -e\n"
                     "rm -rf \"$2\"\n"
                     "mkdir -p \"$(dirname \"$2\")\"\n"
                     "ditto \"$1\" \"$2\"\n"
                     "find \"$2\" \\( -name Headers -o -name PrivateHeaders \\) -prune -exec rm -rf {} +\n"
                     "codesign --force --sign \"${EXPANDED_CODE_SIGN_IDENTITY:-${JASPER_MACOS_SIGN_IDENTITY:--}}\" "
                     "--preserve-metadata=identifier,entitlements,flags --timestamp=none \"$2\"\n"
                     "]==])\n";
            }
            s += "    add_custom_command(TARGET " + target + " POST_BUILD\n";
            for (const MacosSdk& sdk : sdks){
                if (!sdk.embedded && !sdk.resource) continue;
                const std::string from = "\"" + rootExpression + "/" + sdk.library + "\"";
                if (sdk.embedded){
                    s += "        COMMAND /bin/sh \"${CMAKE_CURRENT_BINARY_DIR}/jasperengine-embed-framework.sh\" " + from +
                         " \"${JASPERENGINE_NATIVE_FRAMEWORKS_DIR}/" + nativeNameOf(sdk.library) + "\"\n";
                    continue;
                }
                const std::string to = "\"${JASPERENGINE_NATIVE_RESOURCES_DIR}/" + nativeNameOf(sdk.library) + "\"";
                s += "        COMMAND \"${CMAKE_COMMAND}\" -E rm -rf " + to + "\n";
                s += "        COMMAND ditto " + from + " " + to + "\n";
            }
            s += "        VERBATIM)\n";
        }
    }
    s += "endif()\n";
    return s;
}

bool NativePlugins::isStaticArchive(const fs::path& binary){
    std::ifstream in(binary, std::ios::binary);
    char magic[8] = {0};
    in.read(magic, 8);
    return in.gcount() == 8 && std::memcmp(magic, "!<arch>\n", 8) == 0;
}

fs::path NativePlugins::frameworkBinary(const fs::path& framework){
    const std::string name = framework.stem().string();
    std::error_code ec;
    for (const fs::path& candidate : {framework / name, framework / "Versions" / "Current" / name, framework / "Versions" / "A" / name}) {
        if (fs::is_regular_file(candidate, ec)) return candidate;
    }
    return {};
}

fs::path NativePlugins::xcframeworkBinary(const fs::path& xcframework){
    const std::vector<NativeXcframeworkLibrary> libraries = nativeXcframeworkLibraries(xcframework);
    if (libraries.empty()) return {};
    const NativeXcframeworkLibrary& first = libraries.front();
    const fs::path base = xcframework / first.identifier;
    if (!first.binary.empty()) return base / first.binary;
    if (fs::path(first.library).extension() == ".framework") return frameworkBinary(base / first.library);
    return base / first.library;
}

fs::path NativePlugins::xcframeworkLibrary(const fs::path& xcframework, const std::string& platform, fs::path* headers){
    for (const NativeXcframeworkLibrary& library : nativeXcframeworkLibraries(xcframework)){
        if (library.platform != platform || !library.variant.empty()) continue;
        if (headers) *headers = library.headers.empty() ? fs::path() : fs::path(library.identifier) / library.headers;
        return fs::path(library.identifier) / library.library;
    }
    if (headers) headers->clear();
    return {};
}

std::string NativePlugins::xmlFragmentError(const std::string& xml){
    if (xml.find_first_not_of(" \t\r\n") == std::string::npos) return std::string();
    if (xml.find("<?xml") != std::string::npos) return "a fragment has no <?xml ...?> declaration (only the elements)";
    // the fragment as the content of one element; the first line of the wrapper is the
    // fragment's first line
    const std::string wrapped = "<fragment>" + xml + "\n</fragment>";
    tinyxml2::XMLDocument doc;
    if (doc.Parse(wrapped.c_str(), wrapped.size()) != tinyxml2::XML_SUCCESS){
        std::string error = doc.ErrorStr() ? doc.ErrorStr() : "malformed XML";
        // tinyxml2 says "Error=XML_ERROR_MISMATCHED_ELEMENT ErrorID=14 (0xe) Line number=2: ..."
        const size_t line = error.find("Line number=");
        if (line != std::string::npos) error = "line " + error.substr(line + 12);
        return "not well-formed XML: " + error;
    }
    return std::string();
}

// ---------------------------------------------------------------- scaffolding

namespace {

    bool wants(const NativePlugins::CreateOptions& options, const char* platform){
        return std::find(options.platforms.begin(), options.platforms.end(), platform) != options.platforms.end();
    }

    std::string packagePath(const std::string& package){
        std::string path = package;
        std::replace(path.begin(), path.end(), '.', '/');
        return path;
    }

    // where the Objective-C/Swift files go: apple/ when both Apple platforms want them
    std::string appleFolder(const NativePlugins::CreateOptions& options){
        const bool ios = wants(options, "ios");
        const bool macos = wants(options, "macos");
        if (options.appleLanguage == "swift" || (ios && macos)) return "apple";
        return ios ? "ios" : "macos";
    }

    std::string javaSource(const NativePlugins::CreateOptions& o){
        std::string s;
        s += "package " + o.package + ";\n\n";
        const bool anyAsync = std::any_of(o.methods.begin(), o.methods.end(), [](const NativePlugins::MethodSpec& m) { return m.async; });
        s += "import android.app.Activity;\n\n";
        if (anyAsync){
            s += "import com.jaspergameengine.bridge.JasperNative;\n";
            s += "import com.jaspergameengine.bridge.NativeCall;\n\n";
        }
        s += "// The Android side of the native plugin " + o.name + " (docs/mcp/guide/native-bridge.md).\n";
        s += "// Lua: Native.call(\"" + o.name + "\", \"method\", ...) runs a public static method of that name\n";
        s += "// with one parameter per Lua argument (boolean, int, long, double, String, JSONObject,\n";
        s += "// JSONArray, Object...) and returns a JSON-compatible value (null, Boolean, Number, String,\n";
        s += "// JSONObject, JSONArray, Map, List). Native.callAsync adds a trailing NativeCall parameter:\n";
        s += "// complete it later, from any thread, with call.resolve(value) or call.reject(code, message).\n";
        s += "// JasperNative.getActivity(), runOnUiThread, emit(\"" + o.name + "\", \"event\", payload) and the\n";
        s += "// lifecycle listeners reach the rest of Android.\n";
        s += "public class " + o.name + " {\n\n";
        s += "\t// Once, on the UI thread, after the activity is created (SDK initialization)\n";
        s += "\tpublic static void onJasperInit(Activity activity) {\n";
        s += "\t}\n";
        for (const NativePlugins::MethodSpec& m : o.methods){
            s += "\n";
            if (m.async){
                s += "\t// Native.callAsync(\"" + o.name + "\", \"" + m.name + "\", {}, function(result, err, code) end)\n";
                s += "\tpublic static void " + m.name + "(final NativeCall call) {\n";
                s += "\t\tJasperNative.runOnUiThread(new Runnable() {\n";
                s += "\t\t\t@Override public void run() {\n";
                s += "\t\t\t\tcall.resolve(null);\n";
                s += "\t\t\t}\n";
                s += "\t\t});\n";
                s += "\t}\n";
            } else {
                s += "\t// Native.call(\"" + o.name + "\", \"" + m.name + "\")\n";
                s += "\tpublic static Object " + m.name + "() {\n";
                s += "\t\treturn null;\n";
                s += "\t}\n";
            }
        }
        s += "}\n";
        return s;
    }

    std::string kotlinSource(const NativePlugins::CreateOptions& o){
        std::string s;
        s += "package " + o.package + "\n\n";
        const bool anyAsync = std::any_of(o.methods.begin(), o.methods.end(), [](const NativePlugins::MethodSpec& m) { return m.async; });
        s += "import android.app.Activity\n";
        if (anyAsync){
            s += "import com.jaspergameengine.bridge.JasperNative\n";
            s += "import com.jaspergameengine.bridge.NativeCall\n";
        }
        s += "\n";
        s += "// The Android side of the native plugin " + o.name + " (docs/mcp/guide/native-bridge.md).\n";
        s += "// Lua: Native.call(\"" + o.name + "\", \"method\", ...) runs the @JvmStatic function of that name\n";
        s += "// with one parameter per Lua argument (Boolean, Int, Long, Double, String, JSONObject,\n";
        s += "// JSONArray, Any?...) returning a JSON-compatible value. Native.callAsync adds a trailing\n";
        s += "// NativeCall parameter: complete it later, from any thread, with call.resolve(value) or\n";
        s += "// call.reject(code, message).\n";
        s += "object " + o.name + " {\n\n";
        s += "    // Once, on the UI thread, after the activity is created (SDK initialization)\n";
        s += "    @JvmStatic\n";
        s += "    fun onJasperInit(activity: Activity) {\n";
        s += "    }\n";
        for (const NativePlugins::MethodSpec& m : o.methods){
            s += "\n";
            if (m.async){
                s += "    // Native.callAsync(\"" + o.name + "\", \"" + m.name + "\", {}, function(result, err, code) end)\n";
                s += "    @JvmStatic\n";
                s += "    fun " + m.name + "(call: NativeCall) {\n";
                s += "        JasperNative.runOnUiThread { call.resolve(null) }\n";
                s += "    }\n";
            } else {
                s += "    // Native.call(\"" + o.name + "\", \"" + m.name + "\")\n";
                s += "    @JvmStatic\n";
                s += "    fun " + m.name + "(): Any? {\n";
                s += "        return null\n";
                s += "    }\n";
            }
        }
        s += "}\n";
        return s;
    }

    std::string objcHeader(const NativePlugins::CreateOptions& o){
        std::string s;
        s += "// The Apple side of the native plugin " + o.name + " (docs/mcp/guide/native-bridge.md)\n\n";
        s += "#import <Foundation/Foundation.h>\n";
        s += "#import \"JasperNative.h\"\n\n";
        s += "@interface " + o.name + " : NSObject <JasperNativePlugin>\n";
        s += "+ (void)jasperInit;\n";
        for (const NativePlugins::MethodSpec& m : o.methods){
            if (m.async) s += "+ (void)" + m.name + ":(NSArray*)args call:(JasperNativeCall*)call;\n";
            else s += "+ (id)" + m.name + ":(NSArray*)args;\n";
        }
        s += "@end\n";
        return s;
    }

    std::string objcSource(const NativePlugins::CreateOptions& o){
        std::string s;
        s += "// The Apple side of the native plugin " + o.name + " (docs/mcp/guide/native-bridge.md).\n";
        s += "// Lua: Native.call(\"" + o.name + "\", \"method\", ...) runs the class method `+ (id)method:(NSArray*)args`\n";
        s += "// (args: the Lua arguments as NSNull, NSNumber, NSString, NSArray, NSDictionary) returning one of\n";
        s += "// those (or nil). Native.callAsync runs `+ (void)method:(NSArray*)args call:(JasperNativeCall*)call`:\n";
        s += "// complete the call later, from any thread, with [call resolve:value] or [call reject:code message:text].\n";
        s += "// [JasperNative viewController], emit:event:payload: and the lifecycle observers reach the app.\n\n";
        s += "#import \"" + o.name + ".h\"\n\n";
        s += "@implementation " + o.name + "\n\n";
        s += "// Once, on the main thread, when the game's view has loaded (SDK initialization)\n";
        s += "+ (void)jasperInit {\n";
        s += "}\n";
        for (const NativePlugins::MethodSpec& m : o.methods){
            s += "\n";
            if (m.async){
                s += "// Native.callAsync(\"" + o.name + "\", \"" + m.name + "\", {}, function(result, err, code) end)\n";
                s += "+ (void)" + m.name + ":(NSArray*)args call:(JasperNativeCall*)call {\n";
                s += "    [call resolve:nil];\n";
                s += "}\n";
            } else {
                s += "// Native.call(\"" + o.name + "\", \"" + m.name + "\")\n";
                s += "+ (id)" + m.name + ":(NSArray*)args {\n";
                s += "    return nil;\n";
                s += "}\n";
            }
        }
        s += "\n@end\n";
        return s;
    }

    std::string swiftSource(const NativePlugins::CreateOptions& o){
        std::string s;
        s += "// The Apple side of the native plugin " + o.name + " (docs/mcp/guide/native-bridge.md).\n";
        s += "// Lua: Native.call(\"" + o.name + "\", \"method\", ...) runs `@objc static func method(_ args: [Any]) -> Any?`\n";
        s += "// (args: the Lua arguments as NSNull, NSNumber, String, [Any], [String: Any]). Native.callAsync runs\n";
        s += "// `@objc static func method(_ args: [Any], call: JasperNativeCall)`: complete the call later, from any\n";
        s += "// thread, with call.resolve(value) or call.reject(code, message: text). JasperNative (the bridging\n";
        s += "// header) gives viewController, emit and the lifecycle observers.\n\n";
        s += "import Foundation\n\n";
        s += "@objc(" + o.name + ") public class " + o.name + ": NSObject, JasperNativePlugin {\n\n";
        s += "    // Once, on the main thread, when the game's view has loaded (SDK initialization)\n";
        s += "    @objc public static func jasperInit() {\n";
        s += "    }\n";
        for (const NativePlugins::MethodSpec& m : o.methods){
            s += "\n";
            if (m.async){
                s += "    // Native.callAsync(\"" + o.name + "\", \"" + m.name + "\", {}, function(result, err, code) end)\n";
                s += "    @objc public static func " + m.name + "(_ args: [Any], call: JasperNativeCall) {\n";
                s += "        call.resolve(nil)\n";
                s += "    }\n";
            } else {
                s += "    // Native.call(\"" + o.name + "\", \"" + m.name + "\")\n";
                s += "    @objc public static func " + m.name + "(_ args: [Any]) -> Any? {\n";
                s += "        return nil\n";
                s += "    }\n";
            }
        }
        s += "}\n";
        return s;
    }

    std::string webSource(const NativePlugins::CreateOptions& o){
        std::string s;
        s += "// The Web side of the native plugin " + o.name + " (docs/mcp/guide/native-bridge.md). Loaded with a\n";
        s += "// <script> before the engine. Lua: Native.call(\"" + o.name + "\", \"method\", ...) calls the function\n";
        s += "// of that name with the Lua arguments; its return value goes back as JSON. Native.callAsync\n";
        s += "// awaits what it returns (a Promise is fine): reject with {code, message} to set the error code.\n";
        s += "// JasperNative.emit(\"" + o.name + "\", \"event\", payload) fires Native.on handlers.\n";
        s += "JasperNative.register(\"" + o.name + "\", {\n";
        for (const NativePlugins::MethodSpec& m : o.methods){
            if (m.async){
                s += "    // Native.callAsync(\"" + o.name + "\", \"" + m.name + "\", {}, function(result, err, code) end)\n";
                s += "    " + m.name + ": async function () {\n";
                s += "        return null;\n";
                s += "    },\n";
            } else {
                s += "    // Native.call(\"" + o.name + "\", \"" + m.name + "\")\n";
                s += "    " + m.name + ": function () {\n";
                s += "        return null;\n";
                s += "    },\n";
            }
        }
        s += "});\n";
        return s;
    }

    std::string cppSource(const NativePlugins::CreateOptions& o){
        // the platform plugins answer on their platforms: this one would shadow them (C++ first)
        std::vector<std::string> conditions;
        if (wants(o, "android")) conditions.push_back("defined(__ANDROID__)");
        if (wants(o, "ios")) conditions.push_back("(defined(__APPLE__) && TARGET_OS_IPHONE)");
        if (wants(o, "macos")) conditions.push_back("(defined(__APPLE__) && !TARGET_OS_IPHONE)");
        if (wants(o, "web")) conditions.push_back("defined(__EMSCRIPTEN__)");
        std::string s;
        s += "// The C++ side of the native plugin " + o.name + " (docs/mcp/guide/native-bridge.md): Lua's\n";
        s += "// Native.call(\"" + o.name + "\", \"method\", ...) runs the function registered under that name with the\n";
        s += "// arguments as a JSON array; it returns a JSON value or throws NativeError(code, message).\n";
        s += "// Async methods get a NativeCall to complete later, from any thread.\n";
        if (!conditions.empty()){
            s += "// Compiled where no platform plugin of this name exists (a C++ plugin answers first).\n";
        }
        s += "\n#include \"JasperEngine.h\"\n\n";
        std::string condition;
        if (!conditions.empty()){
            if (wants(o, "ios") || wants(o, "macos")){
                s += "#if defined(__APPLE__)\n#include <TargetConditionals.h>\n#endif\n\n";
            }
            for (const std::string& c : conditions){
                if (!condition.empty()) condition += " || ";
                condition += c;
            }
            s += "#if !(" + condition + ")\n\n";
        }
        s += "using namespace jasperengine;\n\n";
        s += "JASPERENGINE_NATIVE_PLUGIN(" + o.name + ") {\n";
        bool first = true;
        for (const NativePlugins::MethodSpec& m : o.methods){
            if (!first) s += "\n";
            first = false;
            if (m.async){
                s += "    // Native.callAsync(\"" + o.name + "\", \"" + m.name + "\", {}, function(result, err, code) end)\n";
                s += "    plugin.asyncMethod(\"" + m.name + "\", [](NativeCall call) {\n";
                s += "        call.resolve(nullptr);\n";
                s += "    });\n";
            } else {
                s += "    // Native.call(\"" + o.name + "\", \"" + m.name + "\")\n";
                s += "    plugin.method(\"" + m.name + "\", [](const NativeJson& /*args*/) -> NativeJson {\n";
                s += "        return nullptr;\n";
                s += "    });\n";
            }
        }
        s += "}\n";
        if (!conditions.empty()) s += "\n#endif\n";
        return s;
    }

    std::string mockSource(const NativePlugins::CreateOptions& o){
        std::string s;
        s += "-- Lua mock of the native plugin " + o.name + " (docs/mcp/guide/native-bridge.md): it answers\n";
        s += "-- Native.call / Native.callAsync where no real implementation exists (editor Play, desktop\n";
        s += "-- builds); a C++ or platform plugin of the same name always wins. A function gets the call's\n";
        s += "-- arguments and returns the value, or nil, message [, code] to fail. Async calls also get the\n";
        s += "-- NativeCall last: return Native.pending() and complete it later with call:resolve(value) or\n";
        s += "-- call:reject(code, message). Native.emitMock(\"" + o.name + "\", \"event\", payload) fakes an event.\n";
        s += "local " + o.name + " = {}\n";
        for (const NativePlugins::MethodSpec& m : o.methods){
            s += "\n";
            if (m.async){
                s += "-- async: the Lua arguments come first, the NativeCall last: function " + o.name + "." + m.name + "(a, b, call)\n";
                s += "function " + o.name + "." + m.name + "(call)\n";
                s += "    return nil\n";
                s += "end\n";
            } else {
                s += "function " + o.name + "." + m.name + "()\n";
                s += "    return nil\n";
                s += "end\n";
            }
        }
        s += "\nreturn " + o.name + "\n";
        return s;
    }

    struct PlannedFile {
        fs::path path;          // absolute
        std::string content;
    };

    std::vector<PlannedFile> plan(const fs::path& projectPath, const fs::path& luaPath, const NativePlugins::CreateOptions& o){
        std::vector<PlannedFile> files;
        const fs::path native = NativePlugins::root(projectPath);
        if (wants(o, "android")){
            if (o.androidLanguage == "kotlin"){
                files.push_back({native / "android" / "kotlin" / packagePath(o.package) / (o.name + ".kt"), kotlinSource(o)});
            } else {
                files.push_back({native / "android" / "java" / packagePath(o.package) / (o.name + ".java"), javaSource(o)});
            }
        }
        if (wants(o, "ios") || wants(o, "macos")){
            const fs::path dir = native / appleFolder(o);
            if (o.appleLanguage == "swift"){
                files.push_back({dir / (o.name + ".swift"), swiftSource(o)});
            } else {
                files.push_back({dir / (o.name + ".h"), objcHeader(o)});
                files.push_back({dir / (o.name + ".m"), objcSource(o)});
            }
        }
        if (wants(o, "web")) files.push_back({native / "web" / (lower(o.name) + ".js"), webSource(o)});
        if (wants(o, "cpp")) files.push_back({native / "cpp" / (o.name + "Plugin.cpp"), cppSource(o)});
        files.push_back({luaPath / "native_mocks" / (o.name + ".lua"), mockSource(o)});
        return files;
    }

    std::string projectRelative(const fs::path& projectPath, const fs::path& path){
        const fs::path relative = path.lexically_normal().lexically_relative(projectPath.lexically_normal());
        return (relative.empty() || *relative.begin() == "..") ? path.generic_string() : relative.generic_string();
    }

}

std::string NativePlugins::validate(const CreateOptions& o){
    if (!isIdentifier(o.name)) return "name must be an identifier (letters, digits, _; not starting with a digit), e.g. \"Device\"";
    if (javaKeywords().count(o.name)) return "name \"" + o.name + "\" is a reserved word";
    if (o.platforms.empty()) return "platforms must list at least one of android, ios, macos, web, cpp";
    for (const std::string& platform : o.platforms){
        if (platform != "android" && platform != "ios" && platform != "macos" && platform != "web" && platform != "cpp"){
            return "unknown platform \"" + platform + "\" (android, ios, macos, web, cpp)";
        }
    }
    if (o.androidLanguage != "java" && o.androidLanguage != "kotlin") return "android_language must be \"java\" or \"kotlin\"";
    if (o.appleLanguage != "objc" && o.appleLanguage != "swift") return "apple_language must be \"objc\" or \"swift\"";
    if (wants(o, "android") && !isPackageName(o.package)) return "package must be a Java package name, e.g. \"com.example.plugins\"";
    if (wants(o, "android")){
        size_t start = 0;
        while (true){
            const size_t dot = o.package.find('.', start);
            const std::string part = o.package.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
            if (javaKeywords().count(part)) return "package part \"" + part + "\" is a reserved word";
            if (dot == std::string::npos) break;
            start = dot + 1;
        }
    }
    std::set<std::string> seen;
    for (const MethodSpec& m : o.methods){
        if (!isIdentifier(m.name)) return "method name \"" + m.name + "\" is not an identifier";
        if (javaKeywords().count(m.name)) return "method name \"" + m.name + "\" is a reserved word";
        if (m.name == "onJasperInit" || m.name == "jasperInit") return "\"" + m.name + "\" is the initialization hook every plugin has";
        if (!seen.insert(m.name).second) return "method \"" + m.name + "\" is listed twice";
    }
    return std::string();
}

std::vector<std::string> NativePlugins::plannedFiles(const fs::path& projectPath, const fs::path& luaPath, const CreateOptions& options){
    std::vector<std::string> out;
    for (const PlannedFile& file : plan(projectPath, luaPath, options)) out.push_back(projectRelative(projectPath, file.path));
    return out;
}

bool NativePlugins::create(const fs::path& projectPath, const fs::path& luaPath, const CreateOptions& options,
                           std::vector<CreatedFile>& written, std::string& error){
    written.clear();
    error = validate(options);
    if (!error.empty()) return false;
    const std::vector<PlannedFile> files = plan(projectPath, luaPath, options);
    std::vector<bool> existed;
    std::error_code ec;
    for (const PlannedFile& file : files){
        const bool exists = fs::exists(file.path, ec);
        if (exists && !options.overwrite){
            error = projectRelative(projectPath, file.path) + " already exists (overwrite: true replaces the plugin's files)";
            return false;
        }
        existed.push_back(exists);
    }
    // the Lua folder may sit outside the project: each file is written beneath its own root
    for (size_t i = 0; i < files.size(); i++){
        const PlannedFile& file = files[i];
        const bool isMock = i + 1 == files.size();
        const fs::path writeRoot = isMock ? luaPath : projectPath;
        if (!safewrite::writeFileResolvedBeneath(writeRoot, file.path, file.content, error)){
            return false;
        }
        written.push_back({projectRelative(projectPath, file.path), existed[i]});
    }
    return true;
}
