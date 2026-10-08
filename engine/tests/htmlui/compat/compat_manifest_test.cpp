// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// ImportManifest: URL normalization, parsing (phase-2 §3.13.2 format), lookups, loading
// through a FileAccess and through the engine's files with reload.

#include "CompatTestCommon.h"

#include "htmlui/compat/ImportManifest.h"

#include <gtest/gtest.h>

using namespace jasperengine::htmlui::compat;
using namespace jasperengine::test::compat;

namespace {

    const char* manifestJson = R"({
  "version": 1,
  "entries": [
    {
      "url": "https://fonts.googleapis.com/css2?family=Orbitron:wght@700&family=Inter:wght@400;600&display=swap",
      "kind": "google-fonts-css",
      "path": "htmlui-imports/fonts/orbitron-inter-8f2a1c.css",
      "files": ["htmlui-imports/fonts/orbitron/Orbitron-700.ttf", "htmlui-imports/fonts/inter/Inter-400.ttf", "htmlui-imports/fonts/inter/Inter-600.ttf"],
      "license": "htmlui-imports/fonts/orbitron-inter-8f2a1c.licenses.txt",
      "licenseName": "SIL Open Font License 1.1",
      "source": "Google Fonts",
      "fetched": "2026-10-07T12:00:00Z",
      "sha256": "abc"
    },
    {
      "url": "HTTP://Lottie.Host/anim.json#ignored",
      "kind": "lottie",
      "path": "htmlui-imports/lottie/0123456789ab-anim/animation.json"
    },
    { "url": "https://broken.example.com/no-path" },
    "not an object"
  ]
})";

}

TEST(CompatManifest, NormalizeUrl){
    EXPECT_EQ(ImportManifest::normalizeUrl("HTTP://Fonts.Example.COM/A/b.css?X=1#frag"), "https://fonts.example.com/A/b.css?X=1");
    EXPECT_EQ(ImportManifest::normalizeUrl("//cdn.example.com/x.css"), "https://cdn.example.com/x.css");
    EXPECT_EQ(ImportManifest::normalizeUrl("https://example.com:443/a"), "https://example.com/a");
    EXPECT_EQ(ImportManifest::normalizeUrl("http://example.com:80/a"), "https://example.com/a");
    EXPECT_EQ(ImportManifest::normalizeUrl("https://example.com:8080/a"), "https://example.com:8080/a");
    EXPECT_EQ(ImportManifest::normalizeUrl("https://example.com"), "https://example.com/");
    EXPECT_EQ(ImportManifest::normalizeUrl("https://example.com?q=1"), "https://example.com/?q=1");
    EXPECT_EQ(ImportManifest::normalizeUrl("  https://example.com/a  "), "https://example.com/a");
    // not a remote URL: unchanged (but trimmed, without fragment)
    EXPECT_EQ(ImportManifest::normalizeUrl("img/a.png#x"), "img/a.png");
}

TEST(CompatManifest, ParseAndFind){
    ImportManifest manifest;
    std::string error;
    ASSERT_TRUE(ImportManifest::parse(manifestJson, manifest, &error)) << error;
    EXPECT_TRUE(manifest.loaded());
    ASSERT_EQ(manifest.entries().size(), 2u);   // entries without a path or not objects are skipped

    const ImportEntry* fonts = manifest.find("https://fonts.googleapis.com/css2?family=Orbitron:wght@700&family=Inter:wght@400;600&display=swap");
    ASSERT_NE(fonts, nullptr);
    EXPECT_EQ(fonts->kind, "google-fonts-css");
    EXPECT_EQ(fonts->path, "htmlui-imports/fonts/orbitron-inter-8f2a1c.css");
    ASSERT_EQ(fonts->files.size(), 3u);
    EXPECT_EQ(fonts->files[1], "htmlui-imports/fonts/inter/Inter-400.ttf");
    EXPECT_EQ(fonts->licenseName, "SIL Open Font License 1.1");
    EXPECT_EQ(fonts->source, "Google Fonts");
    EXPECT_EQ(fonts->fetched, "2026-10-07T12:00:00Z");
    EXPECT_EQ(fonts->sha256, "abc");
    // the same URL written differently
    EXPECT_EQ(manifest.find("http://FONTS.googleapis.com/css2?family=Orbitron:wght@700&family=Inter:wght@400;600&display=swap#x"), fonts);
    // the query is part of the identity
    EXPECT_EQ(manifest.find("https://fonts.googleapis.com/css2?family=Inter"), nullptr);

    const ImportEntry* lottie = manifest.find("https://lottie.host/anim.json");
    ASSERT_NE(lottie, nullptr);
    EXPECT_EQ(lottie->url, "https://lottie.host/anim.json");
    EXPECT_TRUE(lottie->files.empty());

    // sorted by url
    EXPECT_LT(manifest.entries()[0].url, manifest.entries()[1].url);
}

