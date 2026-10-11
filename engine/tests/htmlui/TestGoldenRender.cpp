// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Full-path goldens of boxes, layout, images, blending, clipping and transforms: the
// documents of a scene go through UIDocumentSystem, the context's offscreen target and the
// composite in the scene's camera pass (golden/orientation.png, boxes_borders, flexbox,
// images, alpha_blend, overflow_scissor, transform).

#include "TestGoldenCommon.h"

#include "HtmlUIContext.h"
#include "subsystem/UIDocumentSystem.h"
#include "object/ui/Image.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>

using namespace jasperengine;
using namespace jasperengine::test;

TEST_F(FullPathGoldenTest, Orientation){
    SceneFixture fixture;
    fixture.addDocument("golden/orientation.rml");
    ASSERT_TRUE(fixture.waitUntilLoaded());

    Screen screen;
    ASSERT_TRUE(capture(screen));

    // red (10,10)-(110,60) top-left, green (530,10)-(630,60) top-right, blue (10,300)-(110,350)
    EXPECT_TRUE(screen.is(11, 11, 255, 0, 0));
    EXPECT_TRUE(screen.is(108, 58, 255, 0, 0));
    EXPECT_TRUE(screen.isBackground(9, 9));
    EXPECT_TRUE(screen.isBackground(111, 61));
    EXPECT_TRUE(screen.is(531, 11, 0, 255, 0));
    EXPECT_TRUE(screen.is(628, 58, 0, 255, 0));
    EXPECT_TRUE(screen.isBackground(631, 30));
    EXPECT_TRUE(screen.is(11, 301, 0, 0, 255));
    EXPECT_TRUE(screen.is(108, 348, 0, 0, 255));
    EXPECT_TRUE(screen.isBackground(60, 352));
    // the mirrored places stay empty
    EXPECT_TRUE(screen.isBackground(580, 325));
    EXPECT_TRUE(screen.isBackground(60, 200));

    EXPECT_SCREEN_GOLDEN(screen, "orientation");
}

TEST_F(FullPathGoldenTest, BoxesBorders){
    SceneFixture fixture;
    fixture.addDocument("render/boxes.rml");
    ASSERT_TRUE(fixture.waitUntilLoaded());

    Screen screen;
    ASSERT_TRUE(capture(screen));

    // #solid: 4 px white border around #3060c0, border box (20,20)-(148,108)
    EXPECT_TRUE(screen.is(80, 60, 0x30, 0x60, 0xc0));
    EXPECT_TRUE(screen.is(21, 60, 255, 255, 255));
    EXPECT_TRUE(screen.is(146, 60, 255, 255, 255));
    EXPECT_TRUE(screen.isBackground(150, 60));
    EXPECT_TRUE(screen.isBackground(80, 18));
    // #sides: per-side widths and colors, border box (180,20)-(320,112)
    EXPECT_TRUE(screen.is(250, 20, 255, 0, 0));
    EXPECT_TRUE(screen.is(317, 66, 0, 255, 0));
    EXPECT_TRUE(screen.is(250, 107, 0, 0, 255));
    EXPECT_TRUE(screen.is(186, 66, 255, 255, 0));
    EXPECT_TRUE(screen.is(250, 66, 0x40, 0x40, 0x40));
    // radius 0 keeps its corner, 8 and 24 cut theirs
    EXPECT_TRUE(screen.is(341, 21, 0xc0, 0x30, 0x30));
    EXPECT_TRUE(screen.isBackground(441, 21));
    EXPECT_TRUE(screen.isBackground(541, 21));
    EXPECT_TRUE(screen.is(480, 60, 0x30, 0xc0, 0x30));
    EXPECT_TRUE(screen.is(583, 63, 0x30, 0x30, 0xc0));
    // nested: outer padding, middle margin and padding, inner rounded
    EXPECT_TRUE(screen.is(30, 160, 0x60, 0x60, 0x60));
    EXPECT_TRUE(screen.is(50, 180, 0xa0, 0x60, 0x20));
    EXPECT_TRUE(screen.is(110, 210, 0x20, 0xa0, 0xa0));
    // the ring is hollow, the pill rounded at both ends
    EXPECT_TRUE(screen.isBackground(410, 220));
    EXPECT_TRUE(screen.is(345, 220, 0xff, 0xc0, 0x00));
    EXPECT_TRUE(screen.is(550, 190, 0xe0, 0x40, 0xe0));
    EXPECT_TRUE(screen.isBackground(481, 171));

    EXPECT_SCREEN_GOLDEN(screen, "boxes_borders");
}

