// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "NativeExportAndroid.h"

#include "util/FileUtils.h"
#include "util/NativePlugins.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <map>
#include <set>

using namespace jasperengine::editor;

namespace {

    namespace fs = std::filesystem;

    bool readText(const fs::path& path, std::string& out){
        std::ifstream in(path, std::ios::binary);
        if (!in) return false;
        out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        // the templates are LF; a CRLF checkout must not double them
        out.erase(std::remove(out.begin(), out.end(), '\r'), out.end());
        return true;
    }

    void replaceFirst(std::string& text, const std::string& from, const std::string& to){
        const size_t at = text.find(from);
        if (at != std::string::npos) text.replace(at, from.size(), to);
    }

    std::string gradleString(const std::string& value){
        std::string out;
        for (char c : value){
            if (c == '\\' || c == '"' || c == '$') out += '\\';
            out += c;
        }
        return out;
    }

    // ---- a tiny tokenizer: identifiers and single punctuation characters, comments and
    // string/char literals removed

    struct Token {
        std::string text;
        bool identifier = false;
    };

    std::vector<Token> tokenize(const std::string& s, bool kotlin){
        std::vector<Token> tokens;
        size_t i = 0;
        const size_t n = s.size();
        while (i < n){
            const char c = s[i];
            if (c == '/' && i + 1 < n && s[i + 1] == '/'){
                while (i < n && s[i] != '\n') i++;
                continue;
            }
            if (c == '/' && i + 1 < n && s[i + 1] == '*'){
                // Kotlin block comments nest
                int depth = 0;
                while (i < n){
                    if (s.compare(i, 2, "/*") == 0){ depth++; i += 2; continue; }
                    if (s.compare(i, 2, "*/") == 0){ i += 2; if (--depth == 0 || !kotlin) break; continue; }
                    i++;
                }
                continue;
            }
            if (kotlin && s.compare(i, 3, "\"\"\"") == 0){
                const size_t end = s.find("\"\"\"", i + 3);
                i = end == std::string::npos ? n : end + 3;
                continue;
            }
            if (c == '"' || c == '\''){
                i++;
                while (i < n && s[i] != c && s[i] != '\n'){
                    if (s[i] == '\\') i++;
                    i++;
                }
                i++;
                continue;
            }
            if (std::isalpha((unsigned char)c) || c == '_' || c == '$'){
                size_t start = i;
                while (i < n && (std::isalnum((unsigned char)s[i]) || s[i] == '_' || s[i] == '$')) i++;
                tokens.push_back({s.substr(start, i - start), true});
                continue;
            }
            if (!std::isspace((unsigned char)c)) tokens.push_back({std::string(1, c), false});
            i++;
        }
        return tokens;
    }

    std::string lowerExtension(const fs::path& path){
        std::string ext = path.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return ext;
    }

}

