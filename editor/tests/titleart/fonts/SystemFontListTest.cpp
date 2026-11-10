// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// docs/specs/S82-title-designer.md R58, AC25: the system font list. ParsesFcList reads a captured
// `fc-list --format '%{file}\t%{index}\t%{family}\t%{style}\t%{fontformat}\n'` output; the list's
// background listing runs once per session; the platform lister (fontconfig, CoreText or
// DirectWrite) is asked for at least one font where the CI machine has fonts.

#include <gtest/gtest.h>

#include "titleart/fonts/SystemFontList.h"

#include <atomic>
#include <chrono>
#include <thread>

using namespace jasperengine::editor::titlefonts;

namespace {

    // an Ubuntu machine's fc-list, trimmed: TrueType and CFF faces, a collection's faces, a
    // variable font and one of its named instances, bitmap and Type 1 fonts, localized names,
    // fontconfig's escapes, a repeated line, a CRLF line and a broken line
    const char* CAPTURED_FC_LIST =
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf\t0\tDejaVu Sans\tBold\tTrueType\n"
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf\t0\tDejaVu Sans\tBook\tTrueType\n"
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc\t0\tNoto Sans CJK JP,Noto Sans CJK JP Regular\tRegular\tCFF\n"
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc\t1\tNoto Sans CJK KR,Noto Sans CJK KR Regular\tRegular\tCFF\n"
        "/usr/share/fonts/truetype/noto/NotoSans[wght].ttf\t0\tNoto Sans\tRegular\tTrueType\n"
        "/usr/share/fonts/truetype/noto/NotoSans[wght].ttf\t65536\tNoto Sans,Noto Sans Thin\tThin,Regular\tTrueType\n"
        "/usr/share/fonts/X11/misc/6x13-ISO8859-1.pcf.gz\t0\tFixed\tSemiCondensed\tPCF\n"
        "/usr/share/fonts/type1/urw-base35/NimbusSans-Regular.t1\t0\tNimbus Sans\tRegular\tType 1\n"
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf\t0\tDejaVu Sans\tBook\tTrueType\n"
        "/usr/share/fonts/truetype/fonts-japanese-gothic.ttf\t0\tTakaoPGothic,Takao P\xE3\x82\xB4\xE3\x82\xB7\xE3\x83\x83\xE3\x82\xAF\tRegular\tTrueType\r\n"
        "/home/user/.local/share/fonts/Comic-Like.otf\t0\tComic\\-Like\tBold Italic,Negreta cursiva\tCFF\n"
        "/usr/share/fonts/broken-line.ttf\t0\tBroken\n";

}

// Red control: keeping every fontformat (or not skipping index >= 65536) lists the PCF, the Type 1
// font or the named instance and fails the count
TEST(SystemFontListTest, ParsesFcList) {
    const std::vector<SystemFontFace> faces = parseFcList(CAPTURED_FC_LIST);
    ASSERT_EQ(faces.size(), 7u);

    // sorted by family, then style
    EXPECT_EQ(faces[0].family, "Comic-Like") << "fontconfig's escape undone";
    EXPECT_EQ(faces[0].style, "Bold Italic") << "the first of the localized styles";
    EXPECT_EQ(faces[0].format, SystemFontFormat::Cff);
    EXPECT_EQ(faces[1].family, "DejaVu Sans");
    EXPECT_EQ(faces[1].style, "Bold");
    EXPECT_EQ(faces[2].style, "Book");
    EXPECT_EQ(faces[2].path, "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf");
    EXPECT_EQ(faces[2].format, SystemFontFormat::TrueType);
    EXPECT_EQ(faces[3].family, "Noto Sans");
    EXPECT_EQ(faces[3].faceIndex, 0) << "the variable font itself, not its Thin instance";
    EXPECT_EQ(faces[4].family, "Noto Sans CJK JP");
    EXPECT_EQ(faces[4].faceIndex, 0);
    EXPECT_EQ(faces[4].format, SystemFontFormat::Cff);
    EXPECT_EQ(faces[5].family, "Noto Sans CJK KR");
    EXPECT_EQ(faces[5].faceIndex, 1) << "the collection's second face";
    EXPECT_EQ(faces[5].path, faces[4].path);
    EXPECT_EQ(faces[6].family, "TakaoPGothic");
    EXPECT_EQ(faces[6].style, "Regular") << "the CRLF line kept (its CR is not part of the format)";

    for (const SystemFontFace& face : faces) {
        EXPECT_NE(face.family, "Fixed");
        EXPECT_NE(face.family, "Nimbus Sans");
        EXPECT_NE(face.family, "Broken");
    }
    EXPECT_TRUE(parseFcList("").empty());
}

// Red control: starting a listing on every request() runs the lister twice
TEST(SystemFontListTest, ListsOnceInTheBackground) {
    SystemFontList list;
    std::atomic<int> runs{0};
    list.setLister([&runs]() {
        runs++;
        return parseFcList(CAPTURED_FC_LIST);
    });
    EXPECT_EQ(list.state(), SystemFontList::State::Idle);
    EXPECT_TRUE(list.faces().empty());
    list.request();
    list.request();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (list.state() != SystemFontList::State::Ready && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    ASSERT_EQ(list.state(), SystemFontList::State::Ready);
    EXPECT_EQ(list.faces().size(), 7u);
    list.request();
    EXPECT_EQ(runs.load(), 1);
    list.reset();
    EXPECT_EQ(list.state(), SystemFontList::State::Idle);
}

// The platform lister on the machine running the tests (CI's macOS and Windows runners have
// fonts; a Linux container without fontconfig has none, which only this platform check allows)
TEST(SystemFontListTest, PlatformListerFindsFonts) {
    const std::vector<SystemFontFace> faces = SystemFontList::listPlatform();
#if defined(__APPLE__) || defined(_WIN32)
    ASSERT_GE(faces.size(), 1u);
#endif
    for (const SystemFontFace& face : faces) {
        EXPECT_FALSE(face.path.empty());
        EXPECT_FALSE(face.family.empty()) << face.path;
        EXPECT_GE(face.faceIndex, 0);
    }
}
