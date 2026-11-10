// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Remote stylesheets (fonts preferred as TTF, URLs rewritten, WOFF2-only, @import chains),
// images by content type and magic bytes, Lottie JSON with assets, dotLottie v1/v2 and
// LottieFiles pages (phase-2 §3.13.3).

#include "ImporterTestCommon.h"

#include "htmlui/ContentSniffer.h"
#include "htmlui/HtmlUiImporters.h"
#include "htmlui/LottieImporter.h"
#include "htmlui/StylesheetImporter.h"

#include "htmlui/compat/UrlResolver.h"
#include "util/FontFace.h"

#include "json.hpp"

using namespace jasperengine::editor::htmlui;
using namespace jasperengine::editor::htmlui::test;
using jasperengine::htmlui::compat::ImportEntry;
using jasperengine::htmlui::compat::UrlResolver;

namespace {

    ImportedResource importOne(const TempProject& project, const std::string& url, std::vector<compat::Diagnostic>* diagnostics = nullptr){
        RemoteImporter importer(project.target());
        ImportedResource result = importer.import(url);
        if (diagnostics)
            *diagnostics = importer.takeDiagnostics();
        return result;
    }

    // every url() of a stylesheet resolves to a file of the project
    void expectLocalUrls(const TempProject& project, const std::string& cssAssetPath){
        const std::string css = project.read("assets/" + cssAssetPath);
        for (const CssUrl& found : findCssUrls(css)){
            if (UrlResolver::classify(found.url) == compat::UrlKind::Remote)
                continue;
            const std::string local = UrlResolver::joinPath(cssAssetPath, found.url);
            EXPECT_TRUE(project.exists("assets/" + local)) << found.url << " in " << cssAssetPath << " -> " << local;
        }
    }

}

// ---------------------------------------------------------------------------------------------
// CSS helpers
// ---------------------------------------------------------------------------------------------

TEST(CssUrls, FindsEveryUrlWithItsRange){
    const std::string css = "@import \"a.css\";\n@import url(b.css) screen;\n"
                            ".x{background:url( 'c.png' ) , image-set(\"d.png\" 1x, url(e.png) 2x)}\n"
                            "@media (min-width: 1px){ .y{ .z & { mask: url(f.svg#m) } } }\n"
                            "@font-face{font-family:F;src:local(F), url(g.woff2) format('woff2'), url(\"g.ttf\") format(\"truetype\")}\n";
    const std::vector<CssUrl> urls = findCssUrls(css);
    std::vector<std::string> names;
    for (const CssUrl& url : urls){
        names.push_back(url.url);
        // the range covers exactly the written url
        const std::string written = css.substr(url.offset, url.length);
        EXPECT_NE(written.find(url.url.substr(0, 1)), std::string::npos) << written;
    }
    EXPECT_EQ(names, (std::vector<std::string>{"a.css", "b.css", "c.png", "d.png", "e.png", "f.svg#m", "g.woff2", "g.ttf"}));
    EXPECT_EQ(urls[0].context, CssUrl::Context::Import);
    EXPECT_EQ(urls[2].context, CssUrl::Context::Other);
    EXPECT_EQ(urls[6].context, CssUrl::Context::FontFace);
    EXPECT_EQ(urls[6].fontFace, 0);

    const std::vector<CssFontFace> faces = findFontFaces(css);
    ASSERT_EQ(faces.size(), 1u);
    ASSERT_EQ(faces[0].sources.size(), 3u);
    EXPECT_TRUE(faces[0].sources[0].url.empty());
    EXPECT_TRUE(faces[0].sources[1].woff());
    EXPECT_TRUE(faces[0].sources[1].usable());
    EXPECT_TRUE(faces[0].sources[2].usable());
    EXPECT_EQ(css.substr(faces[0].srcOffset, faces[0].srcLength),
              "local(F), url(g.woff2) format('woff2'), url(\"g.ttf\") format(\"truetype\")");

    EXPECT_EQ(applyCssEdits("abcdef", {{4, 1, "E"}, {0, 2, "AB!"}}), "AB!cdEf");
    EXPECT_EQ(cssUrl("a \"b\".png"), "url(\"a \\\"b\\\".png\")");
}

