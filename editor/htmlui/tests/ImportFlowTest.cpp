// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The manifest writer, discovery and whole-document imports, the runtime resolving what was
// imported (no network), offline behavior, Remove unused, the import-ui CLI, the auto-import
// queue and the Lucide icons of the facade (phase-2 §3.13, §5.4).

#include "ImporterTestCommon.h"
#include "htmlui/compat/runtime/LucideIcons.h"

#include "htmlui/HtmlUiImportCli.h"
#include "htmlui/HtmlUiImportMaintenance.h"
#include "htmlui/HtmlUiImportQueue.h"
#include "htmlui/HtmlUiImporters.h"
#include "htmlui/ImportManifestWriter.h"

#include "htmlui/compat/CompatApi.h"
#include "htmlui/compat/SourceFiles.h"
#include "htmlui/compat/UrlResolver.h"

#include <atomic>
#include <condition_variable>
#include <sstream>
#include <thread>

using namespace jasperengine::editor::htmlui;
using namespace jasperengine::editor::htmlui::test;
using jasperengine::htmlui::compat::ImportEntry;
using jasperengine::htmlui::compat::ImportManifest;
using jasperengine::htmlui::compat::UrlResolver;

namespace {

    const std::vector<std::string> kMenuUrls = {
        "https://fonts.googleapis.com/css2?family=Orbitron:wght@700&display=swap",
        "https://fonts.googleapis.com/css2?family=Inter:wght@400;700&display=swap",
        "https://cdnjs.cloudflare.com/fa/css/all.min.css",
        "https://images.example.com/photo?w=800",
        "https://images.example.com/placeholder",
        "https://images.example.com/octet",
        "https://lottie.host/anim.json",
    };

    // what the runtime's resolver says about each URL of the menu, through the project's manifest
    std::vector<compat::Diagnostic> resolveAtRuntime(const TempProject& project, const std::vector<std::string>& urls, bool filesExist = true){
        const compat::FileAccess files = project.target().fileAccess();
        const ImportManifest manifest = ImportManifest::load(files);
        const UrlResolver resolver(files, &manifest);
        compat::SourceFiles sources;
        compat::DiagnosticSink sink(sources);
        for (const std::string& url : urls){
            const compat::ResolvedUrl resolved = resolver.resolve(url, "ui/menu.html", "image", compat::SourceRef{}, &sink);
            if (resolved.entry && filesExist){
                EXPECT_FALSE(resolved.dropped()) << url;
                EXPECT_TRUE(project.exists("assets/" + resolved.assetPath)) << url << " -> " << resolved.assetPath;
            }
        }
        return sink.take();
    }

}

// ---------------------------------------------------------------------------------------------
// manifest writer
// ---------------------------------------------------------------------------------------------

TEST(ManifestWriter, SortedAtomicAndReplacing){
    TempProject project(false);
    ImportManifestWriter writer(project.assets());
    ImportEntry b;
    b.url = "https://b.example.com/x.png";
    b.kind = "image";
    b.path = "htmlui-imports/images/b.png";
    ImportEntry a = b;
    a.url = "HTTP://A.example.com:443/y.png#frag";
    a.path = "htmlui-imports/images/a.png";
    ASSERT_TRUE(writer.add({b}));
    ASSERT_TRUE(writer.add({a}));

    const std::string json = project.read("assets/htmlui-imports/manifest.json");
    EXPECT_LT(json.find("https://a.example.com/y.png"), json.find("https://b.example.com/x.png"));
    EXPECT_NE(json.find("\"version\": 1"), std::string::npos);
    // the runtime reads what the writer wrote
    ImportManifest parsed;
    std::string error;
    ASSERT_TRUE(ImportManifest::parse(json, parsed, &error)) << error;
    ASSERT_EQ(parsed.entries().size(), 2u);
    EXPECT_EQ(parsed.entries()[0].url, "https://a.example.com/y.png");   // the default port dropped
    EXPECT_NE(parsed.find("http://b.example.com/x.png"), nullptr);
    // no temporary file left behind
    size_t files = 0;
    for (const auto& item : fs::directory_iterator(project.assets() / "htmlui-imports"))
        files += item.is_regular_file() ? 1 : 0;
    EXPECT_EQ(files, 1u);

    // a re-import replaces the entry of the same URL
    ImportEntry again = b;
    again.path = "htmlui-imports/images/b2.png";
    ASSERT_TRUE(writer.add({again}));
    EXPECT_EQ(writer.read().entries().size(), 2u);
    EXPECT_EQ(writer.read().find(b.url)->path, "htmlui-imports/images/b2.png");

    ASSERT_TRUE(writer.remove({"https://B.example.com/x.png"}));
    EXPECT_EQ(writer.read().entries().size(), 1u);
}

