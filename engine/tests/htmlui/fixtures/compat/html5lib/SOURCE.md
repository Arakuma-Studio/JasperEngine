# html5lib tree-construction tests

The `tree-construction/*.dat` files of [html5lib-tests](https://github.com/html5lib/html5lib-tests)
(MIT, `LICENSE`, `AUTHORS.rst`) at commit `9329e64694e7835d0dcff9811e22856ef6ad16f9`
(2026-06-22), the last one before the tree-construction tests moved to web-platform-tests
(`224991e`). The `scripted/` tests are not included.

Used by `engine/tests/htmlui/compat/compat_html5lib_test.cpp`: every `#data` input is parsed
by the compat layer's HtmlParser (patched lexbor, `engine/libs/lexbor/PATCHES.md`); every
element created from a start tag must point at `<` + its name, implied elements must have no
offset, attributes must point at their names.
