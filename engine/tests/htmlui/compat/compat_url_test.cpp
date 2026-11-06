// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// UrlResolver: every row of the phase-2 §3.7.4 table, data: URIs, path normalization, and
// the diagnostics it reports.

#include "htmlui/compat/Diagnostics.h"
#include "htmlui/compat/Hash.h"
#include "htmlui/compat/ImportManifest.h"
#include "htmlui/compat/SourceFiles.h"
#include "htmlui/compat/UrlResolver.h"
#include "htmlui/compat/VirtualFiles.h"

#include <gtest/gtest.h>

#include <map>
#include <set>

using namespace jasperengine::htmlui::compat;

namespace {

    // a small project: ui/menu.html, ui/img/a.png, fonts/x.ttf, htmlui-imports/...
    FileAccess project(){
        static const std::set<std::string> existing = {
            "ui/menu.html", "ui/img/a.png", "ui/icons.svg", "fonts/x.ttf", "ui/my file.png",
            "htmlui-imports/images/example.com/1234-photo.jpg",
        };
        FileAccess access;
        access.exists = [](const std::string& path){ return existing.count(path) > 0; };
        access.read = [](const std::string& path, std::string& content){
            if (!existing.count(path))
                return false;
            content = "x";
            return true;
        };
        return access;
    }

    ImportManifest manifestWithPhoto(){
        ImportManifest manifest;
        ImportEntry photo;
        photo.url = ImportManifest::normalizeUrl("https://images.example.com/photo?w=800");
        photo.kind = "image";
        photo.path = "htmlui-imports/images/example.com/1234-photo.jpg";
        manifest.add(photo);
        ImportEntry gone;
        gone.url = ImportManifest::normalizeUrl("https://cdn.example.com/gone.css");
        gone.kind = "stylesheet";
        gone.path = "htmlui-imports/css/cdn.example.com/gone.css";
        manifest.add(gone);
        return manifest;
    }

}

TEST(CompatUrl, Classify){
    EXPECT_EQ(UrlResolver::classify(""), UrlKind::Empty);
    EXPECT_EQ(UrlResolver::classify("   "), UrlKind::Empty);
    EXPECT_EQ(UrlResolver::classify("img/a.png"), UrlKind::Relative);
    EXPECT_EQ(UrlResolver::classify("../fonts/x.ttf"), UrlKind::Relative);
    EXPECT_EQ(UrlResolver::classify("/ui/a.png"), UrlKind::RootRelative);
    EXPECT_EQ(UrlResolver::classify("asset://ui/a.png"), UrlKind::RootRelative);
    EXPECT_EQ(UrlResolver::classify("https://fonts.googleapis.com/css2?family=Inter"), UrlKind::Remote);
    EXPECT_EQ(UrlResolver::classify("HTTP://example.com/a.png"), UrlKind::Remote);
    EXPECT_EQ(UrlResolver::classify("//cdn.example.com/a.css"), UrlKind::Remote);
    EXPECT_EQ(UrlResolver::classify("data:image/png;base64,AAAA"), UrlKind::Data);
    EXPECT_EQ(UrlResolver::classify("data://save.json"), UrlKind::EnginePath);
    EXPECT_EQ(UrlResolver::classify("lua://ui.lua"), UrlKind::EnginePath);
    EXPECT_EQ(UrlResolver::classify("#section"), UrlKind::Fragment);
    EXPECT_EQ(UrlResolver::classify("javascript:void(0)"), UrlKind::OtherScheme);
    EXPECT_EQ(UrlResolver::classify("mailto:a@b.c"), UrlKind::OtherScheme);
    EXPECT_EQ(UrlResolver::classify("tel:123"), UrlKind::OtherScheme);
    EXPECT_EQ(UrlResolver::classify("@htmlui/ua.rcss"), UrlKind::Virtual);
    EXPECT_EQ(UrlResolver::classify("/@htmlui/ua/check.svg"), UrlKind::Virtual);
    EXPECT_EQ(UrlResolver::classify("C:/x.png"), UrlKind::Relative);
}