TEST(ManifestWriter, ConcurrentWritersKeepEveryEntry){
    TempProject project(false);
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; t++){
        threads.emplace_back([&project, t]{
            ImportManifestWriter writer(project.assets());
            for (int i = 0; i < 10; i++){
                ImportEntry entry;
                entry.url = "https://h.example.com/" + std::to_string(t) + "-" + std::to_string(i) + ".png";
                entry.kind = "image";
                entry.path = "x.png";
                writer.add({entry});
            }
        });
    }
    for (std::thread& thread : threads)
        thread.join();
    EXPECT_EQ(project.manifest().entries().size(), 40u);
}

// ---------------------------------------------------------------------------------------------
// discovery and whole documents
// ---------------------------------------------------------------------------------------------

TEST(Discovery, FindsTheRemoteReferencesOfADocument){
    TempProject project;
    const std::vector<DiscoveredReference> references = discoverRemoteReferences(project.target(), "ui/menu.html");
    std::vector<std::string> urls;
    for (const DiscoveredReference& reference : references)
        urls.push_back(reference.url);
    for (const std::string& url : kMenuUrls)
        EXPECT_NE(std::find(urls.begin(), urls.end(), url), urls.end()) << url;
    // preconnect hints and plain links are no resources
    for (const std::string& url : urls){
        EXPECT_NE(url, "https://fonts.googleapis.com");
        EXPECT_NE(url, "https://example.com/help");
    }
    // located where the document has them
    for (const DiscoveredReference& reference : references){
        if (reference.url == "https://images.example.com/octet"){
            EXPECT_EQ(reference.location.file, "ui/menu.html");
            EXPECT_EQ(reference.location.line, 13u);
            EXPECT_EQ(reference.location.column, 11u);
            EXPECT_EQ(reference.kind, "image");
        }
    }

    // the CSS side alone (the compat collection)
    const std::vector<DiscoveredReference> css = discoverRemoteReferences(project.target(), "ui/menu.css");
    ASSERT_EQ(css.size(), 4u);
    EXPECT_EQ(css[0].kind, "google-fonts-css");
    EXPECT_EQ(css[0].location.line, 2u);
}

