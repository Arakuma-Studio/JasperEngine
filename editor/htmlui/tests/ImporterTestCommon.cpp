// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ImporterTestCommon.h"

#include "htmlui/ImportManifestWriter.h"
#include "htmlui/StylesheetImporter.h"

#include "htmlui/compat/CompatApi.h"
#include "htmlui/compat/UrlResolver.h"

#include "json.hpp"
#include "util/FileUtils.h"

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>
#include <regex>
#include <sstream>

#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace jasperengine;
using namespace jasperengine::editor::htmlui;
using namespace jasperengine::editor::htmlui::test;

namespace {

    std::string lower(std::string text){
        for (char& c : text){
            if (c >= 'A' && c <= 'Z')
                c = (char)(c - 'A' + 'a');
        }
        return text;
    }

    class ImporterEnvironment : public ::testing::Environment {
    public:
        void SetUp() override {
            ASSERT_TRUE(HttpClient::available()) << "the curl executable is needed by the importer tests";
            ASSERT_TRUE(sharedServer().running()) << "tools/htmlui/fixture_server.py did not start";
            HttpClient::setDefaultHostRewrite(sharedServer().rewrites());
            HttpClient::setDefaultTimeoutSeconds(20);
            if (!htmlStageAvailable())
                setReferenceCollector(&minimalHtmlReferences);
        }

        void TearDown() override {
            setReferenceCollector({});
            HttpClient::setDefaultHostRewrite({});
            sharedServer().stop();
        }
    };

    const ::testing::Environment* const environment = ::testing::AddGlobalTestEnvironment(new ImporterEnvironment());

}

fs::path test::fixtureDir(){
    return editor::FileUtils::pathFromUtf8(JASPER_EDITOR_TEST_FIXTURES);
}

fs::path test::sourceDir(){
    return editor::FileUtils::pathFromUtf8(JASPER_EDITOR_TEST_SOURCE_DIR);
}

// ---------------------------------------------------------------------------------------------
// FixtureServer
// ---------------------------------------------------------------------------------------------

FixtureServer::FixtureServer(){
    int pipeFds[2];
    if (pipe(pipeFds) != 0)
        return;
    const std::string python = JASPER_EDITOR_TEST_PYTHON;
    const std::string script = (sourceDir() / "tools/htmlui/fixture_server.py").string();
    const std::string root = fixtureDir().string();

    pid = fork();
    if (pid == 0){
        dup2(pipeFds[1], STDOUT_FILENO);
        close(pipeFds[0]);
        close(pipeFds[1]);
        execl(python.c_str(), python.c_str(), script.c_str(), "--root", root.c_str(), "--port", "0", (char*)nullptr);
        _exit(127);
    }
    close(pipeFds[1]);
    if (pid < 0){
        close(pipeFds[0]);
        return;
    }

    // the first line is the port
    std::string line;
    pollfd poller{pipeFds[0], POLLIN, 0};
    while (line.find('\n') == std::string::npos && poll(&poller, 1, 15000) > 0){
        char buffer[64];
        const ssize_t count = read(pipeFds[0], buffer, sizeof(buffer));
        if (count <= 0)
            break;
        line.append(buffer, (size_t)count);
    }
    close(pipeFds[0]);
    port = std::atoi(line.c_str());
    if (port <= 0)
        stop();
}

FixtureServer::~FixtureServer(){
    stop();
}

void FixtureServer::stop(){
    if (pid > 0){
        kill(pid, SIGTERM);
        int status = 0;
        while (waitpid(pid, &status, 0) == -1 && errno == EINTR){
        }
    }
    pid = -1;
}

std::string FixtureServer::base() const{
    return "http://127.0.0.1:" + std::to_string(port);
}