std::vector<NativeExportAndroid::PluginClass> NativeExportAndroid::parseSource(const std::string& source, bool kotlin, const std::string& file){
    std::vector<PluginClass> out;
    const std::vector<Token> tokens = tokenize(source, kotlin);
    std::string packageName;
    int depth = 0;
    std::set<std::string> modifiers;
    for (size_t i = 0; i < tokens.size(); i++){
        const Token& t = tokens[i];
        if (!t.identifier){
            if (t.text == "{") depth++;
            else if (t.text == "}") depth = std::max(0, depth - 1);
            if (depth == 0 && (t.text == ";" || t.text == "}" || t.text == "=")) modifiers.clear();
            if (t.text == "@" && i + 1 < tokens.size()) i++;   // an annotation's name (and @interface)
            continue;
        }
        if (depth != 0) continue;
        if (t.text == "package"){
            std::string name;
            size_t j = i + 1;
            while (j < tokens.size() && (tokens[j].identifier || tokens[j].text == ".")){
                // a Kotlin package line ends at the line break: the next declaration starts
                // with a keyword after an identifier without a dot
                if (tokens[j].identifier && !name.empty() && name.back() != '.') break;
                name += tokens[j].text;
                j++;
            }
            packageName = name;
            i = j - 1;
            modifiers.clear();
            continue;
        }
        if (t.text == "import"){
            size_t j = i + 1;
            std::string last;
            while (j < tokens.size() && (tokens[j].identifier || tokens[j].text == "." || tokens[j].text == "*")){
                if (tokens[j].identifier && !last.empty() && last != "." && last != "*") break;
                last = tokens[j].text;
                j++;
            }
            i = j - 1;
            modifiers.clear();
            continue;
        }
        const bool declaration = t.text == "class" || t.text == "interface" || (!kotlin && (t.text == "enum" || t.text == "record")) ||
                                 (kotlin && t.text == "object");
        if (declaration){
            // `enum class X` (Kotlin), `fun interface X`: the keyword after
            if (i + 1 < tokens.size() && tokens[i + 1].identifier && (tokens[i + 1].text == "class" || tokens[i + 1].text == "interface")){
                continue;
            }
            if (i + 1 >= tokens.size() || !tokens[i + 1].identifier){
                modifiers.clear();
                continue;
            }
            const std::string name = tokens[i + 1].text;
            const bool visible = kotlin ? (!modifiers.count("private") && !modifiers.count("internal")) : modifiers.count("public") != 0;
            if (visible && !modifiers.count("companion")){
                PluginClass cls;
                cls.simpleName = name;
                cls.packageName = packageName;
                cls.qualifiedName = packageName.empty() ? name : packageName + "." + name;
                cls.file = file;
                out.push_back(cls);
            }
            modifiers.clear();
            i++;
            continue;
        }
        // a member that is not a type ends the modifiers before it (Kotlin has no ';': without
        // this, `private lateinit var x` would make the next `object` private)
        if (kotlin && (t.text == "val" || t.text == "var" || t.text == "fun" || t.text == "typealias")){
            modifiers.clear();
            continue;
        }
        modifiers.insert(t.text);
    }
    return out;
}

bool NativeExportAndroid::collectClasses(const fs::path& androidDir, std::vector<PluginClass>& out, std::string& error){
    out.clear();
    std::vector<fs::path> sources;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(androidDir, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)){
        const std::string name = it->path().filename().string();
        const bool directory = it->is_directory(ec);
        if (name.empty() || name[0] == '.' || (directory && (name == "build" || name == "res" || name == "libs"))){
            if (directory) it.disable_recursion_pending();
            continue;
        }
        const std::string ext = lowerExtension(it->path());
        if (!directory && (ext == ".java" || ext == ".kt")) sources.push_back(it->path());
    }
    std::sort(sources.begin(), sources.end());
    std::map<std::string, std::string> seen;   // simple name -> file
    for (const fs::path& path : sources){
        std::string text;
        if (!readText(path, text)){
            error = "Cannot read the native plugin source " + path.generic_string();
            return false;
        }
        const std::string relative = path.lexically_relative(androidDir).generic_string();
        for (PluginClass& cls : parseSource(text, lowerExtension(path) == ".kt", relative)){
            auto [it, inserted] = seen.emplace(cls.simpleName, relative);
            if (!inserted){
                error = "Two native plugin classes are named " + cls.simpleName + ": plugins/native/android/" + it->second +
                        " and plugins/native/android/" + relative + ". Lua calls a plugin by that name: rename one class";
                return false;
            }
            out.push_back(std::move(cls));
        }
    }
    std::sort(out.begin(), out.end(), [](const PluginClass& a, const PluginClass& b) { return a.simpleName < b.simpleName; });
    return true;
}

std::string NativeExportAndroid::registrySource(const std::vector<PluginClass>& classes){
    std::string s;
    s += "// Written by the export from plugins/native/android: Native.call(\"Name\", ...) from Lua reaches\n";
    s += "// the class registered under Name (JasperNative reads this map by reflection).\n";
    s += "package com.jaspergameengine.nativeplugins;\n\n";
    s += "import java.util.Collections;\n";
    s += "import java.util.LinkedHashMap;\n";
    s += "import java.util.Map;\n\n";
    s += "public final class JasperNativeRegistry {\n";
    s += "    public static final Map<String, String> CLASSES;\n\n";
    s += "    static {\n";
    s += "        Map<String, String> classes = new LinkedHashMap<>();\n";
    for (const PluginClass& cls : classes){
        s += "        classes.put(\"" + cls.simpleName + "\", \"" + cls.qualifiedName + "\");\n";
    }
    s += "        CLASSES = Collections.unmodifiableMap(classes);\n";
    s += "    }\n\n";
    s += "    private JasperNativeRegistry() {}\n";
    s += "}\n";
    return s;
}