TEST(Discovery, DocumentImportThenTheRuntimeResolvesEverything){
    TempProject project;
    sharedServer().reset();

    // before: every URL is missing at runtime (HTMLUI-IMP-0001, fallback)
    const auto before = resolveAtRuntime(project, kMenuUrls);
    EXPECT_EQ(withCode(before, "HTMLUI-IMP-0001").size(), kMenuUrls.size()) << dump(before);

    const ImportResult result = importDocumentReferences(project.root(), "assets/ui/menu.html", "");
    ASSERT_TRUE(result.ok) << result.error << "\n" << dump(result.diagnostics);
    EXPECT_EQ(result.replaced.size(), kMenuUrls.size());
    for (const auto& pair : result.replaced){
        EXPECT_EQ(pair.second.compare(0, 22, "assets/htmlui-imports/"), 0) << pair.second;
        EXPECT_TRUE(project.exists(pair.second)) << pair.second;
    }
    EXPECT_FALSE(result.fontFaceCss.empty());
    // the importer never edits the document
    EXPECT_EQ(project.read("assets/ui/menu.html"), readFile(fixtureDir() / "project/assets/ui/menu.html"));

    // after: the runtime finds every URL locally, no network involved
    const auto after = resolveAtRuntime(project, kMenuUrls);
    EXPECT_TRUE(withCode(after, "HTMLUI-IMP-0001").empty()) << dump(after);
    EXPECT_TRUE(withCode(after, "HTMLUI-IMP-0009").empty()) << dump(after);

    // the stylesheet compiles against the populated project without import errors
    compat::CheckOptions options;
    options.verifyWithRmlUi = false;
    options.files = project.target().fileAccess();
    const compat::CheckReport report = compat::checkStylesheet("ui/menu.css", options);
    EXPECT_TRUE(withCode(report.diagnostics, "HTMLUI-IMP-0001").empty()) << dump(report.diagnostics);
    if (htmlStageAvailable()){
        const compat::CheckReport document = compat::checkDocument("ui/menu.html", options);
        EXPECT_TRUE(withCode(document.diagnostics, "HTMLUI-IMP-0001").empty()) << dump(document.diagnostics);
    }

    // a second run downloads nothing: everything is in the manifest
    sharedServer().reset();
    const ImportResult second = importDocumentReferences(project.root(), "assets/ui/menu.html", "");
    EXPECT_TRUE(second.ok);
    EXPECT_TRUE(second.written.empty());
    EXPECT_EQ(second.replaced.size(), kMenuUrls.size());
    EXPECT_TRUE(sharedServer().requests().empty());
}

TEST(Discovery, MissingFileOfAnEntryIsReimported){
    TempProject project;
    ASSERT_TRUE(importUrl(project.root(), "https://images.example.com/placeholder", "").ok);
    const std::string path = project.manifest().find("https://images.example.com/placeholder")->path;
    fs::remove(project.assets() / path);

    const auto runtime = resolveAtRuntime(project, {"https://images.example.com/placeholder"}, false);
    EXPECT_EQ(withCode(runtime, "HTMLUI-IMP-0009").size(), 1u) << dump(runtime);

    const ImportResult again = importDocumentReferences(project.root(), "assets/ui/menu.css", "");
    EXPECT_TRUE(again.ok) << again.error;
    EXPECT_TRUE(project.exists("assets/" + path));
}

TEST(Offline, ServerStoppedFailsInTheEditorAndFallsBackAtRuntime){
    TempProject project;
    FixtureServer server;
    ASSERT_TRUE(server.running());
    ScopedRewrite rewrite(server.rewrites());
    server.stop();

    const ImportResult result = importDocumentReferences(project.root(), "assets/ui/menu.css", "");
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.failed.size(), 4u);
    const auto failed = withCode(result.diagnostics, "HTMLUI-IMP-0002");
    EXPECT_EQ(failed.size(), 4u) << dump(result.diagnostics);
    for (const compat::Diagnostic& diagnostic : failed){
        EXPECT_EQ(diagnostic.file, "ui/menu.css");
        EXPECT_GT(diagnostic.line, 0u);
    }
    // nothing written
    EXPECT_TRUE(result.written.empty());
    EXPECT_FALSE(fs::exists(project.assets() / "htmlui-imports"));

    // the runtime: not imported, the fallback (dropped URL)
    const compat::FileAccess files = project.target().fileAccess();
    const ImportManifest manifest = ImportManifest::load(files);
    const UrlResolver resolver(files, &manifest);
    compat::SourceFiles sources;
    compat::DiagnosticSink sink(sources);
    const compat::ResolvedUrl resolved = resolver.resolve(kMenuUrls[3], "ui/menu.css", "image", compat::SourceRef{}, &sink);
    EXPECT_TRUE(resolved.dropped());
    const auto missing = withCode(sink.take(), "HTMLUI-IMP-0001");
    ASSERT_EQ(missing.size(), 1u);
    EXPECT_EQ(missing[0].severity, compat::Severity::Error);
}

// ---------------------------------------------------------------------------------------------
// Remove unused
// ---------------------------------------------------------------------------------------------