std::map<std::string, std::string> FixtureServer::rewrites() const{
    const std::string server = base();
    return {
        {"https://fonts.googleapis.com", server + "/gf"},
        {"https://fonts.gstatic.com", server + "/gstatic"},
        {"https://fonts.google.com/download", server + "/gfdl/download"},
        {"https://fonts.google.com/metadata", server + "/gfmeta/metadata"},
        {"https://cdnjs.cloudflare.com", server + "/cdn"},
        {"https://images.example.com", server + "/img"},
        {"https://lottie.host", server + "/lottie"},
        {"https://lottiefiles.com", server + "/lfpage"},
        {"https://status.example.com", server + "/status"},
        {"https://slow.example.com", server + "/slow"},
        {"https://redirect.example.com", server + "/redirect"},
    };
}

std::string FixtureServer::fetch(const std::string& path) const{
    HttpClient client;
    client.setHostRewrite({});
    const HttpResponse response = client.get(base() + path);
    std::string body;
    HttpClient::readBody(response, body);
    return body;
}

std::vector<std::pair<std::string, std::string>> FixtureServer::requests() const{
    std::vector<std::pair<std::string, std::string>> list;
    const nlohmann::json json = nlohmann::json::parse(fetch("/__requests"), nullptr, false);
    if (!json.is_array())
        return list;
    for (const nlohmann::json& item : json)
        list.emplace_back(item.value("path", ""), item.value("ua", ""));
    return list;
}

void FixtureServer::reset() const{
    fetch("/__reset");
}

FixtureServer& test::sharedServer(){
    static FixtureServer server;
    return server;
}

// ---------------------------------------------------------------------------------------------
// files, projects, environment
// ---------------------------------------------------------------------------------------------

fs::path test::makeTempDir(const std::string& prefix){
    std::random_device device;
    std::ostringstream name;
    name << prefix << std::hex << device() << device();
    const fs::path dir = fs::temp_directory_path() / name.str();
    fs::create_directories(dir);
    return dir;
}

std::string test::readFile(const fs::path& path){
    std::ifstream file(path, std::ios::binary);
    std::ostringstream stream;
    stream << file.rdbuf();
    return stream.str();
}

void test::writeFile(const fs::path& path, const std::string& content){
    fs::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << content;
}