TEST(CssUrls, ResolvesRelativeUrls){
    EXPECT_EQ(resolveUrl("https://h.com/a/b/c.css", "../d/e.png"), "https://h.com/a/d/e.png");
    EXPECT_EQ(resolveUrl("https://h.com/a/b/c.css", "/x.png"), "https://h.com/x.png");
    EXPECT_EQ(resolveUrl("https://h.com/a/b/c.css?v=1", "f.ttf?v=2#iefix"), "https://h.com/a/b/f.ttf?v=2#iefix");
    EXPECT_EQ(resolveUrl("https://h.com/a/c.css", "//cdn.com/y.png"), "https://cdn.com/y.png");
    EXPECT_EQ(resolveUrl("https://h.com/a/c.css", "https://o.com/z.png"), "https://o.com/z.png");
    EXPECT_EQ(resolveUrl("https://h.com/a/c.css?q", "?r"), "https://h.com/a/c.css?r");
    EXPECT_EQ(resolveUrl("https://h.com", "../../x.png"), "https://h.com/x.png");
}

// ---------------------------------------------------------------------------------------------
// stylesheets
// ---------------------------------------------------------------------------------------------

TEST(Stylesheets, FontAwesomeKeepsTheTrueTypeSources){
    TempProject project;
    std::vector<compat::Diagnostic> diagnostics;
    const std::string url = "https://cdnjs.cloudflare.com/fa/css/all.min.css";
    const ImportedResource result = importOne(project, url, &diagnostics);
    ASSERT_TRUE(result.ok) << result.error << dump(diagnostics);

    const ImportEntry* entry = result.entry();
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->kind, "stylesheet");
    EXPECT_EQ(entry->path, "htmlui-imports/css/cdnjs.cloudflare.com/fa/css/all.min.css");
    EXPECT_EQ(entry->licenseName, "unknown");
    EXPECT_EQ(entry->source, "cdnjs.cloudflare.com");

    // the fonts and the image in the same tree; no WOFF2 downloaded
    EXPECT_TRUE(project.exists("assets/htmlui-imports/css/cdnjs.cloudflare.com/fa/webfonts/fa-solid-900.ttf"));
    EXPECT_TRUE(project.exists("assets/htmlui-imports/css/cdnjs.cloudflare.com/fa/webfonts/fa-brands-400.ttf"));
    EXPECT_TRUE(project.exists("assets/htmlui-imports/css/cdnjs.cloudflare.com/fa/img/pattern.png"));
    EXPECT_FALSE(project.exists("assets/htmlui-imports/css/cdnjs.cloudflare.com/fa/webfonts/fa-solid-900.woff2"));

    const std::string css = project.read("assets/" + entry->path);
    EXPECT_EQ(css.find("woff2"), std::string::npos) << css;
    EXPECT_NE(css.find(".fa-play:before{content:\"\\f04b\"}"), std::string::npos);   // the rest unchanged
    for (const CssFontFace& face : findFontFaces(css)){
        ASSERT_EQ(face.sources.size(), 1u);
        EXPECT_TRUE(face.sources[0].usable());
    }
    expectLocalUrls(project, entry->path);
    EXPECT_TRUE(withCode(diagnostics, "HTMLUI-IMP-0007").empty());
    EXPECT_EQ(withCode(diagnostics, "HTMLUI-IMP-0008").size(), 1u) << dump(diagnostics);
    EXPECT_EQ(entry->files.size(), 4u);
}