TEST_F(FullPathGoldenTest, Flexbox){
    SceneFixture fixture;
    fixture.addDocument("golden/flexbox.rml");
    ASSERT_TRUE(fixture.waitUntilLoaded());

    Screen screen;
    ASSERT_TRUE(capture(screen));

    // flex-start: 40 px items, 10 px gaps from x = 10
    EXPECT_TRUE(screen.is(30, 25, 0xe0, 0x40, 0x40));
    EXPECT_TRUE(screen.is(80, 25, 0x40, 0xe0, 0x40));
    EXPECT_TRUE(screen.is(130, 25, 0x40, 0x40, 0xe0));
    EXPECT_TRUE(screen.is(200, 25, 0x30, 0x30, 0x30));
    // center: 140 px of items in 300 starts at x = 90
    EXPECT_TRUE(screen.is(50, 75, 0x30, 0x30, 0x30));
    EXPECT_TRUE(screen.is(110, 75, 0xe0, 0x40, 0x40));
    EXPECT_TRUE(screen.is(210, 75, 0x40, 0x40, 0xe0));
    // space-between: the last item ends at the row's end
    EXPECT_TRUE(screen.is(160, 125, 0x40, 0xe0, 0x40));
    EXPECT_TRUE(screen.is(305, 125, 0x40, 0x40, 0xe0));
    // flex-grow 1:2:1: widths 72.5, 135, 72.5
    EXPECT_TRUE(screen.is(15, 175, 0xe0, 0x40, 0x40));
    EXPECT_TRUE(screen.is(160, 175, 0x40, 0xe0, 0x40));
    EXPECT_TRUE(screen.is(305, 175, 0x40, 0x40, 0xe0));
    // align-items start / center / end of a 30 px item in an 80 px row
    EXPECT_TRUE(screen.is(20, 215, 0xe0, 0x40, 0x40));
    EXPECT_TRUE(screen.is(20, 285, 0x30, 0x30, 0x30));
    EXPECT_TRUE(screen.is(125, 250, 0xe0, 0x40, 0x40));
    EXPECT_TRUE(screen.is(125, 215, 0x30, 0x30, 0x30));
    EXPECT_TRUE(screen.is(230, 285, 0xe0, 0x40, 0x40));
    EXPECT_TRUE(screen.is(230, 215, 0x30, 0x30, 0x30));
    // wrap: six items on the first line, the seventh starts the second
    EXPECT_TRUE(screen.is(280, 310, 0x40, 0x40, 0xe0));
    EXPECT_TRUE(screen.is(30, 340, 0xe0, 0x40, 0x40));
    EXPECT_TRUE(screen.is(80, 340, 0x40, 0xe0, 0x40));
    EXPECT_TRUE(screen.is(130, 340, 0x30, 0x30, 0x30));
    EXPECT_TRUE(screen.is(30, 325, 0x30, 0x30, 0x30));
    // the card (200x130 + 2 px border) centered in the 300x230 stage at (330,10):
    // border box (378,58)-(582,192)
    EXPECT_TRUE(screen.is(370, 125, 0x28, 0x38, 0x48));
    EXPECT_TRUE(screen.is(590, 125, 0x28, 0x38, 0x48));
    EXPECT_TRUE(screen.is(480, 50, 0x28, 0x38, 0x48));
    EXPECT_TRUE(screen.is(480, 200, 0x28, 0x38, 0x48));
    EXPECT_TRUE(screen.is(480, 74, 0x30, 0x60, 0xc0));     // header (60..90)
    EXPECT_TRUE(screen.is(400, 125, 0xd0, 0xa0, 0x30));    // first body cell
    EXPECT_TRUE(screen.is(560, 125, 0xd0, 0xa0, 0x30));    // last body cell
    EXPECT_TRUE(screen.is(480, 125, 0xd0, 0xa0, 0x30));    // middle cell
    EXPECT_TRUE(screen.is(416, 175, 0x40, 0xa0, 0x40));    // footer buttons at both ends
    EXPECT_TRUE(screen.is(544, 175, 0x40, 0xa0, 0x40));
    EXPECT_TRUE(screen.is(480, 175, 0xf0, 0xf0, 0xf0));
    // column with space-between, centered
    EXPECT_TRUE(screen.is(480, 258, 0xe0, 0x40, 0x40));
    EXPECT_TRUE(screen.is(480, 300, 0x40, 0xe0, 0x40));
    EXPECT_TRUE(screen.is(480, 342, 0x40, 0x40, 0xe0));
    EXPECT_TRUE(screen.is(440, 258, 0x30, 0x30, 0x30));

    EXPECT_SCREEN_GOLDEN(screen, "flexbox");
}