TEST(RemoveUnused, KeepsWhatDocumentsUseAndWhatTheirImportsNeed){
    TempProject project;
    // the chain stylesheet (with its nested Google Fonts import) is referenced; the photo is not
    project.write("assets/ui/chain.css", "@import url(\"https://cdnjs.cloudflare.com/chain/a.css\");\n");
    ASSERT_TRUE(importDocumentReferences(project.root(), "assets/ui/chain.css", "").ok);
    ASSERT_TRUE(importUrl(project.root(), "https://images.example.com/webp", "").ok);
    ASSERT_TRUE(importGoogleFonts(project.root(), {"Karla:400"}, "").ok);
    const compat::ImportManifest before = project.manifest();
    const std::string webp = before.find("https://images.example.com/webp")->path;
    const std::string karla = before.find("https://fonts.googleapis.com/css2?family=Karla:wght@400")->path;
    const std::string orbitron = before.find("https://fonts.googleapis.com/css2?family=Orbitron:wght@700")->path;

    RemoveUnusedResult removed = removeUnusedImports(project.target());
    ASSERT_TRUE(removed.error.empty()) << removed.error;
    std::sort(removed.removedUrls.begin(), removed.removedUrls.end());
    EXPECT_EQ(removed.removedUrls.size(), 2u);
    EXPECT_FALSE(project.exists("assets/" + webp));
    EXPECT_FALSE(project.exists("assets/" + karla));
    EXPECT_FALSE(project.exists("assets/htmlui-imports/fonts/karla/Karla-400.ttf"));
    // the empty directories went with them
    EXPECT_FALSE(fs::exists(project.assets() / "htmlui-imports/fonts/karla"));

    const compat::ImportManifest after = project.manifest();
    EXPECT_NE(after.find("https://cdnjs.cloudflare.com/chain/a.css"), nullptr);
    // used through the chain stylesheet's files only
    EXPECT_NE(after.find("https://fonts.googleapis.com/css2?family=Orbitron:wght@700"), nullptr);
    EXPECT_TRUE(project.exists("assets/" + orbitron));
    EXPECT_EQ(after.find("https://images.example.com/webp"), nullptr);

    // nothing more to remove
    EXPECT_TRUE(removeUnusedImports(project.target()).removedUrls.empty());
}

// ---------------------------------------------------------------------------------------------
// the import-ui CLI
// ---------------------------------------------------------------------------------------------

namespace {

    int runCli(std::vector<std::string> args, std::string& out, std::string& err){
        args.insert(args.begin(), "import-ui");
        std::vector<char*> argv;
        for (std::string& arg : args)
            argv.push_back(arg.data());
        std::ostringstream outStream, errStream;
        const int code = runImportUi((int)argv.size(), argv.data(), "jasperengine-editor", outStream, errStream);
        out = outStream.str();
        err = errStream.str();
        return code;
    }

}

TEST(ImportUiCli, ExitCodesAndOutput){
    TempProject project;
    std::string out, err;
    EXPECT_EQ(runCli({"--help"}, out, err), 0);
    EXPECT_NE(out.find("import-ui --project"), std::string::npos);
    EXPECT_EQ(runCli({"assets/ui/menu.css"}, out, err), 2);   // no --project
    EXPECT_NE(err.find("--project is required"), std::string::npos);
    EXPECT_EQ(runCli({"--project", project.root().string(), "assets/ui/nothing.css"}, out, err), 2);
    EXPECT_EQ(runCli({"--project", project.root().string(), "--bogus", "assets/ui/menu.css"}, out, err), 2);

    // success: the diagnostics compiler-style with project-relative paths
    EXPECT_EQ(runCli({"--project", project.root().string(), "assets/ui/menu.css"}, out, err), 0) << out << err;
    EXPECT_NE(out.find("info: imported 'https://images.example.com/placeholder'"), std::string::npos) << out;
    EXPECT_NE(out.find("[HTMLUI-IMP-0004]"), std::string::npos);
    EXPECT_NE(err.find("[HTML UI] importing https://"), std::string::npos) << err;
    EXPECT_NE(err.find("import-ui: 4 imported, 0 up to date, 0 failed"), std::string::npos) << err;
    // again: up to date
    EXPECT_EQ(runCli({"--project", project.root().string(), "-q", "assets/ui/menu.css"}, out, err), 0);
    EXPECT_TRUE(out.empty()) << out;
    EXPECT_TRUE(err.empty()) << err;

    // a failed download: exit 1, the error located in the document
    project.write("assets/ui/broken.css", ".x {\n  background: url(https://status.example.com/404);\n}\n");
    EXPECT_EQ(runCli({"--project", project.root().string(), "assets/ui/broken.css"}, out, err), 1);
    EXPECT_NE(out.find("assets/ui/broken.css:2:19: error: download of 'https://status.example.com/404' failed: HTTP 404"),
              std::string::npos) << out;
}