TEST(Stylesheets, WoffOnlyFontsAreImported){
    TempProject project;
    std::vector<compat::Diagnostic> diagnostics;
    const ImportedResource result = importOne(project, "https://cdnjs.cloudflare.com/woff2only.css", &diagnostics);
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_TRUE(withCode(diagnostics, "HTMLUI-IMP-0007").empty()) << dump(diagnostics);
    EXPECT_TRUE(project.exists("assets/htmlui-imports/css/cdnjs.cloudflare.com/fonts/woffonly.woff2"));
    EXPECT_TRUE(project.exists("assets/htmlui-imports/css/cdnjs.cloudflare.com/fonts/woffonly.woff"));
    ASSERT_NE(result.entry(), nullptr);
    expectLocalUrls(project, result.entry()->path);

    // a WOFF2 file imported on its own
    std::vector<compat::Diagnostic> direct;
    const ImportedResource font = importOne(project, "https://cdnjs.cloudflare.com/fonts/x.woff2", &direct);
    ASSERT_TRUE(font.ok) << font.error;
    ASSERT_NE(font.entry(), nullptr);
    EXPECT_EQ(font.entry()->kind, "font");
    EXPECT_TRUE(project.exists("assets/" + font.entry()->path));
    EXPECT_TRUE(withCode(direct, "HTMLUI-IMP-0007").empty()) << dump(direct);
    const std::string bytes = project.read("assets/" + font.entry()->path);
    std::string error;
    EXPECT_NE(jasperengine::FontFace::loadMemory((const unsigned char*)bytes.data(), bytes.size(), 0, true,
                                                font.entry()->path, &error), nullptr) << error;
}

TEST(Stylesheets, ImportChainsCyclesAndGoogleFonts){
    TempProject project;
    std::vector<compat::Diagnostic> diagnostics;
    const ImportedResource result = importOne(project, "https://cdnjs.cloudflare.com/chain/a.css", &diagnostics);
    ASSERT_TRUE(result.ok) << result.error << dump(diagnostics);

    const std::string a = "htmlui-imports/css/cdnjs.cloudflare.com/chain/a.css";
    const std::string b = "htmlui-imports/css/cdnjs.cloudflare.com/chain/b.css";
    ASSERT_TRUE(project.exists("assets/" + a));
    ASSERT_TRUE(project.exists("assets/" + b));
    // b imports a back: the cycle ends at the file being written
    EXPECT_NE(project.read("assets/" + b).find("url(\"a.css\")"), std::string::npos) << project.read("assets/" + b);
    // the Google Fonts @import became its own entry, the sheet points at its local CSS
    const compat::ImportManifest manifest = project.manifest();
    const ImportEntry* fonts = manifest.find("https://fonts.googleapis.com/css2?family=Orbitron:wght@700");
    ASSERT_NE(fonts, nullptr);
    EXPECT_EQ(fonts->kind, "google-fonts-css");
    const std::string css = project.read("assets/" + a);
    EXPECT_EQ(css.find("https://"), std::string::npos) << css;
    expectLocalUrls(project, a);
    expectLocalUrls(project, b);
    // the image of another host goes to its own tree, with the query hashed into the name
    bool photo = false;
    for (const std::string& file : result.entry()->files)
        photo = photo || file.find("css/images.example.com/photo-") != std::string::npos;
    EXPECT_TRUE(photo);
    // the sheet's entry lists the Google Fonts files it relies on (Remove unused keeps them)
    EXPECT_NE(std::find(result.entry()->files.begin(), result.entry()->files.end(), fonts->path), result.entry()->files.end());
}

// ---------------------------------------------------------------------------------------------
// images
// ---------------------------------------------------------------------------------------------