std::string NativeExportAndroid::keepRules(const std::vector<PluginClass>& classes, const std::string& userRules){
    std::string s;
    s += "# Written by the export: native plugin classes are reached by reflection (Native.call)\n";
    s += "-keep class com.jaspergameengine.** { *; }\n";
    std::set<std::string> packages;
    std::set<std::string> loose;
    for (const PluginClass& cls : classes){
        if (cls.packageName.empty()) loose.insert(cls.simpleName);
        else packages.insert(cls.packageName);
    }
    for (const std::string& package : packages) s += "-keep class " + package + ".** { *; }\n";
    for (const std::string& name : loose) s += "-keep class " + name + " { *; }\n";
    if (!userRules.empty()){
        s += "\n# plugins/native/android/proguard-rules.pro\n";
        s += userRules;
        if (userRules.back() != '\n') s += "\n";
    }
    return s;
}

std::string NativeExportAndroid::dependencyLine(const std::string& entry, const std::string& configuration){
    std::string text = entry;
    text.erase(0, text.find_first_not_of(" \t"));
    text.erase(text.find_last_not_of(" \t") + 1);
    // group:name:version[:classifier][@type], no spaces or quotes
    const bool coordinate = std::count(text.begin(), text.end(), ':') >= 2 && text.find_first_of(" \t\"'(){}") == std::string::npos;
    if (coordinate) return configuration + " \"" + gradleString(text) + "\"";
    return text;
}

std::string NativeExportAndroid::withGradleProperties(const std::string& text, const std::vector<std::string>& lines){
    std::vector<std::string> out;
    size_t start = 0;
    while (start < text.size()){
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        out.push_back(text.substr(start, end - start));
        start = end + 1;
    }
    auto keyOf = [](const std::string& line) {
        const size_t eq = line.find_first_of("=:");
        std::string key = line.substr(0, eq);
        key.erase(0, key.find_first_not_of(" \t"));
        key.erase(key.find_last_not_of(" \t") + 1);
        return key;
    };
    for (const std::string& line : lines){
        const std::string key = keyOf(line);
        if (key.empty() || line.find('=') == std::string::npos) continue;
        bool replaced = false;
        for (std::string& existing : out){
            if (!existing.empty() && existing[0] != '#' && keyOf(existing) == key){
                existing = line;
                replaced = true;
                break;
            }
        }
        if (!replaced) out.push_back(line);
    }
    std::string result;
    for (const std::string& line : out){
        result += line;
        result += '\n';
    }
    return result;
}