TEST_F(FullPathGoldenTest, Images){
    SceneFixture fixture;
    fixture.addDocument("render/images.rml");
    ASSERT_TRUE(fixture.waitUntilLoaded());

    Screen screen;
    ASSERT_TRUE(capture(screen));

    // checker.png at 1x: 8 px cells, the top-left one red (row 0 of the file is the top)
    EXPECT_TRUE(screen.is(24, 24, 255, 0, 0));
    EXPECT_TRUE(screen.is(32, 24, 0x40, 0x40, 0x40));
    EXPECT_TRUE(screen.is(40, 24, 255, 255, 255));
    EXPECT_TRUE(screen.is(24, 32, 0x40, 0x40, 0x40));
    // at 2.5x: 20 px cells
    EXPECT_TRUE(screen.is(118, 28, 255, 0, 0));
    EXPECT_TRUE(screen.is(140, 28, 0x40, 0x40, 0x40));
    EXPECT_TRUE(screen.is(160, 28, 255, 255, 255));
    EXPECT_TRUE(screen.is(118, 48, 0x40, 0x40, 0x40));
    // alpha50 (255, 0, 0, 128) over white and over the background
    EXPECT_TRUE(screen.is(330, 50, 255, 127, 127));
    EXPECT_TRUE(screen.is(305, 25, 255, 255, 255));
    EXPECT_TRUE(screen.is(316, 136, 143, 15, 15));
    // the image decorator
    EXPECT_TRUE(screen.is(404, 24, 255, 0, 0));
    EXPECT_TRUE(screen.is(412, 24, 0x40, 0x40, 0x40));
    // icon.svg: white circle on the blue rounded square, at 1x and 3x
    EXPECT_TRUE(screen.is(512, 32, 255, 255, 255, 3));
    EXPECT_TRUE(screen.is(504, 32, 0x20, 0x60, 0xc0, 3));
    EXPECT_TRUE(screen.is(576, 56, 255, 255, 255, 3));
    EXPECT_TRUE(screen.is(552, 56, 0x20, 0x60, 0xc0, 6));

    EXPECT_SCREEN_GOLDEN(screen, "images");
}

TEST_F(FullPathGoldenTest, AlphaBlend){
    SceneFixture fixture;

    // a component-UI image under the document: the premultiplied composite blends with
    // what the camera pass drew before
    Image& under = fixture.add<Image>();
    under.setPosition(20, 110);
    under.setSize(600, 40);
    under.setColor(1.0f, 1.0f, 0.0f, 1.0f);

    fixture.addDocument("render/alpha.rml");
    ASSERT_TRUE(fixture.waitUntilLoaded());

    Screen screen;
    ASSERT_TRUE(capture(screen));

    // over the background (as render_alpha)
    EXPECT_TRUE(screen.is(60, 60, 143, 15, 15));
    EXPECT_TRUE(screen.is(260, 200, 15, 15, 143));
    EXPECT_TRUE(screen.is(350, 50, 143, 143, 143));
    EXPECT_TRUE(screen.is(300, 290, 87, 87, 87, 3));
    // over the yellow image: only the image, then #FF000080 over it: 128 + 255 * 127 / 255
    EXPECT_TRUE(screen.is(30, 130, 255, 255, 0));
    EXPECT_TRUE(screen.is(60, 130, 255, 127, 0, 3));
    // red and blue (premultiplied (64, 0, 128, 192) in the layer) over yellow
    EXPECT_TRUE(screen.is(150, 130, 127, 63, 128, 3));
    EXPECT_TRUE(screen.is(300, 130, 255, 255, 0));
    // half-opacity white group, and its green child, over yellow: the group is composited once (CSS; phase 3,
    // p3-paint), the opaque green child covers the white in it
    EXPECT_TRUE(screen.is(350, 130, 255, 255, 128, 3));
    EXPECT_TRUE(screen.is(400, 130, 128, 255, 0, 3));

    EXPECT_SCREEN_GOLDEN(screen, "alpha_blend");
}