TEST(Images, TypeFromContentTypeAndMagicBytes){
    TempProject project;
    struct Case { const char* url; const char* extension; const char* kind; };
    for (const Case& c : {Case{"https://images.example.com/photo?w=800", "jpg", "image"},
                          Case{"https://images.example.com/placeholder", "svg", "svg"},
                          Case{"https://images.example.com/octet", "png", "image"}}){
        std::vector<compat::Diagnostic> diagnostics;
        const ImportedResource result = importOne(project, c.url, &diagnostics);
        ASSERT_TRUE(result.ok) << c.url << ": " << result.error;
        const ImportEntry* entry = result.entry();
        EXPECT_EQ(entry->kind, c.kind);
        EXPECT_EQ(entry->path.compare(0, 41, "htmlui-imports/images/images.example.com/"), 0) << entry->path;
        EXPECT_EQ(entry->path.substr(entry->path.size() - std::string(c.extension).size()), c.extension) << entry->path;
        EXPECT_TRUE(project.exists("assets/" + entry->path));
        EXPECT_TRUE(withCode(diagnostics, "HTMLUI-IMP-0003").empty()) << dump(diagnostics);
        EXPECT_EQ(withCode(diagnostics, "HTMLUI-IMP-0008").size(), 1u) << dump(diagnostics);
    }
}

TEST(Images, WebPIsSavedWithAWarning){
    TempProject project;
    std::vector<compat::Diagnostic> diagnostics;
    const ImportedResource result = importOne(project, "https://images.example.com/webp", &diagnostics);
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(result.entry()->path.substr(result.entry()->path.size() - 5), ".webp");
    EXPECT_TRUE(project.exists("assets/" + result.entry()->path));
    const auto unsupported = withCode(diagnostics, "HTMLUI-IMP-0003");
    ASSERT_EQ(unsupported.size(), 1u) << dump(diagnostics);
    EXPECT_EQ(unsupported[0].severity, compat::Severity::Warning);
}

TEST(Images, AnHtmlPageIsNotAnImage){
    TempProject project;
    std::vector<compat::Diagnostic> diagnostics;
    const ImportedResource result = importOne(project, "https://images.example.com/page", &diagnostics);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(withCode(diagnostics, "HTMLUI-IMP-0003").size(), 1u) << dump(diagnostics);
    EXPECT_FALSE(project.exists("assets/htmlui-imports/manifest.json"));
}

TEST(Images, Sniffing){
    using Type = SniffedContent::Type;
    EXPECT_EQ(sniffContent("\x89PNG\r\n\x1A\n....", "", "").type, Type::Png);
    EXPECT_EQ(sniffContent(std::string("\xFF\xD8\xFF\xE0", 4), "application/octet-stream", "").type, Type::Jpeg);
    EXPECT_EQ(sniffContent("GIF89a..", "", "").type, Type::Gif);
    EXPECT_EQ(sniffContent(std::string("RIFF\x10\0\0\0WEBPVP8L", 16), "", "").type, Type::WebP);
    EXPECT_EQ(sniffContent(std::string("\0\0\0\x1C" "ftypavif", 12), "", "").type, Type::Avif);
    EXPECT_EQ(sniffContent("\xEF\xBB\xBF<?xml version=\"1.0\"?>\n<!-- c --><!DOCTYPE svg><svg/>", "text/plain", "").type, Type::Svg);
    EXPECT_EQ(sniffContent("<svg xmlns='http://www.w3.org/2000/svg'/>", "", "").type, Type::Svg);
    EXPECT_EQ(sniffContent("{\"v\":\"5\",\"layers\":[]}", "application/json", "").type, Type::Json);
    EXPECT_EQ(sniffContent("body{color:red}", "text/css", "").type, Type::Css);
    EXPECT_EQ(sniffContent("<!doctype html><html>", "text/html", "").type, Type::Html);
    EXPECT_EQ(sniffContent("data", "image/x-tga", "").type, Type::Tga);
    EXPECT_EQ(sniffContent("BMxxxxxxxxxxxxxxxxxxxxxxxxxxxx", "image/bmp", "").type, Type::Bmp);
    EXPECT_EQ(sniffContent(std::string("\0\1\0\0", 4), "", "").type, Type::TrueType);
    EXPECT_EQ(sniffContent("wOF2", "", "").type, Type::Woff2);
    EXPECT_EQ(sniffContent(std::string("\x1A\x45\xDF\xA3\x9F\x42\x86\x81", 8), "", "").type, Type::WebM);
    EXPECT_EQ(sniffContent(std::string("\0\0\0\x18" "ftypmp42", 12), "", "").type, Type::Mp4);
    EXPECT_EQ(sniffContent(std::string("\0\0\0\x14" "ftypqt  ", 12), "", "").type, Type::QuickTime);
    EXPECT_EQ(sniffContent("OggS\0\2", "", "").type, Type::Ogg);
    EXPECT_TRUE(sniffContent(std::string("\0\0\0\x18" "ftypisom", 12), "", "").isVideo());
    EXPECT_FALSE(sniffContent(std::string("\0\0\0\x1C" "ftypavif", 12), "", "").isVideo());
    EXPECT_EQ(sniffContent(std::string("\0\0\0\x18" "ftypmp42", 12), "", "").extension(), "mp4");
    EXPECT_EQ(urlExtension("https://h.com/a/b.PNG?x=1#y"), "png");
    EXPECT_EQ(urlExtension("https://h.com/a.b/c"), "");
    EXPECT_EQ(urlBaseName("https://h.com/a/photo%20one.jpg?w=1"), "photo one");
}

