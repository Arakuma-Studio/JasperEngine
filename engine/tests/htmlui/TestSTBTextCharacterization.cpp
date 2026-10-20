// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Characterization of STBText (the component UI's text: UISystem, Text, Button, InputField).
// Every expected value below was recorded on the STBText that existed before the text stack
// was split into FontFace / FontRegistry / TextShaper / GlyphAtlas, so these tests prove
// the refactor kept the exact output: sizes, quads, UVs, caret positions and extents,
// line metrics, atlas pixels and the atlas version sequence.
//
// A signature is a readable summary plus FNV-1a hashes of the full output; on a mismatch
// the test prints the actual signature (the way the values were recorded).

#include "harness/TestSystem.h"

#include "buffer/InterleavedBuffer.h"
#include "texture/TextureData.h"
#include "util/STBText.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    struct Fnv {
        uint64_t value = 1469598103934665603ULL;

        void add(const void* data, size_t size){
            const unsigned char* bytes = (const unsigned char*)data;
            for (size_t i = 0; i < size; i++){
                value ^= bytes[i];
                value *= 1099511628211ULL;
            }
        }
        void addFloat(float f){
            add(&f, sizeof(f));
        }
    };

    std::string hex(uint64_t value){
        char text[32];
        snprintf(text, sizeof(text), "%016llx", (unsigned long long)value);
        return text;
    }

    std::string num(float value){
        char text[32];
        snprintf(text, sizeof(text), "%.9g", value);
        return text;
    }

    struct TextRun {
        std::string text;
        unsigned int width = 0;
        unsigned int height = 0;
        bool fixedWidth = false;
        bool fixedHeight = false;
        bool multiline = false;
        bool invert = false;
    };

    // one createText call as UISystem::createText makes it
    std::string runText(STBText& stb, const TextRun& run, bool withPositions){
        InterleavedBuffer buffer;
        buffer.addAttribute(AttributeType::POSITION, 3);
        buffer.addAttribute(AttributeType::TEXCOORD1, 2);

        std::vector<uint16_t> indices;
        std::vector<Vector2> charPositions;
        std::vector<CharExtent> charExtents;
        unsigned int width = run.width;
        unsigned int height = run.height;

        stb.createText(run.text, &buffer, indices, charPositions, charExtents, width, height,
                       run.fixedWidth, run.fixedHeight, run.multiline, run.invert);

        Fnv geometry;
        Attribute* positions = buffer.getAttribute(AttributeType::POSITION);
        Attribute* texcoords = buffer.getAttribute(AttributeType::TEXCOORD1);
        for (unsigned int i = 0; i < buffer.getCount(); i++){
            Vector3 p = buffer.getVector3(positions, i);
            Vector2 t = buffer.getVector2(texcoords, i);
            geometry.addFloat(p.x); geometry.addFloat(p.y); geometry.addFloat(p.z);
            geometry.addFloat(t.x); geometry.addFloat(t.y);
        }
        geometry.add(indices.data(), indices.size() * sizeof(uint16_t));

        Fnv carets;
        for (const Vector2& p : charPositions){
            carets.addFloat(p.x); carets.addFloat(p.y);
        }
        for (const CharExtent& e : charExtents){
            carets.addFloat(e.left); carets.addFloat(e.right); carets.addFloat(e.y);
            carets.add(&e.rtl, sizeof(e.rtl));
        }

        std::string out = "size=" + std::to_string(width) + "x" + std::to_string(height) +
                          " vertices=" + std::to_string(buffer.getCount()) +
                          " quads=" + std::to_string(indices.size() / 6) +
                          " chars=" + std::to_string(charPositions.size()) +
                          " version=" + std::to_string(stb.getAtlasVersion()) +
                          " geometry=" + hex(geometry.value) +
                          " carets=" + hex(carets.value);

        if (withPositions){
            out += " pos=";
            for (size_t i = 0; i < charPositions.size(); i++){
                out += (i ? "," : "") + num(charPositions[i].x) + ":" + num(charPositions[i].y);
            }
        }

        return out;
    }

    std::string atlasSignature(STBText& stb){
        TextureData* data = stb.getTextureData();
        if (!data)
            return "atlas=none";
        Fnv pixels;
        pixels.add(data->getData(), data->getSize());
        return "atlas=" + std::to_string(data->getWidth()) + "x" + std::to_string(data->getHeight()) +
               " pixels=" + hex(pixels.value);
    }

    std::string metrics(STBText& stb){
        return "ascent=" + num(stb.getAscent()) + " descent=" + num(stb.getDescent()) +
               " lineGap=" + num(stb.getLineGap()) + " lineHeight=" + std::to_string(stb.getLineHeight());
    }

}

