#!/usr/bin/env python3
"""Taffy's browser-generated layout fixtures as engine box-parity fixtures (phase-3 spec 5.3).

    tools/htmlui/import-taffy-fixtures.py import [--taffy DIR]
        Copies the fixture HTML of Taffy's suites grid, flex, block, blockflex, blockgrid,
        gridflex, leaf and float (pinned commit, MIT) into engine/tests/htmlui/fixtures/taffy/<suite>/,
        each with Taffy's <script>/<link> replaced by a link to ../base.css: Taffy's
        test_base_style.css with its embedded Ahem WOFF2 replaced by the local
        engine/tests/htmlui/fonts/Ahem.ttf (the 15px ::-webkit-scrollbar rule stays: Chromium draws
        space-taking scrollbars, the engine's UA sheet gives its scrollbars Chromium's 15px).
        Writes base.css, LICENSE-taffy.txt, README.txt and excluded.txt (fixtures that are not
        compared, with the reason). known-failures.txt is left alone.

    tools/htmlui/import-taffy-fixtures.py known-failures RESULTS [--merge]
        Writes engine/tests/htmlui/fixtures/taffy/known-failures.txt from a BoxParityTest results
        file (<build>/tests/htmlui/htmlui-test-output/taffy/results.txt: "<suite>/<name> pass|fail
        <first difference>" per fixture): one line per failing fixture with the package that owns
        it and a reason class read from the fixture's styles. --merge keeps the owner and reason of
        fixtures already listed. Run it for the baseline only; afterwards each package removes the
        lines of the fixtures it fixed (the test fails while a listed fixture passes).

    tools/htmlui/import-taffy-fixtures.py classify SUITE/NAME
        Prints the owner and reason class the known-failures command would give a fixture.

References: node tools/htmlui/render-reference.mjs --fixtures taffy --boxes-only (README.md).
"""

import argparse
import html
import os
import re
import subprocess
import sys

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
OUT = os.path.join(REPO, 'engine', 'tests', 'htmlui', 'fixtures', 'taffy')
DEFAULT_TAFFY = '/tmp/htmlui-src/taffy'
TAFFY_URL = 'https://github.com/DioxusLabs/taffy'
TAFFY_COMMIT = '8409f3f8c6de1244cb4cf7fc6c485be2481876cd'
SUITES = ['grid', 'flex', 'block', 'blockflex', 'blockgrid', 'gridflex', 'leaf', 'float']

SCRIPT_RE = re.compile(r'[ \t]*<script src="[./]*scripts/gentest/test_helper\.js"></script>[ \t]*\r?\n')
LINK_RE = re.compile(r'<link\s+rel="stylesheet"\s+type="text/css"\s+href="[./]*scripts/gentest/test_base_style\.css"\s*/?>')
LOCAL_LINK = '<link rel="stylesheet" type="text/css" href="../base.css">'
FONT_FACE_SRC_RE = re.compile(r'src:\s*url\(data:application/font-woff2;[^)]*\)\s*format\([\'"]woff2[\'"]\);')
LOCAL_FONT_SRC = 'src: url("../../fonts/Ahem.ttf") format("truetype");'


def fail(message):
    print(f'import-taffy-fixtures: {message}', file=sys.stderr)
    sys.exit(1)


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)


def read(path):
    with open(path, encoding='utf-8') as f:
        return f.read()


# --- exclusions (phase-3 spec 5.3, 2.2) ---------------------------------------------------------

def exclusion(name, source):
    """(reason class, explanation) when the fixture is not compared, else None."""
    if name.startswith('x'):
        return 'taffy-invalid', "Taffy's own known-invalid fixture (name starts with x)"
    if re.search(r'direction\s*:\s*rtl', source):
        return 'rtl-excluded', 'direction: rtl: mirrored layout is not implemented (phase-3 spec 2.2)'
    if re.search(r'writing-mode', source):
        return 'writing-mode-excluded', 'writing-mode: vertical layout is not implemented (phase-3 spec 2.2)'
    if re.search(r'<script>', source):
        return 'needs-script', 'builds its styles with JavaScript, which neither the engine nor the reference runs'
    return None


