# lexbor in JasperEngine

- Upstream: https://github.com/lexbor/lexbor (Apache-2.0, `LICENSE` and `NOTICE` here).
- Version: **3.1.0**, which upstream has not tagged yet: the sources are `master` at commit
  `f4cbbcd91359a0ec9499e3ce7e263de629482d61` (2026-09-28, "Core: fixed allocation and move
  sizes in lexbor_array_obj_t"), whose `version` file reads `LEXBOR_VERSION=3.1.0` (the bump
  commit `6205bbe`). The phase-2 experiments (`docs/html-ui/phase-2-compat.md` §3.1) ran on
  the same commit.
- Vendored subset: `source/lexbor/{core,dom,html,ns,tag}` and
  `source/lexbor/ports/{posix,windows_nt}/lexbor/core` under `lexbor/` (the include root is
  this directory: `#include <lexbor/html/html.h>`), without upstream's `config.cmake` /
  `port.conf` files. `css`, `selectors`, `style`, `encoding`, `url`, `unicode`, `punycode`
  and the other modules are not needed by the HTML parser and are not here.
- Build: `CMakeLists.txt` (ours) globs the `.c` files of the subset and the port of the
  platform (`windows_nt` on Windows, `posix` elsewhere), C99, static, `LEXBOR_STATIC`
  public, hidden visibility (the shared editor engine must not export `lxb_*`/`lexbor_*`;
  `engine/CMakeLists.txt` also passes `--exclude-libs,liblexbor.a`), warnings off like every
  vendored library.
- Users: only `engine/core/htmlui/compat/html/HtmlParser.cpp` includes lexbor; it converts
  the DOM into the compat layer's own `HtmlTree` at once (no lexbor type leaves that file).

## Patches

Every change to upstream files is listed here, in the commit `lexbor: record source offsets`.

### 1. Source offsets of nodes (`lexbor: record source offsets`)

The HTML UI reports every diagnostic at `file:line:column` of the user's HTML
(phase-2 §3.7.1). lexbor's DOM keeps no source position, but its tokens point into the
buffer handed to `lxb_html_document_parse` when the whole document is parsed in one call.

- `lexbor/dom/interfaces/node.h`: defines the existing hook `LXB_DOM_NODE_USER_VARIABLES`
  (unless the build defines it) as two fields of `lxb_dom_node_t`,
  `const lxb_char_t *jasper_src_begin` and `jasper_src_end`. Nodes are allocated zeroed,
  so they stay NULL unless set below. (Defined in the header rather than on the compiler
  command line: the value contains `;`, which CMake lists cannot carry, and every build
  system — CMake, the Xcode workspace — then sees the same struct layout.)
- `lexbor/html/tree.c`, `lxb_html_tree_create_element_for_token`: when the token is the
  tokenizer's own current token (`token == tree->tkz_ref->token`) and not an adoption-agency
  re-creation (`token->base_element == NULL`), stores `token->begin` (the byte after `<`)
  and `token->end` (the end of the tag name). Elements the tree builder creates from fake
  tokens (implied `html`, `head`, `body`, `tbody`, `tr`, `colgroup`, `p`, ...) and
  reconstructed formatting elements keep NULL: the compat layer marks them implied.
- `lexbor/html/tree.c`, `lxb_html_tree_append_attributes`: for attributes of that token,
  stores the attribute name's start and the end of its value (before a closing quote), or
  of its name for an attribute without a value, on the `lxb_dom_attr_t` node.
- `lexbor/html/tree.c`, `lxb_html_tree_insert_character_for_data`: a new text node gets the
  range of the text token being processed (the token its first characters came from).
- `lexbor/html/tree.c`, `lxb_html_tree_insert_comment`: the comment token's range (the
  comment text).

Verified against the html5lib tree-construction corpus by
`engine/tests/htmlui/compat/compat_html5lib_test.cpp`: every element created from a start
tag points at `<` + its tag name, implied elements have no offset, attributes point at their
names.

To update lexbor: copy the subset of the new version, re-apply this patch (four hunks in
`tree.c`, one in `node.h`), run `compat_html5lib_test` and the `compat_html` goldens.