TempProject::TempProject(bool copyFixture): directory(makeTempDir("jasper-import-test-")){
    if (copyFixture)
        fs::copy(fixtureDir() / "project", directory, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
    else
        writeFile(directory / "project.yaml", "name: Empty\nassetsDir: assets\n");
    fs::create_directories(directory / "assets");
}

TempProject::~TempProject(){
    std::error_code ec;
    fs::remove_all(directory, ec);
}

ImportTarget TempProject::target(const std::string& destDir) const{
    ImportTarget importTarget;
    std::string error;
    EXPECT_TRUE(ImportTarget::forProject(directory, destDir, importTarget, error)) << error;
    return importTarget;
}

void TempProject::write(const std::string& projectRelative, const std::string& content) const{
    writeFile(directory / editor::FileUtils::pathFromUtf8(projectRelative), content);
}

std::string TempProject::read(const std::string& projectRelative) const{
    return readFile(directory / editor::FileUtils::pathFromUtf8(projectRelative));
}

bool TempProject::exists(const std::string& projectRelative) const{
    std::error_code ec;
    return fs::is_regular_file(directory / editor::FileUtils::pathFromUtf8(projectRelative), ec);
}

compat::ImportManifest TempProject::manifest() const{
    return ImportManifestWriter(assets()).read();
}

ScopedEnv::ScopedEnv(const char* name, const char* value): name(name){
    const char* current = std::getenv(name);
    hadPrevious = current != nullptr;
    if (current)
        previous = current;
    if (value)
        setenv(name, value, 1);
    else
        unsetenv(name);
}

ScopedEnv::~ScopedEnv(){
    if (hadPrevious)
        setenv(name.c_str(), previous.c_str(), 1);
    else
        unsetenv(name.c_str());
}

ScopedRewrite::ScopedRewrite(std::map<std::string, std::string> rewrites): previous(HttpClient::getDefaultHostRewrite()){
    HttpClient::setDefaultHostRewrite(std::move(rewrites));
}

ScopedRewrite::~ScopedRewrite(){
    HttpClient::setDefaultHostRewrite(previous);
}

// ---------------------------------------------------------------------------------------------
// TTF, diagnostics
// ---------------------------------------------------------------------------------------------

bool TtfInfo::hasTable(const std::string& tag) const{
    return std::find(tables.begin(), tables.end(), tag) != tables.end();
}

TtfInfo test::ttfInfo(const std::string& data){
    TtfInfo info;
    auto u16 = [&data](size_t at){ return at + 2 <= data.size() ? (int)(((unsigned char)data[at] << 8) | (unsigned char)data[at + 1]) : 0; };
    auto u32 = [&data](size_t at){
        if (at + 4 > data.size())
            return 0u;
        return ((unsigned)(unsigned char)data[at] << 24) | ((unsigned)(unsigned char)data[at + 1] << 16) |
               ((unsigned)(unsigned char)data[at + 2] << 8) | (unsigned)(unsigned char)data[at + 3];
    };
    if (data.size() < 12 || (data.compare(0, 4, std::string("\0\1\0\0", 4)) != 0 && data.compare(0, 4, "true") != 0 &&
                             data.compare(0, 4, "OTTO") != 0))
        return info;
    const int count = u16(4);
    for (int i = 0; i < count; i++){
        const size_t entry = 12 + 16 * (size_t)i;
        if (entry + 16 > data.size())
            return info;
        const std::string tag = data.substr(entry, 4);
        info.tables.push_back(tag);
        if (tag == "OS/2"){
            const size_t offset = u32(entry + 8);
            info.weight = u16(offset + 4);
            info.italic = (u16(offset + 62) & 1) != 0;
        }
    }
    info.valid = true;
    return info;
}

std::vector<compat::Diagnostic> test::withCode(const std::vector<compat::Diagnostic>& diagnostics, const char* id){
    std::vector<compat::Diagnostic> found;
    for (const compat::Diagnostic& diagnostic : diagnostics){
        if (diagnostic.code.id() == id)
            found.push_back(diagnostic);
    }
    return found;
}

std::string test::dump(const std::vector<compat::Diagnostic>& diagnostics){
    std::string text;
    for (const compat::Diagnostic& diagnostic : diagnostics)
        text += compat::toText(diagnostic) + "\n";
    return text;
}

// ---------------------------------------------------------------------------------------------
// the minimal HTML scan (until p2-html)
// ---------------------------------------------------------------------------------------------

bool test::htmlStageAvailable(){
#if __has_include("htmlui/compat/DocumentLoader.h")
    return true;
#else
    return false;
#endif
}

std::vector<compat::RemoteReference> test::minimalHtmlReferences(const std::string& htmlPath, const compat::FileAccess& files){
    const std::string lowered = lower(htmlPath);
    if (lowered.size() >= 4 && lowered.compare(lowered.size() - 4, 4, ".css") == 0)
        return compat::collectRemoteReferences(htmlPath, files);

    std::vector<compat::RemoteReference> references;
    std::string html;
    if (!files.read || !files.read(htmlPath, html))
        return references;

    auto add = [&references](const std::string& url, const std::string& kind){
        if (compat::UrlResolver::classify(url) == compat::UrlKind::Remote)
            references.push_back(compat::RemoteReference{url, kind, compat::SourceRef{}});
    };
    auto addCss = [&add](const std::string& css){
        for (const CssUrl& found : findCssUrls(css)){
            const bool isImport = found.context == CssUrl::Context::Import;
            const bool google = found.url.find("fonts.googleapis.com") != std::string::npos;
            add(found.url, isImport ? (google ? "google-fonts-css" : "stylesheet") :
                           found.context == CssUrl::Context::FontFace ? "font" : "image");
        }
    };

    static const std::regex tagPattern(R"re(<(link|img|source|lottie|svg|style)\b([^>]*)>)re", std::regex::icase);
    static const std::regex attributePattern(R"re(([a-zA-Z_:][-a-zA-Z0-9_:.]*)\s*=\s*(?:"([^"]*)"|'([^']*)'|([^\s"'>]+)))re");
    for (auto it = std::sregex_iterator(html.begin(), html.end(), tagPattern); it != std::sregex_iterator(); ++it){
        const std::string tag = lower((*it)[1].str());
        const std::string attributesText = (*it)[2].str();
        std::map<std::string, std::string> attributes;
        for (auto at = std::sregex_iterator(attributesText.begin(), attributesText.end(), attributePattern); at != std::sregex_iterator(); ++at){
            const std::string value = (*at)[2].matched ? (*at)[2].str() : (*at)[3].matched ? (*at)[3].str() : (*at)[4].str();
            attributes[lower((*at)[1].str())] = value;
        }
        if (attributes.count("style"))
            addCss("x{" + attributes["style"] + "}");
        if (tag == "link"){
            if (lower(attributes["rel"]).find("stylesheet") == std::string::npos)
                continue;
            const std::string href = attributes["href"];
            if (compat::UrlResolver::classify(href) == compat::UrlKind::Remote){
                add(href, href.find("fonts.googleapis.com") != std::string::npos ? "google-fonts-css" : "stylesheet");
            }else if (!href.empty()){
                const std::string local = compat::UrlResolver::joinPath(htmlPath, href);
                for (const compat::RemoteReference& reference : compat::collectRemoteReferences(local, files))
                    references.push_back(reference);
            }
        }else if (tag == "img" || tag == "source"){
            add(attributes["src"], "image");
            std::stringstream srcset(attributes["srcset"]);
            std::string candidate;
            while (std::getline(srcset, candidate, ',')){
                std::stringstream words(candidate);
                std::string url;
                words >> url;
                add(url, "image");
            }
        }else if (tag == "lottie"){
            add(attributes["src"], "lottie");
        }else if (tag == "svg"){
            add(attributes["src"], "svg");
        }else if (tag == "style"){
            const size_t start = (size_t)((*it).position(0) + (*it).length(0));
            const size_t end = html.find("</style", start);
            if (end != std::string::npos)
                addCss(html.substr(start, end - start));
        }
    }
    return references;
}