TEST(CompatUrl, RelativeAndRootRelative){
    const UrlResolver resolver(project());

    ResolvedUrl a = resolver.resolve("img/a.png", "ui/menu.html", "image");
    EXPECT_EQ(a.kind, UrlKind::Relative);
    EXPECT_EQ(a.assetPath, "ui/img/a.png");
    EXPECT_EQ(a.emitted, "asset://ui/img/a.png");
    EXPECT_TRUE(a.exists);
    EXPECT_TRUE(a.local());

    ResolvedUrl up = resolver.resolve("../fonts/x.ttf", "ui/menu.css", "font");
    EXPECT_EQ(up.assetPath, "fonts/x.ttf");
    EXPECT_EQ(up.emitted, "asset://fonts/x.ttf");
    EXPECT_TRUE(up.exists);

    ResolvedUrl dots = resolver.resolve("./img/../img/./a.png", "ui/menu.html", "image");
    EXPECT_EQ(dots.assetPath, "ui/img/a.png");

    ResolvedUrl root = resolver.resolve("asset://ui/img/a.png", "deep/dir/page.html", "image");
    EXPECT_EQ(root.kind, UrlKind::RootRelative);
    EXPECT_EQ(root.assetPath, "ui/img/a.png");
    EXPECT_EQ(root.emitted, "asset://ui/img/a.png");

    ResolvedUrl scheme = resolver.resolve("asset://ui/img/a.png", "x.html", "image");
    EXPECT_EQ(scheme.assetPath, "ui/img/a.png");

    // query and fragment are not part of the file; the fragment is kept
    ResolvedUrl fragment = resolver.resolve("icons.svg?v=2#home", "ui/menu.html", "svg");
    EXPECT_EQ(fragment.assetPath, "ui/icons.svg");
    EXPECT_EQ(fragment.fragment, "#home");
    EXPECT_EQ(fragment.emitted, "asset://ui/icons.svg#home");
    EXPECT_TRUE(fragment.exists);

    // percent-encoded names
    ResolvedUrl space = resolver.resolve("my%20file.png", "ui/menu.html", "image");
    EXPECT_EQ(space.assetPath, "ui/my file.png");
    EXPECT_TRUE(space.exists);

    // backslashes
    ResolvedUrl windows = resolver.resolve("img\\a.png", "ui\\menu.html", "image");
    EXPECT_EQ(windows.assetPath, "ui/img/a.png");

    // above the assets directory
    ResolvedUrl outside = resolver.resolve("../../ui/img/a.png", "ui/menu.html", "image");
    EXPECT_TRUE(outside.outsideAssets);
    EXPECT_FALSE(outside.exists);
}

TEST(CompatUrl, MissingLocalFileIsKeptAndReported){
    const UrlResolver resolver(project());
    SourceFiles files;
    const uint32_t file = files.add("ui/menu.html", "<img src=\"img/b.png\">");
    DiagnosticSink sink(files);
    const ResolvedUrl missing = resolver.resolve("img/b.png", "ui/menu.html", "image", SourceRef{file, 10, 9}, &sink);
    EXPECT_FALSE(missing.exists);
    EXPECT_EQ(missing.emitted, "asset://ui/img/b.png");   // kept
    ASSERT_EQ(sink.size(), 1u);
    const Diagnostic& d = sink.diagnostics()[0];
    EXPECT_EQ(d.code, codes::FileNotFound);
    EXPECT_EQ(d.severity, Severity::Error);
    EXPECT_EQ(d.file, "ui/menu.html");
    EXPECT_EQ(d.line, 1u);
    EXPECT_EQ(d.column, 11u);
    EXPECT_EQ(d.subject, "img/b.png");

    // nothing reported for a file that exists
    DiagnosticSink quiet(files);
    resolver.resolve("img/a.png", "ui/menu.html", "image", SourceRef{file, 10, 9}, &quiet);
    EXPECT_EQ(quiet.size(), 0u);
}

TEST(CompatUrl, RemoteThroughTheManifest){
    const ImportManifest manifest = manifestWithPhoto();
    const UrlResolver resolver(project(), &manifest);
    SourceFiles files;
    const uint32_t file = files.add("ui/menu.css", "x");
    DiagnosticSink sink(files);

    const ResolvedUrl hit = resolver.resolve("http://IMAGES.example.com/photo?w=800#top", "ui/menu.css", "image", SourceRef{file, 0, 1}, &sink);
    EXPECT_EQ(hit.kind, UrlKind::Remote);
    ASSERT_NE(hit.entry, nullptr);
    EXPECT_EQ(hit.assetPath, "htmlui-imports/images/example.com/1234-photo.jpg");
    EXPECT_EQ(hit.emitted, "asset://htmlui-imports/images/example.com/1234-photo.jpg#top");
    EXPECT_TRUE(hit.exists);
    EXPECT_EQ(sink.size(), 0u);

    const ResolvedUrl miss = resolver.resolve("https://fonts.googleapis.com/css2?family=Inter", "ui/menu.css", "stylesheet", SourceRef{file, 0, 1}, &sink);
    EXPECT_EQ(miss.entry, nullptr);
    EXPECT_TRUE(miss.dropped());
    ASSERT_EQ(sink.size(), 1u);
    EXPECT_EQ(sink.diagnostics()[0].code, codes::NotImported);
    EXPECT_EQ(sink.diagnostics()[0].severity, Severity::Error);
    EXPECT_EQ(sink.diagnostics()[0].suggestion, "import it: load the document in the editor (it imports remote resources), run jasperengine-editor import-ui, or MCP import_files from_document");
    EXPECT_NE(sink.diagnostics()[0].message.find("fonts fall back"), std::string::npos);

    // an entry whose file is gone
    DiagnosticSink gone(files);
    const ResolvedUrl stale = resolver.resolve("//cdn.example.com/gone.css", "ui/menu.css", "stylesheet", SourceRef{file, 0, 1}, &gone);
    ASSERT_NE(stale.entry, nullptr);
    EXPECT_FALSE(stale.exists);
    ASSERT_EQ(gone.size(), 1u);
    EXPECT_EQ(gone.diagnostics()[0].code, codes::ManifestFileMissing);
    EXPECT_EQ(gone.diagnostics()[0].severity, Severity::Warning);
}