TEST_F(FullPathGoldenTest, OverflowScissor){
    SceneFixture fixture;
    UIDocument document = fixture.addDocument("golden/overflow_scissor.rml");
    ASSERT_TRUE(fixture.waitUntilLoaded());

    // scroll the list by two rows from test code (wheel input is phase 1)
    HtmlUIContext* context = fixture.getScene().getSystem<UIDocumentSystem>()->getContext();
    ASSERT_NE(context, nullptr);
    Rml::ElementDocument* rmlDocument = context->getRmlContext()->GetDocument(0);
    ASSERT_NE(rmlDocument, nullptr);
    Rml::Element* list = rmlDocument->GetElementById("list");
    ASSERT_NE(list, nullptr);
    list->SetScrollTop(48.0f);
    EXPECT_FLOAT_EQ(list->GetScrollTop(), 48.0f);

    Screen screen;
    ASSERT_TRUE(capture(screen, 2));

    // #clip (20,20)-(220,120): #big starts 50 px left of it and 30 px down, clipped to it
    EXPECT_TRUE(screen.is(22, 60, 0xe0, 0x80, 0x20));
    EXPECT_TRUE(screen.is(120, 30, 0x30, 0x30, 0x60));
    EXPECT_TRUE(screen.is(218, 118, 0xe0, 0x80, 0x20));
    EXPECT_TRUE(screen.isBackground(18, 60));
    EXPECT_TRUE(screen.isBackground(222, 100));
    EXPECT_TRUE(screen.isBackground(120, 122));
    // nested clips: #innerbig is inside #innerclip and #outer, (360,120)-(420,180)
    EXPECT_TRUE(screen.is(375, 135, 0x20, 0xc0, 0xc0));
    EXPECT_TRUE(screen.is(418, 178, 0x20, 0xc0, 0xc0));
    EXPECT_TRUE(screen.is(345, 110, 0x60, 0x30, 0x30));
    EXPECT_TRUE(screen.isBackground(422, 150));
    EXPECT_TRUE(screen.isBackground(380, 182));
    // the list scrolled by two rows: Item 3 (even) at the top, Item 7 (even) at the bottom
    EXPECT_TRUE(screen.is(600, 22, 0x40, 0x40, 0x80));
    EXPECT_TRUE(screen.is(600, 50, 0x80, 0x40, 0x40));
    EXPECT_TRUE(screen.is(600, 138, 0x40, 0x40, 0x80));
    EXPECT_TRUE(screen.isBackground(600, 142));
    // each visible row has its label, clipped rows are not drawn
    for (int row = 0; row < 5; row++){
        const int top = 20 + row * 24;
        const int r = (row % 2 == 0) ? 0x40 : 0x80;
        const int b = (row % 2 == 0) ? 0x80 : 0x40;
        InkBox label = screen.ink({466, top + 2, 560, top + 22}, r, 0x40, b);
        EXPECT_GT(label.count, 30u) << "row " << row;
    }
    EXPECT_TRUE(screen.inkOnBackground({455, 141, 640, 200}).empty());
    // clipped text: nothing beyond the box's right edge (170) or below it (260)
    InkBox text = screen.ink({20, 220, 170, 260}, 0x30, 0x50, 0x50);
    EXPECT_GT(text.count, 200u);
    EXPECT_GE(text.x1, 165);
    EXPECT_TRUE(screen.inkOnBackground({171, 215, 400, 265}).empty());
    // the unclipped box
    EXPECT_TRUE(screen.is(320, 320, 0x80, 0x80, 0x80));

    EXPECT_SCREEN_GOLDEN(screen, "overflow_scissor");
}

TEST_F(FullPathGoldenTest, Transform){
    SceneFixture fixture;
    fixture.addDocument("golden/transform.rml");
    ASSERT_TRUE(fixture.waitUntilLoaded());

    Screen screen;
    ASSERT_TRUE(capture(screen));

    // the reference box with its centered label
    EXPECT_TRUE(screen.is(25, 45, 0x60, 0x60, 0x60));
    InkBox label = screen.ink({20, 40, 120, 100}, 0x60, 0x60, 0x60);
    EXPECT_GT(label.count, 50u);
    EXPECT_NEAR(label.centerX(), 70.0f, 3.0f);
    // rotate(15deg) about the center (210,70): the corners move, the center stays
    EXPECT_TRUE(screen.is(210, 55, 0xe0, 0x40, 0x40, 40));
    EXPECT_TRUE(screen.isBackground(161, 41));
    EXPECT_TRUE(screen.is(255, 60, 0xe0, 0x40, 0x40));   // near the rotated top-right corner (266,54)
    EXPECT_TRUE(screen.isBackground(262, 45));
    // scale(1.5) about the center (370,70): 150x90 from (295,25)
    EXPECT_TRUE(screen.is(298, 28, 0x40, 0xa0, 0x40));
    EXPECT_TRUE(screen.is(442, 112, 0x40, 0xa0, 0x40));
    EXPECT_TRUE(screen.isBackground(292, 70));
    // translate(30, 20): (50,220)-(150,280)
    EXPECT_TRUE(screen.is(52, 222, 0x40, 0x40, 0xe0));
    EXPECT_TRUE(screen.is(148, 278, 0x40, 0x40, 0xe0));
    EXPECT_TRUE(screen.isBackground(25, 205));
    // perspective: the yellow card is narrower than 100 px and keeps a dark label
    InkBox persp = screen.ink({200, 180, 400, 330}, 0x30, 0x30, 0x30);
    EXPECT_GT(persp.count, 2000u);
    EXPECT_LT(persp.width(), 100);
    EXPECT_GT(screen.countColor({250, 220, 350, 290}, 0, 0, 0, 60), 20u);
    // the label of the rotated box is drawn rotated: white ink inside the red box only
    EXPECT_GT(screen.countColor({160, 40, 260, 100}, 255, 255, 255, 60), 20u);

    EXPECT_SCREEN_GOLDEN(screen, "transform");
}