// ---------------------------------------------------------------------------------------------
// processes
// ---------------------------------------------------------------------------------------------

int test::runProgram(const std::vector<std::string>& args, std::string& output, const std::vector<std::string>& env){
    output.clear();
    int pipeFds[2];
    if (args.empty() || pipe(pipeFds) != 0)
        return -1;
    const pid_t child = fork();
    if (child == 0){
        dup2(pipeFds[1], STDOUT_FILENO);
        dup2(pipeFds[1], STDERR_FILENO);
        close(pipeFds[0]);
        close(pipeFds[1]);
        for (const std::string& assignment : env){
            const size_t equals = assignment.find('=');
            if (equals == std::string::npos)
                unsetenv(assignment.c_str());
            else
                setenv(assignment.substr(0, equals).c_str(), assignment.substr(equals + 1).c_str(), 1);
        }
        std::vector<char*> argv;
        for (const std::string& arg : args)
            argv.push_back(const_cast<char*>(arg.c_str()));
        argv.push_back(nullptr);
        execv(argv[0], argv.data());
        _exit(127);
    }
    close(pipeFds[1]);
    if (child < 0){
        close(pipeFds[0]);
        return -1;
    }
    char buffer[4096];
    ssize_t count;
    while ((count = read(pipeFds[0], buffer, sizeof(buffer))) > 0)
        output.append(buffer, (size_t)count);
    close(pipeFds[0]);
    int status = 0;
    while (waitpid(child, &status, 0) == -1 && errno == EINTR){
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}
