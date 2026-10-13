Fonts of the browser-parity fixtures (phase-3 spec 5.2, 5.3). Every fixture under
fixtures/p3/ and fixtures/taffy/ loads its fonts from here with @font-face, so Chromium (the
references, tools/htmlui/render-reference.mjs) and the engine draw the same faces; fixtures
avoid generic families.

Roboto-Regular-subset-post.ttf
    The engine's built-in face (the bytes of roboto_regular_ttf in engine/core/util/DefaultFont.h,
    Roboto, Copyright 2011 The Roboto Project Authors; license as stated in DefaultFont.h) with a
    'post' table (format 3, Roboto's underline position -150 and thickness 100 of 2048 units)
    added: Chromium's font sanitizer (OTS) rejects a TrueType font without one ("OTS parsing
    error: post: missing required table") and draws another face. Every other table is the same
    bytes (BrowserFixtures.FontsAreTheEnginesFaces compares them with DefaultFont.h). The font of
    the fixtures that draw Roboto text. (The copy without 'post' was removed in p3-integration;
    BrowserFixtures.FontFacesLoadInChromium checks that every local font a Chromium-rendered
    fixture loads exists and has the tables the sanitizer requires.)

Ahem.ttf, Ahem-COPYING.txt
    Ahem, https://github.com/Kozea/Ahem tag 1.0 (commit 7f4a69f546fd7bb7ba7031536d8470fbe87f4e26),
    public domain / CC0 (Ahem-COPYING.txt). Em square 1000 units, ascent 0.8 em, descent 0.2 em,
    most glyphs a full 1 em square: the font of Taffy's layout fixtures (its test_base_style.css
    embeds a FontForge re-save of the same font as WOFF2, with the same metrics and advances).