TEST(STBTextCharacterizationTest, LatinLine){
    STBText stb;
    ASSERT_NE(stb.load("", {}, 24), nullptr);
    EXPECT_EQ(metrics(stb), "ascent=19 descent=-5 lineGap=0 lineHeight=24");
    // load() resets the atlas once
    EXPECT_EQ(stb.getAtlasVersion(), 1ul);

    TextRun run;
    run.text = "Hello, World! AV Ta fi 0123";
    EXPECT_EQ(runText(stb, run, true), "size=239x24 vertices=104 quads=26 chars=27 version=20 geometry=c9f7be8d0d9df887 "
              "carets=8e28ccf20616cce9 pos=14.6099997:0,25.4699993:0,30.4499989:0,35.4300003:0,47.1100006:0,"
              "51.1399994:0,56.2200012:0,74.0800018:0,85.7600021:0,92.7000046:0,97.6800079:0,109.230011:0,"
              "114.51001:0,119.590012:0,132.080017:0,145.12001:0,149.800003:0,160.889999:0,172.029999:0,"
              "177.110001:0,188.460007:0,188.460007:0,193.540009:0,205.050003:0,216.559998:0,228.069992:0,"
              "239.579987:0");
    EXPECT_EQ(atlasSignature(stb), "atlas=512x512 pixels=886f9c335494ada3");

    // the same text again adds no glyph: the version stays
    EXPECT_EQ(runText(stb, run, false), "size=239x24 vertices=104 quads=26 chars=27 version=20 geometry=c9f7be8d0d9df887 "
              "carets=8e28ccf20616cce9");

    // flipped (UI cameras with flipY), as UISystem asks for
    run.invert = true;
    EXPECT_EQ(runText(stb, run, false), "size=239x24 vertices=104 quads=26 chars=27 version=20 geometry=baf6d31588b17e2f "
              "carets=8e28ccf20616cce9");

    EXPECT_EQ(num(stb.getCharWidth('W')) + " " + num(stb.getCharWidth(' ')) + " " + num(stb.getCharWidth(0x4E2D)), "18.1700001 5.07999992 9.07999992");

    // empty text: one degenerate triangle, as before
    run.text = "";
    run.invert = false;
    EXPECT_EQ(runText(stb, run, false), "size=0x24 vertices=3 quads=0 chars=0 version=21 geometry=7f265a9391a67118 carets=14650fb0739d0383");
}

TEST(STBTextCharacterizationTest, ArabicShapingBidi){
    STBText stb;
    ASSERT_NE(stb.load("", {}, 32), nullptr);

    TextRun run;
    // (the spaces, digits and punctuation between Arabic words are the primary face's: each codepoint takes the first face
    // that has it, as Chromium falls back)
    run.text = "Hello \xd9\x85\xd8\xb1\xd8\xad\xd8\xa8\xd8\xa7 \xd8\xa8\xd8\xa7\xd9\x84\xd8\xb9\xd8\xa7\xd9\x84\xd9\x85 123 world";
    EXPECT_EQ(runText(stb, run, true), "size=272x32 vertices=124 quads=31 chars=29 version=22 geometry=a56ead26708582ad "
              "carets=b6609a4090298b81 pos=19.4799995:0,33.9599991:0,40.5999985:0,47.2399979:0,62.8133316:0,"
              "69.5866623:0,198.233902:0,190.279358:0,184.885422:0,175.961182:0,170.764206:0,166.355118:0,"
              "159.581787:0,155.506027:0,151.096939:0,147.157547:0,139.263611:0,134.854523:0,130.915131:0,"
              "122.399986:0,84.9333267:0,100.279991:0,115.626656:0,205.007233:0,225.527237:0,241.100571:0,"
              "250.353897:0,256.993896:0,272.39389:0");

    // a paragraph starting with Arabic is right-to-left
    run.text = "\xd8\xb9\xd8\xb1\xd8\xa8\xd9\x8a (abc) \xd9\x84\xd8\xa7";
    EXPECT_EQ(runText(stb, run, true), "size=114x32 vertices=60 quads=15 chars=13 version=32 geometry=52815618d7e697e9 "
              "carets=975ad04066ef7396 pos=114.092133:0,106.470917:0,100.925461:0,96.8497009:0,85.6981812:0,"
              "78.9248505:0,39.7915154:0,55.1248474:0,69.4181824:0,24.9381828:0,15.5915155:0,8.81818199:0,5.5:0");
    EXPECT_EQ(atlasSignature(stb), "atlas=512x512 pixels=6bc7a85364d3d535");
}