TEST(CompatUrl, DataUris){
    const UrlResolver resolver(project());

    // base64 PNG header bytes
    const ResolvedUrl png = resolver.resolve("data:image/png;base64,iVBORw0KGgo=", "ui/menu.html", "image");
    EXPECT_EQ(png.kind, UrlKind::Data);
    ASSERT_NE(png.data, nullptr);
    EXPECT_EQ(*png.data, std::string("\x89PNG\r\n\x1a\n", 8));
    EXPECT_EQ(png.mimeType, "image/png");
    EXPECT_EQ(png.assetPath, "@htmlui/data/" + sha1Hex(*png.data).substr(0, 12) + ".png");
    EXPECT_EQ(png.emitted, "/" + png.assetPath);

    // percent-encoded SVG, parameters ignored, whitespace in base64 skipped
    const ResolvedUrl svg = resolver.resolve("data:image/svg+xml;charset=utf-8,%3Csvg%20xmlns%3D'x'%2F%3E", "ui/menu.html", "image");
    ASSERT_NE(svg.data, nullptr);
    EXPECT_EQ(*svg.data, "<svg xmlns='x'/>");
    EXPECT_EQ(svg.assetPath.substr(svg.assetPath.size() - 4), ".svg");

    std::optional<ParsedDataUri> spaced = UrlResolver::parseDataUri("data:;base64,aGVs\n bG8=");
    ASSERT_TRUE(spaced.has_value());
    EXPECT_EQ(spaced->data, "hello");
    EXPECT_EQ(spaced->mimeType, "text/plain");
    EXPECT_TRUE(spaced->base64);

    EXPECT_FALSE(UrlResolver::parseDataUri("data:image/png;base64").has_value());
    EXPECT_FALSE(UrlResolver::parseDataUri("img/a.png").has_value());

    EXPECT_EQ(UrlResolver::extensionForMime("image/jpeg"), "jpg");
    EXPECT_EQ(UrlResolver::extensionForMime("IMAGE/GIF"), "gif");
    EXPECT_EQ(UrlResolver::extensionForMime("application/json"), "json");
    EXPECT_EQ(UrlResolver::extensionForMime("font/ttf"), "ttf");
    EXPECT_EQ(UrlResolver::extensionForMime("font/otf"), "otf");
    EXPECT_EQ(UrlResolver::extensionForMime("application/x-unknown"), "bin");
}

TEST(CompatUrl, FragmentsVirtualAndOtherSchemes){
    const UrlResolver resolver(project());
    SourceFiles files;
    const uint32_t file = files.add("ui/menu.html", "x");
    DiagnosticSink sink(files);

    const ResolvedUrl anchor = resolver.resolve("#settings", "ui/menu.html", "link", SourceRef{file, 0, 1}, &sink);
    EXPECT_EQ(anchor.kind, UrlKind::Fragment);
    EXPECT_EQ(anchor.emitted, "#settings");

    const ResolvedUrl js = resolver.resolve("javascript:alert(1)", "ui/menu.html", "link", SourceRef{file, 0, 1}, &sink);
    EXPECT_TRUE(js.dropped());
    ASSERT_EQ(sink.size(), 1u);
    EXPECT_EQ(sink.diagnostics()[0].code, codes::LinkNotNavigable);
    EXPECT_EQ(sink.diagnostics()[0].severity, Severity::Info);

    VirtualFiles::add("@htmlui/test/url.svg", "<svg/>");
    const ResolvedUrl virtualFile = resolver.resolve("/@htmlui/test/url.svg", "ui/menu.html", "image");
    EXPECT_EQ(virtualFile.kind, UrlKind::Virtual);
    EXPECT_EQ(virtualFile.assetPath, "@htmlui/test/url.svg");
    EXPECT_EQ(virtualFile.emitted, "/@htmlui/test/url.svg");
    EXPECT_TRUE(virtualFile.exists);
    VirtualFiles::remove("@htmlui/test/url.svg");

    const ResolvedUrl empty = resolver.resolve("", "ui/menu.html", "image");
    EXPECT_EQ(empty.kind, UrlKind::Empty);
    EXPECT_TRUE(empty.dropped());
}