TEST(CompatManifest, RejectsOtherFormats){
    ImportManifest manifest;
    std::string error;
    EXPECT_FALSE(ImportManifest::parse("not json", manifest, &error));
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(ImportManifest::parse("[]", manifest, &error));
    EXPECT_FALSE(ImportManifest::parse(R"({"version": 2, "entries": []})", manifest, &error));
    EXPECT_FALSE(ImportManifest::parse(R"({"entries": []})", manifest, &error));
    EXPECT_TRUE(ImportManifest::parse(R"({"version": 1})", manifest, &error));
    EXPECT_TRUE(manifest.empty());
    EXPECT_TRUE(manifest.loaded());
}

TEST(CompatManifest, AddKeepsEntriesSortedAndUnique){
    ImportManifest manifest;
    ImportEntry b;
    b.url = "https://b.example.com/x";
    b.path = "b";
    ImportEntry a;
    a.url = "https://a.example.com/x";
    a.path = "a";
    manifest.add(b);
    manifest.add(a);
    ImportEntry a2 = a;
    a2.path = "a2";
    manifest.add(a2);
    ASSERT_EQ(manifest.entries().size(), 2u);
    EXPECT_EQ(manifest.entries()[0].path, "a2");
    EXPECT_EQ(manifest.entries()[1].path, "b");
}

TEST(CompatManifest, LoadThroughFileAccess){
    FileAccess files;
    files.read = [](const std::string& path, std::string& content){
        if (path != ImportManifest::kPath)
            return false;
        content = manifestJson;
        return true;
    };
    const ImportManifest manifest = ImportManifest::load(files);
    EXPECT_TRUE(manifest.loaded());
    EXPECT_EQ(manifest.entries().size(), 2u);

    FileAccess none;
    none.read = [](const std::string&, std::string&){ return false; };
    const ImportManifest missing = ImportManifest::load(none);
    EXPECT_FALSE(missing.loaded());
    EXPECT_TRUE(missing.empty());
}

TEST(CompatManifest, ProjectManifestLoadsOnceAndReloads){
    // the test assets have no htmlui-imports/manifest.json
    ImportManifest::reload();
    const ImportManifest& manifest = ImportManifest::get();
    EXPECT_FALSE(manifest.loaded());
    EXPECT_TRUE(manifest.empty());
    EXPECT_EQ(&ImportManifest::get(), &manifest);   // cached
    std::shared_ptr<const ImportManifest> current = ImportManifest::current();
    EXPECT_EQ(current.get(), &manifest);

    ImportManifest::notifyFileChanged("ui/other.css");
    EXPECT_EQ(ImportManifest::current().get(), current.get());
    ImportManifest::notifyFileChanged("/htmlui-imports/manifest.json");
    EXPECT_NE(ImportManifest::current().get(), current.get());   // read again
    // the old snapshot stays valid for its holders
    EXPECT_FALSE(current->loaded());
}
