Taffy layout fixtures (phase-3 spec 5.3): https://github.com/DioxusLabs/taffy commit 8409f3f8c6de1244cb4cf7fc6c485be2481876cd, MIT (LICENSE-taffy.txt).
Imported by tools/htmlui/import-taffy-fixtures.py import; do not edit the fixtures, re-import.

Suites: grid 565, flex 686, block 264, blockflex 12, blockgrid 14, gridflex 7, leaf 15, float 32 (1595 fixtures), 75 excluded (excluded.txt).

Each fixture is Taffy's HTML with its test_helper.js <script> removed and its stylesheet link
pointing at base.css (Taffy's test_base_style.css on the local Ahem.ttf). The references are
Chromium's border boxes of #test-root and every element under it, relative to #test-root, in
golden/browser/taffy/<suite>.boxes.json (node tools/htmlui/render-reference.mjs --fixtures taffy
--boxes-only); BoxParityTest loads each fixture through the compat layer at 1024x768 and compares
the boxes within 1 px. known-failures.txt lists the fixtures the fork does not pass yet (owner and
reason class); excluded.txt the fixtures that are not compared.