def base_css(taffy):
    source = read(os.path.join(taffy, 'scripts', 'gentest', 'test_base_style.css'))
    replaced, count = FONT_FACE_SRC_RE.subn(LOCAL_FONT_SRC, source)
    if count != 1:
        fail('test_base_style.css: expected one embedded WOFF2 Ahem source')
    header = (f'/* Taffy {TAFFY_COMMIT[:8]} scripts/gentest/test_base_style.css (MIT, LICENSE-taffy.txt),\n'
              '   written by tools/htmlui/import-taffy-fixtures.py: the embedded Ahem WOFF2 is replaced by\n'
              '   engine/tests/htmlui/fonts/Ahem.ttf (the same font, Kozea/Ahem). The 15px\n'
              '   ::-webkit-scrollbar rule stays: Chromium draws space-taking scrollbars of that width\n'
              '   (the reference tool keeps scrollbars), and the engine\'s user-agent sheet sizes its\n'
              '   scrollbars like Chromium\'s (15px). Do not edit; re-import. */\n\n')
    return header + replaced


def git_commit(directory):
    try:
        return subprocess.run(['git', '-C', directory, 'rev-parse', 'HEAD'], check=True, capture_output=True, text=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return None


def do_import(args):
    taffy = os.path.abspath(args.taffy)
    commit = git_commit(taffy)
    if commit is None:
        print(f'import-taffy-fixtures: {taffy} is not a git checkout; cannot verify the commit', file=sys.stderr)
    elif commit != TAFFY_COMMIT:
        fail(f'{taffy} is at {commit}, the fixtures are pinned to {TAFFY_COMMIT} (git -C {taffy} checkout {TAFFY_COMMIT[:8]})')

    counts = {}
    excluded = []
    for suite in SUITES:
        source_dir = os.path.join(taffy, 'test_fixtures', suite)
        target_dir = os.path.join(OUT, suite)
        if os.path.isdir(target_dir):
            for old in os.listdir(target_dir):
                if old.endswith('.html'):
                    os.remove(os.path.join(target_dir, old))
        names = sorted(f[:-5] for f in os.listdir(source_dir) if f.endswith('.html'))
        for name in names:
            source = read(os.path.join(source_dir, name + '.html'))
            text, scripts = SCRIPT_RE.subn('', source)
            text, links = LINK_RE.subn(LOCAL_LINK, text)
            if scripts != 1 or links != 1:
                fail(f'{suite}/{name}.html: expected one test_helper.js <script> and one test_base_style.css <link>')
            write(os.path.join(target_dir, name + '.html'), text)
            reason = exclusion(name, source)
            if reason:
                excluded.append((f'{suite}/{name}', reason))
        counts[suite] = len(names)

    write(os.path.join(OUT, 'base.css'), base_css(taffy))
    with open(os.path.join(taffy, 'LICENSE'), encoding='utf-8') as f:
        write(os.path.join(OUT, 'LICENSE-taffy.txt'), f'Taffy ({TAFFY_URL}), commit {TAFFY_COMMIT}: the fixtures under\n'
              'engine/tests/htmlui/fixtures/taffy/<suite>/ and base.css are copied from its test_fixtures/ and\n'
              'scripts/gentest/test_base_style.css under this license.\n\n' + f.read())

    lines = ['# Taffy fixtures that are not compared (BoxParityTest skips them and says why), written by',
             '# tools/htmlui/import-taffy-fixtures.py import. One per line: <suite>/<name> reason=<class> # <why>.',
             '# Classes: taffy-invalid, rtl-excluded, writing-mode-excluded (phase-3 spec 2.2), needs-script.', '']
    for fixture, (reason, why) in excluded:
        lines.append(f'{fixture} reason={reason} # {why}')
    write(os.path.join(OUT, 'excluded.txt'), '\n'.join(lines) + '\n')

    total = sum(counts.values())
    readme = [f'Taffy layout fixtures (phase-3 spec 5.3): {TAFFY_URL} commit {TAFFY_COMMIT}, MIT (LICENSE-taffy.txt).',
              'Imported by tools/htmlui/import-taffy-fixtures.py import; do not edit the fixtures, re-import.', '',
              'Suites: ' + ', '.join(f'{suite} {counts[suite]}' for suite in SUITES) + f' ({total} fixtures), '
              f'{len(excluded)} excluded (excluded.txt).', '',
              'Each fixture is Taffy\'s HTML with its test_helper.js <script> removed and its stylesheet link',
              'pointing at base.css (Taffy\'s test_base_style.css on the local Ahem.ttf). The references are',
              'Chromium\'s border boxes of #test-root and every element under it, relative to #test-root, in',
              'golden/browser/taffy/<suite>.boxes.json (node tools/htmlui/render-reference.mjs --fixtures taffy',
              '--boxes-only); BoxParityTest loads each fixture through the compat layer at 1024x768 and compares',
              'the boxes within 1 px. known-failures.txt lists the fixtures the fork does not pass yet (owner and',
              'reason class); excluded.txt the fixtures that are not compared.', '']
    write(os.path.join(OUT, 'README.txt'), '\n'.join(readme))
    print(f'imported {total} fixtures ({", ".join(f"{s} {counts[s]}" for s in SUITES)}), {len(excluded)} excluded')


# --- known failures ----------------------------------------------------------------------------

def styles_of(source):
    """Every declaration of the fixture (inline styles and <style> blocks), lower case."""
    declarations = []
    for match in re.finditer(r'style="([^"]*)"', source):
        declarations += [d.strip().lower() for d in html.unescape(match.group(1)).split(';') if d.strip()]
    for match in re.finditer(r'<style>(.*?)</style>', source, re.S):
        declarations += [d.strip().lower() for d in re.sub(r'[{}]', ';', match.group(1)).split(';') if ':' in d]
    return declarations


def has(declarations, pattern):
    regex = re.compile(pattern)
    return any(regex.search(d) for d in declarations)


def classify(suite, source):
    """(owner package, reason class) for a failing fixture, from the features it uses; the first
    matching rule wins, so a fixture is owned by the package whose feature is its likely cause."""
    d = styles_of(source)
    grid = has(d, r'^display:\s*(inline-)?grid') or suite in ('grid', 'blockgrid', 'gridflex')
    if grid:
        return 'p3-grid', 'grid-unsupported'
    if has(d, r'calc\(|min\(|max\(|clamp\('):
        return 'p3-values', 'calc'
    if has(d, r'^display:\s*contents'):
        return 'p3-layout', 'display-contents'
    if has(d, r'^aspect-ratio'):
        return 'p3-layout', 'aspect-ratio'
    if has(d, r'(min-|max-|fit-)content|stretch'):
        return 'p3-layout', 'intrinsic-sizing'
    if has(d, r'^order\s*:'):
        return 'p3-layout', 'flex-order'
    if suite == 'float' or has(d, r'^float\s*:|^clear\s*:'):
        return 'p3-layout', 'float'
    if has(d, r'^position:\s*absolute'):
        return 'p3-layout', 'absolute-position'
    if has(d, r'^overflow(-x|-y)?\s*:\s*(scroll|auto|hidden|clip)'):
        return 'p3-layout', 'overflow'
    if has(d, r'^flex-basis|^flex\s*:') and has(d, r'^min-(width|height)\s*:\s*auto|^(min|max)-(width|height)'):
        return 'p3-layout', 'flex-min-max'
    if suite in ('flex', 'blockflex') or has(d, r'^display:\s*(inline-)?flex'):
        if has(d, r'^align-content|^align-items|^align-self|^justify-content|^justify-items|^justify-self'):
            return 'p3-layout', 'flex-alignment'
        if has(d, r'^flex-wrap'):
            return 'p3-layout', 'flex-wrap'
        if has(d, r'^(min|max)-(width|height)'):
            return 'p3-layout', 'flex-min-max'
        return 'p3-layout', 'flex-algorithm'
    if suite == 'block' and has(d, r'^margin'):
        return 'p3-layout', 'margin-collapse'
    if suite == 'leaf' or 'ZeroWidthSpace' in source or re.search(r'>[^<>\s]+<', source):
        return 'p3-layout', 'text-measure'
    return 'p3-layout', 'block-layout'


def read_list(path):
    entries = {}
    if not os.path.exists(path):
        return entries
    for line in read(path).splitlines():
        line = line.split('#', 1)[0].strip()
        if not line:
            continue
        words = line.split()
        entries[words[0]] = dict(w.split('=', 1) for w in words[1:] if '=' in w)
    return entries


def do_known_failures(args):
    results = []
    for line in read(args.results).splitlines():
        words = line.split(None, 2)
        if len(words) >= 2:
            results.append((words[0], words[1], words[2] if len(words) > 2 else ''))
    if not results:
        fail(f'{args.results} lists no fixtures')
    path = os.path.join(OUT, 'known-failures.txt')
    previous = read_list(path) if args.merge else {}
    excluded = read_list(os.path.join(OUT, 'excluded.txt'))
    lines = []
    by_owner = {}
    for fixture, status, detail in sorted(results):
        if status != 'fail' or fixture in excluded:
            continue
        suite, name = fixture.split('/', 1)
        if fixture in previous and 'owner' in previous[fixture] and 'reason' in previous[fixture]:
            owner, reason = previous[fixture]['owner'], previous[fixture]['reason']
        else:
            owner, reason = classify(suite, read(os.path.join(OUT, suite, name + '.html')))
        by_owner.setdefault(owner, {}).setdefault(reason, 0)
        by_owner[owner][reason] += 1
        lines.append(f'{fixture} owner={owner} reason={reason}' + (f' # {detail}' if detail else ''))
    summary = []
    for owner in sorted(by_owner):
        reasons = ', '.join(f'{r} {n}' for r, n in sorted(by_owner[owner].items()))
        summary.append(f'#   {owner}: {sum(by_owner[owner].values())} ({reasons})')
    header = ['# Taffy fixtures the fork does not pass yet: BoxParityTest expects them to fail and fails when one',
              '# passes, so the package that fixes a fixture removes its line (phase-3 spec 4, wave 1).',
              '# <suite>/<name> owner=<package> reason=<class> # <first difference of the baseline run>',
              '# Written by tools/htmlui/import-taffy-fixtures.py known-failures from a baseline run; the reason',
              '# class is read from the features the fixture uses (classify), the owner is the package of that',
              f'# feature. {len(lines)} fixtures:'] + summary + ['']
    write(path, '\n'.join(header + lines) + '\n')
    print(f'wrote {os.path.relpath(path, REPO)}: {len(lines)} known failures')
    for line in summary:
        print(line[1:])


def do_classify(args):
    suite, name = args.fixture.split('/', 1)
    owner, reason = classify(suite, read(os.path.join(OUT, suite, name + '.html')))
    print(f'{args.fixture} owner={owner} reason={reason}')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest='command', required=True)
    p = commands.add_parser('import', help='copy the fixtures, write base.css, excluded.txt, README.txt')
    p.add_argument('--taffy', default=DEFAULT_TAFFY, help=f'Taffy checkout at {TAFFY_COMMIT[:8]} (default {DEFAULT_TAFFY})')
    p.set_defaults(run=do_import)
    p = commands.add_parser('known-failures', help='write known-failures.txt from a BoxParityTest results file')
    p.add_argument('results')
    p.add_argument('--merge', action='store_true', help='keep the owner and reason of listed fixtures')
    p.set_defaults(run=do_known_failures)
    p = commands.add_parser('classify', help='owner and reason class of one fixture')
    p.add_argument('fixture', help='<suite>/<name>')
    p.set_defaults(run=do_classify)
    args = parser.parse_args()
    args.run(args)


if __name__ == '__main__':
    main()