TEST(STBTextCharacterizationTest, MultilineWrap){
    STBText stb;
    ASSERT_NE(stb.load("", {}, 20), nullptr);
    EXPECT_EQ(metrics(stb), "ascent=15.833334 descent=-4.16666698 lineGap=0 lineHeight=20");

    TextRun run;
    run.text = "A long paragraph that must wrap inside a narrow box.\nSecond paragraph after a newline\n\nand an empty line";
    run.width = 160;
    run.height = 0;
    run.fixedWidth = true;
    run.multiline = true;
    EXPECT_EQ(runText(stb, run, true), "size=160x140 vertices=392 quads=98 chars=101 version=26 geometry=7938b9f4a6bbcf3b "
              "carets=f178b60906fca055 pos=11.1333342:0,15.3666677:0,19.5166683:0,29.2500019:0,38.6750031:0,"
              "48.2583389:0,52.4916725:0,62.0750046:0,71.3583374:0,76.8083344:0,86.0916672:0,95.6750031:0,"
              "101.125:0,110.408333:0,119.991669:0,129.391663:0,129.391663:0,5.58333349:20,14.9833336:20,"
              "24.2666664:20,29.8500004:20,34.0833359:20,49.0500031:20,58.4583359:20,67.2666702:20,72.8500061:20,"
              "77.0833435:20,89.9083405:20,95.3583374:20,104.64167:20,114.225006:20,114.225006:20,4.1500001:40,"
              "13.5750008:40,22.3833351:40,26.5333347:40,36.1583366:40,45.2083359:40,49.4416695:40,58.7250023:40,"
              "62.9583359:40,72.3833389:40,81.6666718:40,87.4500046:40,93.0666733:40,102.800003:40,115.625:40,"
              "119.858337:40,129.441666:40,139:40,147.466675:40,151.966675:40,10.1333342:60,19.1833344:60,"
              "28.1166687:60,37.8500023:60,47.2750015:60,56.9000015:60,61.1333351:60,70.7166672:60,80:60,"
              "85.4499969:60,94.7333298:60,104.316666:60,109.766663:60,119.049995:60,128.633331:60,138.033325:60,"
              "138.033325:60,9.28333378:80,15.2166672:80,20.8000011:80,29.8500023:80,35.6333351:80,39.8666687:80,"
              "49.1500015:80,53.3833351:80,62.8083344:80,71.8583374:80,84.6833344:80,88.8333359:80,92.9833374:80,"
              "102.40834:80,111.458344:80,9.28333378:120,18.708334:120,28.3333359:120,32.5666695:120,"
              "41.8500023:120,51.2750015:120,55.5083351:120,64.5583344:120,79.5250015:120,89.1083374:120,"
              "94.6916733:120,102.766678:120,107.000015:120,111.150017:120,115.300018:120,124.725021:120,"
              "133.775024:120");

    // a word longer than the box breaks inside the word
    run.text = "Supercalifragilisticexpialidocious fits nowhere";
    run.width = 90;
    EXPECT_EQ(runText(stb, run, true), "size=90x100 vertices=176 quads=44 chars=47 version=27 geometry=4b76a976da52ecef "
              "carets=10e43df974a7f944 pos=10.1333342:0,19.5416679:0,29.1250019:0,38.1750031:0,43.8000031:0,"
              "52.7333374:0,62.0166702:0,66.1666718:0,70.3166733:0,76.2500076:0,82.0333405:0,9.28333378:20,"
              "18.8666687:20,23.0166683:20,27.1666679:20,31.3166676:20,40.125:20,45.7083321:20,49.8583336:20,"
              "58.7916679:20,67.8416672:20,76.3083344:20,85.8916702:20,4.1500001:40,13.4333344:40,17.583334:40,"
              "21.7333336:40,31.3583336:40,41.0916672:40,50.0250015:40,54.1750031:40,63.9083366:40,73.3166733:40,"
              "82.1250076:40,82.1250076:40,9.45833397:60,9.45833397:60,15.0416679:60,23.8500023:60,23.8500023:60,"
              "9.42500019:80,19.1583328:80,31.9833336:80,41.3833351:80,50.4333344:80,56.0583344:80,65.1083374:80");

    // Arabic wraps with its final forms
    run.text = "\xd9\x85\xd8\xb1\xd8\xad\xd8\xa8\xd8\xa7 \xd8\xa8\xd8\xa7\xd9\x84\xd8\xb9\xd8\xa7\xd9\x84\xd9\x85 \xd9\x85\xd8\xb1\xd8\xad\xd8\xa8\xd8\xa7 \xd8\xa8\xd8\xa7\xd9\x84\xd8\xb9\xd8\xa7\xd9\x84\xd9\x85";
    run.width = 100;
    EXPECT_EQ(runText(stb, run, true), "size=100x20 vertices=124 quads=31 chars=27 version=37 geometry=51e25aa78183803d "
              "carets=4dd0752c8126119d pos=99.0257797:0,94.0541916:0,90.6829758:0,85.1053238:0,81.8572159:0,"
              "79.101532:0,74.8681946:0,72.3208466:0,69.5651627:0,67.1030426:0,62.1693268:0,59.4136429:0,"
              "56.9515228:0,51.6295509:0,47.3962173:0,42.4246254:0,39.0534134:0,33.4757614:0,30.2276535:0,"
              "27.4719715:0,23.2386379:0,20.6912899:0,17.9356079:0,15.4734859:0,10.539773:0,7.784091:0,5.32196999:0");

    // newlines without wrapping, fixed size kept
    run.text = "one\ntwo\nthree";
    run.width = 50;
    run.height = 30;
    run.fixedWidth = true;
    run.fixedHeight = true;
    run.multiline = false;
    EXPECT_EQ(runText(stb, run, true), "size=50x30 vertices=44 quads=11 chars=11 version=37 geometry=c3e0d215610d6655 "
              "carets=fc10512741a9a83c pos=9.73333359:0,19.1583328:0,28.2083321:0,5.58333349:20,18.4083347:20,"
              "28.1416683:20,5.58333349:40,14.9833336:40,20.6083336:40,29.6583328:40,38.7083321:40");
    EXPECT_EQ(atlasSignature(stb), "atlas=512x512 pixels=85c83b135e68734a");
}