TEST(ImportUiCli, EditorBinary){
    // the real subcommand, offline through the mirror (a child process gets no host rewrites)
    TempProject project;
    project.write("assets/ui/cli.css", ".logo { background-image: url(\"https://images.example.com/cli.png\"); }\n"
                                       ".gone { background-image: url(\"https://images.example.com/gone.png\"); }\n");
    const fs::path mirror = makeTempDir("jasper-cli-mirror-");
    writeFile(mirror / "images.example.com" / HttpClient::urlQuote("/cli.png"), readFile(fixtureDir() / "img/pixel.png"));

    const std::string editor = JASPER_EDITOR_TEST_EDITOR_BINARY;
    std::string output;
    EXPECT_EQ(runProgram({editor, "import-ui", "--help"}, output), 0) << output;
    EXPECT_NE(output.find("import-ui --project"), std::string::npos) << output;
    EXPECT_EQ(runProgram({editor, "import-ui"}, output), 2) << output;

    const std::vector<std::string> env = {"JASPERENGINE_IMPORT_MIRROR=" + mirror.string()};
    EXPECT_EQ(runProgram({editor, "import-ui", "--project", project.root().string(), "assets/ui/cli.css"}, output, env), 1) << output;
    EXPECT_NE(output.find("assets/ui/cli.css:2:"), std::string::npos) << output;
    EXPECT_NE(output.find("not found"), std::string::npos) << output;
    EXPECT_NE(project.manifest().find("https://images.example.com/cli.png"), nullptr);

    project.write("assets/ui/cli.css", ".logo { background-image: url(\"https://images.example.com/cli.png\"); }\n");
    EXPECT_EQ(runProgram({editor, "import-ui", "--project", project.root().string(), "assets/ui/cli.css"}, output, env), 0) << output;
    fs::remove_all(mirror);
}

// ---------------------------------------------------------------------------------------------
// the auto-import queue
// ---------------------------------------------------------------------------------------------