bool NativeExportAndroid::write(const fs::path& workspaceDir, const fs::path& exportProjectRoot, const AndroidProjectSettings& settings,
                                std::string& appGradle, Result& result, std::string& error){
    result = Result();
    const std::string compileSdk = std::to_string(settings.targetSdk);
    const std::string minSdk = std::to_string(settings.minSdk);

    // the bridge module: the app's SDK levels
    {
        const fs::path path = workspaceDir / "jasperbridge" / "build.gradle";
        std::string gradle;
        if (!readText(path, gradle) || gradle.find("compileSdk 36") == std::string::npos || gradle.find("minSdkVersion 21") == std::string::npos){
            error = "Incompatible Android Gradle export template: jasperbridge/build.gradle is missing or has no compileSdk 36 / minSdkVersion 21";
            return false;
        }
        replaceFirst(gradle, "compileSdk 36", "compileSdk " + compileSdk);
        replaceFirst(gradle, "minSdkVersion 21", "minSdkVersion " + minSdk);
        FileUtils::writeIfChanged(path, gradle);
    }

    const fs::path pluginsDir = exportProjectRoot / NativePlugins::relativeRoot() / "android";
    std::error_code ec;
    const bool hasPlugins = fs::is_directory(pluginsDir, ec);
    std::vector<PluginClass> classes;
    if (hasPlugins && !collectClasses(pluginsDir, classes, error)) return false;
    const bool hasLibs = hasPlugins && fs::is_directory(pluginsDir / "libs", ec);
    const bool module = hasPlugins || !settings.gradleDependencies.empty();
    // from a module folder (workspaces/androidstudio/<module>) to the exported plugins
    const std::string pluginsRelative = "../../../project/plugins/native/android";

    if (module){
        const fs::path moduleDir = workspaceDir / "nativeplugins";
        const fs::path generated = moduleDir / "generated";
        std::string userRules;
        if (hasPlugins) readText(pluginsDir / "proguard-rules.pro", userRules);
        const bool pluginManifest = hasPlugins && fs::is_regular_file(pluginsDir / "AndroidManifest.xml", ec);
        const bool hasRes = hasPlugins && fs::is_directory(pluginsDir / "res", ec);

        FileUtils::writeIfChanged(generated / "com" / "jaspergameengine" / "nativeplugins" / "JasperNativeRegistry.java", registrySource(classes));
        FileUtils::writeIfChanged(generated / "consumer-rules.pro", keepRules(classes, userRules));
        if (!pluginManifest){
            FileUtils::writeIfChanged(generated / "AndroidManifest.xml", "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<manifest />\n");
        }

        std::string g;
        g += "// Written by the export from plugins/native/android and Project Settings > Native Plugins\n";
        g += "// (android.gradle_dependencies): the native plugins' sources, resources, manifest and SDKs\n";
        g += "apply plugin: 'com.android.library'\n\n";
        g += "def pluginsRoot = \"" + pluginsRelative + "\"\n";
        g += "def pluginsRootAbs = file(pluginsRoot).toString()\n\n";
        g += "android {\n";
        g += "    compileSdk " + compileSdk + "\n";
        g += "    defaultConfig {\n";
        g += "        minSdkVersion " + minSdk + "\n";
        g += "        consumerProguardFiles 'generated/consumer-rules.pro'\n";
        g += "    }\n\n";
        g += "    sourceSets {\n";
        g += "        main {\n";
        // the whole folder is a source root: java/, kotlin/ or any other layout
        if (hasPlugins){
            g += "            java.srcDirs = [pluginsRootAbs, 'generated']\n";
            g += "            kotlin.srcDirs = [pluginsRootAbs]\n";
        } else {
            g += "            java.srcDirs = ['generated']\n";
        }
        if (hasRes) g += "            res.srcDirs = [pluginsRootAbs+'/res']\n";
        g += pluginManifest ? "            manifest.srcFile pluginsRootAbs+'/AndroidManifest.xml'\n" : "            manifest.srcFile 'generated/AndroidManifest.xml'\n";
        g += "        }\n";
        g += "    }\n\n";
        g += "    namespace = 'com.jaspergameengine.nativeplugins'\n";
        g += "}\n\n";
        g += "dependencies {\n";
        g += "    api project(':jasperbridge')\n";
        for (const std::string& dependency : settings.gradleDependencies){
            if (dependency.find_first_not_of(" \t") == std::string::npos) continue;
            g += "    " + dependencyLine(dependency, "api") + "\n";
        }
        // local AARs ship in the app module (a library's AAR cannot hold them)
        if (hasLibs) g += "    compileOnly fileTree(dir: pluginsRootAbs+'/libs', include: ['*.jar', '*.aar'])\n";
        g += "}\n";
        FileUtils::writeIfChanged(moduleDir / "build.gradle", g);

        const fs::path settingsPath = workspaceDir / "settings.gradle";
        std::string settingsGradle;
        if (!readText(settingsPath, settingsGradle) || settingsGradle.find("include ':jasperbridge'") == std::string::npos){
            error = "Incompatible Android Gradle export template: settings.gradle has no include ':jasperbridge'";
            return false;
        }
        if (settingsGradle.find("include ':nativeplugins'") == std::string::npos){
            replaceFirst(settingsGradle, "include ':jasperbridge'", "include ':jasperbridge'\ninclude ':nativeplugins'");
        }
        FileUtils::writeIfChanged(settingsPath, settingsGradle);

        const std::string bridgeLine = "    implementation project(':jasperbridge')\n";
        if (appGradle.find(bridgeLine) == std::string::npos){
            error = "Incompatible Android Gradle export template: app/build.gradle has no implementation project(':jasperbridge')";
            return false;
        }
        std::string lines = bridgeLine + "    // the native plugins (plugins/native/android, Project Settings > Native Plugins)\n"
                                         "    implementation project(':nativeplugins')\n";
        if (hasLibs) lines += "    implementation fileTree(dir: file(\"" + pluginsRelative + "/libs\").toString(), include: ['*.jar', '*.aar'])\n";
        replaceFirst(appGradle, bridgeLine, lines);
    }

    // Maven repositories of the SDKs
    std::vector<std::string> repositories;
    for (const std::string& url : settings.gradleRepositories){
        if (url.find_first_not_of(" \t") != std::string::npos) repositories.push_back(url);
    }
    if (!repositories.empty()){
        const fs::path rootGradlePath = workspaceDir / "build.gradle";
        std::string rootGradle;
        const std::string marker = "allprojects {\n    repositories {\n        google()\n        mavenCentral()\n";
        if (!readText(rootGradlePath, rootGradle) || rootGradle.find(marker) == std::string::npos){
            error = "Incompatible Android Gradle export template: build.gradle has no allprojects.repositories with google() and mavenCentral()";
            return false;
        }
        std::string added = marker;
        for (const std::string& url : repositories) added += "        maven { url \"" + gradleString(url) + "\" }\n";
        replaceFirst(rootGradle, marker, added);
        FileUtils::writeIfChanged(rootGradlePath, rootGradle);
    }

    if (!settings.gradleProperties.empty()){
        const fs::path propertiesPath = workspaceDir / "gradle.properties";
        std::string properties;
        readText(propertiesPath, properties);
        FileUtils::writeIfChanged(propertiesPath, withGradleProperties(properties, settings.gradleProperties));
    }

    // the manifest: permissions by full name, then the raw fragments
    std::set<std::string> written;
    for (const std::string& permission : settings.extraPermissions){
        std::string name = permission;
        name.erase(0, name.find_first_not_of(" \t"));
        name.erase(name.find_last_not_of(" \t") + 1);
        if (name.empty() || !written.insert(name).second) continue;
        if (name.find_first_of("\"<>&") != std::string::npos){
            error = "android.extra_permissions: \"" + name + "\" is not a permission name";
            return false;
        }
        result.permissionsXml += "    <uses-permission android:name=\"" + name + "\" />\n";
    }
    const std::string manifestError = NativePlugins::xmlFragmentError(settings.manifestXml);
    if (!manifestError.empty()){
        error = "Project Settings > Native Plugins > Android > Manifest XML (android.manifest_xml) is " + manifestError;
        return false;
    }
    const std::string applicationError = NativePlugins::xmlFragmentError(settings.manifestApplicationXml);
    if (!applicationError.empty()){
        error = "Project Settings > Native Plugins > Android > Application XML (android.manifest_application_xml) is " + applicationError;
        return false;
    }
    auto indented = [](const std::string& xml, const std::string& indent) {
        std::string out;
        size_t start = 0;
        std::string text = xml;
        text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
        while (start < text.size()){
            size_t end = text.find('\n', start);
            if (end == std::string::npos) end = text.size();
            const std::string line = text.substr(start, end - start);
            if (line.find_first_not_of(" \t") != std::string::npos) out += indent + line + "\n";
            start = end + 1;
        }
        return out;
    };
    result.manifestXml = indented(settings.manifestXml, "    ");
    result.applicationXml = indented(settings.manifestApplicationXml, "        ");
    result.needsToolsNamespace = settings.manifestXml.find("tools:") != std::string::npos ||
                                 settings.manifestApplicationXml.find("tools:") != std::string::npos;
    return true;
}