TEST(STBTextCharacterizationTest, FallbackChain){
    TestSystem::get().clearLogs();

    // a project font first, a missing fallback (logged, skipped), another fallback, then
    // the built-in Roboto and Noto Sans Arabic close the chain
    STBText stb;
    ASSERT_NE(stb.load("fonts/Roboto-Bold.ttf", {"fonts/does-not-exist.ttf", "fonts/Roboto-Italic.ttf"}, 28), nullptr);
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR, "fonts/does-not-exist.ttf"), 1u);
    EXPECT_EQ(metrics(stb), "ascent=22.166666 descent=-5.83333349 lineGap=0 lineHeight=28");

    TextRun run;
    // Latin from Bold, Arabic from the built-in Arabic face, CJK and an emoji from nobody (.notdef)
    run.text = "Bold AV \xd9\x85\xd8\xb1\xd8\xad\xd8\xa8\xd8\xa7 \xe4\xb8\xad \xf0\x9f\x98\x80 end";
    EXPECT_EQ(runText(stb, run, true), "size=197x28 vertices=88 quads=22 chars=21 version=17 geometry=84e486e836bdca67 "
              "carets=ac6c0e6c35d250fc pos=15.248333:0,28.7350006:0,35.0583344:0,48.5100021:0,54.4483337:0,"
              "69.5683365:0,85.1783371:0,91.1166687:0,119.010612:0,112.050385:0,107.330688:0,99.5219727:0,"
              "94.9746246:0,124.948944:0,135.542282:0,141.480621:0,152.073959:0,158.012299:0,170.915634:0,"
              "184.30896:0,197.76062:0");
    EXPECT_EQ(atlasSignature(stb), "atlas=512x512 pixels=fbede30db5ba0b1c");

    // a missing primary font fails the whole load
    STBText missing;
    EXPECT_EQ(missing.load("fonts/does-not-exist.ttf", {}, 28), nullptr);
    EXPECT_EQ(missing.getTextureData(), nullptr);
}