// ---------------------------------------------------------------------------------------------
// Videos (phase 6)
// ---------------------------------------------------------------------------------------------

namespace {

    fs::path videoClip(const std::string& name){
        return sourceDir() / "engine/tests/htmlui/fixtures/video" / name;
    }

    // a stand-in ffmpeg (as VideoImporterTest's): lists libsvtav1, "converts" by copying the AV1 card
    fs::path fakeFfmpeg(const fs::path& dir){
        const fs::path script = dir / "ffmpeg";
        writeFile(script, "#!/bin/sh\ncase \"$*\" in *-encoders*) printf ' V....D libsvtav1 the encoder\\n'; exit 0;; esac\n"
                          "for last; do :; done\ncp '" + videoClip("testcard.webm").string() + "' \"$last\"\n");
        fs::permissions(script, fs::perms::owner_all);
        return script;
    }

}

TEST(Videos, APlayableVideoIsSavedAsItIs){
    TempProject project;
    std::vector<compat::Diagnostic> diagnostics;
    const ImportedResource result = importOne(project, "https://images.example.com/clip.webm", &diagnostics);
    ASSERT_TRUE(result.ok) << result.error << dump(diagnostics);
    const ImportEntry* entry = result.entry();
    EXPECT_EQ(entry->kind, "video");
    EXPECT_EQ(entry->path.compare(0, 41, "htmlui-imports/videos/images.example.com/"), 0) << entry->path;
    EXPECT_EQ(entry->path.substr(entry->path.size() - 10), "-clip.webm") << entry->path;
    EXPECT_EQ(project.read("assets/" + entry->path), readFile(videoClip("testcard-noaudio.webm")));
    EXPECT_EQ(withCode(diagnostics, "HTMLUI-IMP-0008").size(), 1u) << dump(diagnostics);
    EXPECT_TRUE(withCode(diagnostics, "HTMLUI-IMP-0010").empty()) << dump(diagnostics);
}

TEST(Videos, OthersAreConvertedByFfmpeg){
    TempProject project;
    const fs::path tools = makeTempDir("p6video-ffmpeg");
    ScopedEnv ffmpeg("JASPER_FFMPEG", fakeFfmpeg(tools).string().c_str());
    std::vector<compat::Diagnostic> diagnostics;
    const ImportedResource result = importOne(project, "https://images.example.com/trailer", &diagnostics);
    ASSERT_TRUE(result.ok) << result.error << dump(diagnostics);
    const ImportEntry* entry = result.entry();
    EXPECT_EQ(entry->kind, "video");
    EXPECT_EQ(entry->path.substr(entry->path.size() - 13), "-trailer.webm") << entry->path;
    EXPECT_EQ(project.read("assets/" + entry->path), readFile(videoClip("testcard.webm"))) << "the converted file";
    EXPECT_TRUE(withCode(diagnostics, "HTMLUI-IMP-0010").empty() && withCode(diagnostics, "HTMLUI-IMP-0011").empty()) << dump(diagnostics);
    std::error_code ec;
    fs::remove_all(tools, ec);
}