TEST(ImportQueue, OneImportPerUrlAndNoAutomaticRetry){
    std::mutex mutex;
    std::condition_variable release;
    bool open = false;
    std::atomic<int> runs{0};
    ImportQueue queue([&](const ImportJob& job){
        std::unique_lock<std::mutex> lock(mutex);
        release.wait(lock, [&]{ return open; });
        runs++;
        ImportedResource result;
        result.url = job.url;
        result.ok = job.url.find("fail") == std::string::npos;
        result.error = result.ok ? "" : "HTTP 404";
        return result;
    });

    auto job = [](const std::string& url, const std::string& document){
        ImportJob j;
        j.url = url;
        j.documents = {document};
        return j;
    };
    EXPECT_TRUE(queue.enqueue(job("https://a.example.com/x.png", "ui/a.html"), false));
    // the same URL (normalized) while queued or running: one import, both documents reload
    EXPECT_FALSE(queue.enqueue(job("HTTP://A.example.com/x.png#f", "ui/b.html"), false));
    EXPECT_TRUE(queue.enqueue(job("https://a.example.com/fail.png", "ui/a.html"), false));
    EXPECT_TRUE(queue.busy());
    EXPECT_EQ(queue.pending(), 2u);
    {
        std::lock_guard<std::mutex> lock(mutex);
        open = true;
    }
    release.notify_all();
    queue.waitIdle();
    EXPECT_EQ(runs.load(), 2);

    ImportQueue::Batch batch;
    ASSERT_TRUE(queue.takeFinishedBatch(batch));
    EXPECT_EQ(batch.results.size(), 2u);
    std::sort(batch.documents.begin(), batch.documents.end());
    EXPECT_EQ(batch.documents, (std::vector<std::string>{"ui/a.html", "ui/b.html"}));
    EXPECT_FALSE(queue.takeFinishedBatch(batch));

    EXPECT_EQ(queue.status("https://a.example.com/x.png")->state, ImportStatus::State::Imported);
    EXPECT_EQ(queue.status("https://a.example.com/fail.png")->state, ImportStatus::State::Failed);
    EXPECT_EQ(queue.status("https://a.example.com/fail.png")->error, "HTTP 404");
    EXPECT_FALSE(queue.status("https://other.example.com/").has_value());

    // automatic requests skip what this session imported or failed; the panel's Import retries
    EXPECT_FALSE(queue.enqueue(job("https://a.example.com/x.png", "ui/a.html"), false));
    EXPECT_FALSE(queue.enqueue(job("https://a.example.com/fail.png", "ui/a.html"), false));
    EXPECT_TRUE(queue.enqueue(job("https://a.example.com/fail.png", "ui/a.html"), true));
    queue.waitIdle();
    EXPECT_EQ(runs.load(), 3);

    // a new session (project change) forgets them
    queue.clearSession();
    EXPECT_TRUE(queue.enqueue(job("https://a.example.com/x.png", "ui/a.html"), false));
    queue.waitIdle();
    queue.stop();
    EXPECT_FALSE(queue.enqueue(job("https://a.example.com/y.png", "ui/a.html"), true));
}

TEST(ImportQueue, RealImportsOnTheWorker){
    TempProject project;
    ImportQueue queue;
    for (const char* url : {"https://images.example.com/placeholder", "https://lottie.host/anim.json"}){
        ImportJob job;
        job.target = project.target();
        job.url = url;
        job.documents = {"ui/menu.html"};
        EXPECT_TRUE(queue.enqueue(job, false));
    }
    queue.waitIdle();
    ImportQueue::Batch batch;
    ASSERT_TRUE(queue.takeFinishedBatch(batch));
    ASSERT_EQ(batch.results.size(), 2u);
    for (const ImportedResource& result : batch.results)
        EXPECT_TRUE(result.ok) << result.url << ": " << result.error;
    EXPECT_EQ(project.manifest().entries().size(), 2u);
}

// ---------------------------------------------------------------------------------------------
// Lucide icons (built into the engine)
// ---------------------------------------------------------------------------------------------

TEST(LucideIcons, WritesStandaloneSvgs){
    TempProject project(false);
    if (!lucideIconsAvailable()){
        const ImportResult result = importLucideIcons(project.root(), {"play"}, "assets/ui/icons");
        EXPECT_FALSE(result.ok);
        EXPECT_NE(result.error.find("not built into this engine"), std::string::npos);
        GTEST_SKIP() << "the Lucide pack (p2-runtime) is not in this build";
    }
    sharedServer().reset();
    const ImportResult result = importLucideIcons(project.root(), {"lucide:play", "ArrowRight", "not-an-icon-name"}, "assets/ui/icons");
    EXPECT_FALSE(result.ok);
    ASSERT_EQ(result.failed.size(), 1u);
    EXPECT_EQ(result.failed[0].first, "not-an-icon-name");
    EXPECT_TRUE(project.exists("assets/ui/icons/play.svg"));
    EXPECT_TRUE(project.exists("assets/ui/icons/arrow-right.svg"));
    const std::string svg = project.read("assets/ui/icons/play.svg");
    EXPECT_NE(svg.find("stroke=\"currentColor\""), std::string::npos);
    EXPECT_NE(svg.find("viewBox=\"0 0 24 24\""), std::string::npos);
    // no network, no manifest
    EXPECT_TRUE(sharedServer().requests().empty());
    EXPECT_FALSE(project.exists("assets/htmlui-imports/manifest.json"));
}