TEST(STBTextCharacterizationTest, AtlasGrowth){
    STBText stb;
    ASSERT_NE(stb.load("", {}, 72), nullptr);
    EXPECT_EQ(atlasSignature(stb), "atlas=512x512 pixels=2469b722f7cd0383");

    // every printable ASCII, Latin-1, Greek and Cyrillic letter at 72 px does not fit in
    // 512x512: the atlas grows (and re-rasterizes), each step bumps the version
    std::vector<std::string> chunks;
    std::string chunk;
    auto addRange = [&](uint32_t first, uint32_t last){
        for (uint32_t c = first; c <= last; c++){
            if (c < 0x80){
                chunk += (char)c;
            }else if (c < 0x800){
                chunk += (char)(0xC0 | (c >> 6));
                chunk += (char)(0x80 | (c & 0x3F));
            }
            if (chunk.size() >= 48){
                chunks.push_back(chunk);
                chunk.clear();
            }
        }
    };
    addRange(0x21, 0x7E);
    addRange(0xC0, 0xFF);
    addRange(0x391, 0x3A9);
    addRange(0x410, 0x44F);
    chunks.push_back(chunk);

    std::string signatures;
    std::string sequence;
    for (const std::string& text : chunks){
        TextRun run;
        run.text = text;
        signatures += runText(stb, run, false) + "\n";
        sequence += std::to_string(stb.getAtlasVersion()) + "@" +
                    std::to_string(stb.getTextureData()->getWidth()) + " ";
    }
    EXPECT_EQ(signatures, "size=1544x72 vertices=192 quads=48 chars=48 version=49 geometry=e87cdb593a4823d3 "
              "carets=3c5f227f0f8d55f2\nsize=1464x72 vertices=188 quads=47 chars=47 version=96 "
              "geometry=a29ba1be8b97cd6d carets=3712fab0a2bf310c\nsize=875x72 vertices=96 quads=24 chars=24 "
              "version=120 geometry=0eaf92c25eb39ccf carets=f8f4e3d9499b11b5\nsize=780x72 vertices=96 quads=24 "
              "chars=24 version=287 geometry=9d927d922e615c3b carets=fcb74bb8ed43df5a\nsize=831x72 vertices=96 "
              "quads=24 chars=24 version=311 geometry=1704646efa511b7b carets=b9e73df3b542fe54\nsize=954x72 "
              "vertices=96 quads=24 chars=24 version=335 geometry=ad7e07733bcc84ef carets=8ba2cc64f81524c8\n"
              "size=1070x72 vertices=96 quads=24 chars=24 version=359 geometry=d4a9e8e0e3fd0e9b "
              "carets=8e5ede0c15b3a3e9\nsize=834x72 vertices=96 quads=24 chars=24 version=383 "
              "geometry=36d9078352dbedb3 carets=984fc1abbb84f68a\nsize=336x72 vertices=32 quads=8 chars=8 "
              "version=391 geometry=e8d9ece72ebbd47b carets=df5bf93e0ee7a422\n");
    EXPECT_EQ(sequence, "49@512 96@512 120@512 287@1024 311@1024 335@1024 359@1024 383@1024 391@1024 ");
    EXPECT_EQ(atlasSignature(stb), "atlas=1024x1024 pixels=a527613f3154077d");
}