TEST(Videos, WithoutFfmpegTheOriginalIsSavedAndDiagnosed){
    TempProject project;
    ScopedEnv ffmpeg("JASPER_FFMPEG", "");
    std::vector<compat::Diagnostic> diagnostics;
    const ImportedResource result = importOne(project, "https://images.example.com/trailer", &diagnostics);
    ASSERT_TRUE(result.ok) << result.error << dump(diagnostics);
    const ImportEntry* entry = result.entry();
    // the VP9 clip is WebM: saved with that extension, unconverted
    EXPECT_EQ(project.read("assets/" + entry->path), readFile(videoClip("testcard-vp9.webm")));
    const auto notConverted = withCode(diagnostics, "HTMLUI-IMP-0010");
    ASSERT_EQ(notConverted.size(), 1u) << dump(diagnostics);
    // the command to run names the saved file, not a scratch copy
    EXPECT_NE(notConverted[0].suggestion.find(entry->path), std::string::npos) << notConverted[0].suggestion;
    EXPECT_EQ(notConverted[0].suggestion.find("jasper-video-import"), std::string::npos) << notConverted[0].suggestion;
}

// ---------------------------------------------------------------------------------------------
// Lottie
// ---------------------------------------------------------------------------------------------

namespace {

    void expectAnimationWithImage(const TempProject& project, const ImportEntry& entry){
        EXPECT_EQ(entry.kind, "lottie");
        EXPECT_EQ(entry.path.substr(entry.path.size() - 15), "/animation.json") << entry.path;
        ASSERT_TRUE(project.exists("assets/" + entry.path));
        const nlohmann::json animation = nlohmann::json::parse(project.read("assets/" + entry.path));
        ASSERT_TRUE(animation.contains("layers"));
        bool found = false;
        for (const nlohmann::json& asset : animation["assets"]){
            if (!asset.contains("p"))
                continue;
            found = true;
            EXPECT_EQ(asset["u"], "images/");
            const std::string image = entry.path.substr(0, entry.path.size() - 14) + "images/" + asset["p"].get<std::string>();
            EXPECT_TRUE(project.exists("assets/" + image)) << image;
            EXPECT_NE(std::find(entry.files.begin(), entry.files.end(), image), entry.files.end());
        }
        EXPECT_TRUE(found);
        EXPECT_EQ(entry.files.front(), entry.path);
    }

}

TEST(Lottie, JsonWithExternalImages){
    TempProject project;
    std::vector<compat::Diagnostic> diagnostics;
    const ImportedResource result = importOne(project, "https://lottie.host/anim.json", &diagnostics);
    ASSERT_TRUE(result.ok) << result.error << dump(diagnostics);
    expectAnimationWithImage(project, *result.entry());
    EXPECT_EQ(result.entry()->source, "LottieFiles");
    EXPECT_EQ(result.entry()->licenseName, "Lottie Simple License (verify on lottiefiles.com)");
    const auto notes = withCode(diagnostics, "HTMLUI-IMP-0008");
    ASSERT_EQ(notes.size(), 1u) << dump(diagnostics);
    EXPECT_NE(notes[0].message.find("Lottie Simple License"), std::string::npos);
}

TEST(Lottie, DotLottieV1AndV2){
    for (const char* url : {"https://lottie.host/anim-v1.lottie", "https://lottie.host/anim-v2.lottie"}){
        TempProject project;
        std::vector<compat::Diagnostic> diagnostics;
        const ImportedResource result = importOne(project, url, &diagnostics);
        ASSERT_TRUE(result.ok) << url << ": " << result.error << dump(diagnostics);
        expectAnimationWithImage(project, *result.entry());
        // the active animation ("main", not the first "intro")
        const nlohmann::json animation = nlohmann::json::parse(project.read("assets/" + result.entry()->path));
        EXPECT_EQ(animation["nm"], "fixture") << url;
    }
}

TEST(Lottie, LottieFilesPage){
    EXPECT_TRUE(LottieImporter::isLottieFilesPage("https://lottiefiles.com/free-animation/x"));
    EXPECT_FALSE(LottieImporter::isLottieFilesPage("https://lottie.host/a.json"));
    EXPECT_EQ(LottieImporter::findAnimationUrls(readFile(fixtureDir() / "lfpage/animation.html")),
              (std::vector<std::string>{"https://lottie.host/anim-v2.lottie"}));

    TempProject project;
    const std::string page = "https://lottiefiles.com/free-animation/loading-paperplane";
    const ImportResult result = importUrl(project.root(), page, "");
    ASSERT_TRUE(result.ok) << result.error;
    ASSERT_EQ(result.replaced.size(), 1u);
    EXPECT_EQ(result.replaced[0].first, page);
    EXPECT_EQ(result.replaced[0].second.substr(result.replaced[0].second.size() - 15), "/animation.json");
    // both the page and the animation file resolve to it
    const compat::ImportManifest manifest = project.manifest();
    ASSERT_NE(manifest.find(page), nullptr);
    ASSERT_NE(manifest.find("https://lottie.host/anim-v2.lottie"), nullptr);
    EXPECT_EQ(manifest.find(page)->path, manifest.find("https://lottie.host/anim-v2.lottie")->path);
}

TEST(Lottie, NotAnAnimation){
    TempProject project(false);
    const fs::path mirror = project.root() / "mirror";
    writeFile(mirror / "lottie.host" / HttpClient::urlQuote("/not-animation.json"), "{\"name\":\"x\"}");
    ScopedEnv env("JASPERENGINE_IMPORT_MIRROR", mirror.string().c_str());
    std::vector<compat::Diagnostic> diagnostics;
    const ImportedResource result = importOne(project, "https://lottie.host/not-animation.json", &diagnostics);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(withCode(diagnostics, "HTMLUI-IMP-0003").size(), 1u) << dump(diagnostics);
    EXPECT_FALSE(project.exists("assets/htmlui-imports/manifest.json"));
}

// ---------------------------------------------------------------------------------------------
// errors
// ---------------------------------------------------------------------------------------------

TEST(Errors, HttpErrorsAndRedirects){
    TempProject project;
    for (const char* url : {"https://status.example.com/404", "https://status.example.com/500"}){
        std::vector<compat::Diagnostic> diagnostics;
        const ImportedResource result = importOne(project, url, &diagnostics);
        EXPECT_FALSE(result.ok);
        const auto failed = withCode(diagnostics, "HTMLUI-IMP-0002");
        ASSERT_EQ(failed.size(), 1u) << dump(diagnostics);
        EXPECT_EQ(failed[0].severity, compat::Severity::Error);
        EXPECT_NE(failed[0].message.find("HTTP"), std::string::npos);
    }
    EXPECT_FALSE(project.exists("assets/htmlui-imports/manifest.json"));

    // a redirect lands on the SVG; the entry keeps the URL the document has
    const ImportedResource redirected = importOne(project, "https://redirect.example.com");
    ASSERT_TRUE(redirected.ok) << redirected.error;
    EXPECT_EQ(redirected.entry()->kind, "svg");
    EXPECT_NE(project.manifest().find("https://redirect.example.com"), nullptr);
}

TEST(Errors, NotARemoteUrl){
    TempProject project;
    const ImportResult result = importUrl(project.root(), "ui/local.png", "");
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("not an http(s) URL"), std::string::npos);
}
