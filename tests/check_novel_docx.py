#!/usr/bin/env python3
# This file is part of FDTP (File & Directory Tool for Pomera).
# SPDX-FileCopyrightText: 2026 yasoteien
# SPDX-License-Identifier: AGPL-3.0-or-later
"""The Word export with the built-in template (docs/FDTP_REVISION1.md 4.3):
check_novel_docx.py --build DIR [--no-sanitize]

Needs none of the author's files (tests/check_novel_format.py covers a user
template). Builds like check_novel_format.py and checks:
  * novel-format-tool docx RUBY IN - OUT (no template) writes a zip that
    Python's zipfile reads and tests, every part well-formed XML, the
    package parts Word needs, sectPr with w:textDirection tbRl, A4
    portrait, the Mincho body font, one paragraph per line, and w:ruby
    (rubyBase / rt) and w:em with RUBY 1, none with RUBY 0;
  * the editor (F2, Word 縦書きで書き出す) with no template anywhere and
    with $FDTP_WORD_TEMPLATE naming a missing file: written, and the notice
    says 既定の雛形.
Copies the ruby sample to DIR/novel-default-sample.docx for looking at in Word.
"""
import argparse
from pathlib import Path
import shutil
import sys
import xml.etree.ElementTree as ET
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_novel as cn  # noqa: E402
import check_novel_format as cf  # noqa: E402

require, Failure = cn.require, cn.Failure
W = cf.W
PARTS = ['[Content_Types].xml', '_rels/.rels', 'word/document.xml', 'word/_rels/document.xml.rels',
         'word/styles.xml', 'word/settings.xml']
SAMPLE = ('【タイトル】\n第1話　題\n\n【本文（4行）】\n　｜漢字《かんじ》と草薙《くさなぎ》と漢字(かな)。\n\n'
          '　《《傍点》》と<&>"\'\tタブ\n「台詞！」\n')


def check_package(path, ruby):
    require(zipfile.is_zipfile(path), f'{path.name}: a zip')
    with zipfile.ZipFile(path) as z:
        require(z.testzip() is None, f'{path.name}: CRCs')
        names = z.namelist()
        require(names == PARTS, f'{path.name}: parts {names}')
        trees = {n: ET.fromstring(z.read(n)) for n in names}  # every part well-formed
    types = {e.get('PartName') for e in trees['[Content_Types].xml']}
    require({'/word/document.xml', '/word/styles.xml', '/word/settings.xml'} <= types, f'content types {types}')
    body = trees['word/document.xml'].find(W + 'body')
    sect = body[-1]
    require(sect.tag == W + 'sectPr', 'sectPr last')
    require(sect.find(W + 'textDirection').get(W + 'val') == 'tbRl', 'vertical: textDirection tbRl')
    size = sect.find(W + 'pgSz')
    require((size.get(W + 'w'), size.get(W + 'h'), size.get(W + 'orient')) == ('11906', '16838', None),
            f'A4 portrait {size.attrib}')
    fonts = trees['word/styles.xml'].find(f'{W}docDefaults/{W}rPrDefault/{W}rPr/{W}rFonts')
    require(fonts.get(W + 'eastAsia') == '游明朝' and fonts.get(W + 'ascii') == '游明朝', f'body font {fonts.attrib}')
    paras = body.findall(W + 'p')
    want = ['第1話　題'] + SAMPLE.split('\n')[4:-1]
    require(len(paras) == len(want), f'{len(paras)} paragraphs for {len(want)} lines')
    style = paras[0].find(f'{W}pPr/{W}pStyle')
    require(style is not None and style.get(W + 'val') == 'FdtpTitle', 'title paragraph uses FdtpTitle')
    sizes = {st.get(W + 'styleId'): st.find(f'{W}rPr/{W}sz').get(W + 'val')
             for st in trees['word/styles.xml'].findall(W + 'style') if st.find(f'{W}rPr/{W}sz') is not None}
    require(sizes.get('FdtpTitle') == '24' and sizes.get('Normal') == '21', f'title 12 pt, body 10.5 pt: {sizes}')
    rubies = body.findall('.//' + W + 'ruby')
    if not ruby:
        require(not rubies and cf.paragraph_text(paras[1]) == want[1], 'no ruby: the notation as text')
        return paras
    pairs = [(''.join(t.text for t in r.find(W + 'rubyBase').iter(W + 't')),
              ''.join(t.text for t in r.find(W + 'rt').iter(W + 't'))) for r in rubies]
    require(pairs == [('漢字', 'かんじ'), ('草薙', 'くさなぎ'), ('漢字', 'かな')], f'ruby pairs {pairs}')
    vals = {c.tag[len(W):]: c.get(W + 'val') for c in rubies[0].find(W + 'rubyPr')}
    require(vals == {'rubyAlign': 'distributeSpace', 'hps': '10', 'hpsRaise': '18', 'hpsBaseText': '21', 'lid': 'ja-JP'},
            f'rubyPr {vals}')
    em = paras[3].find(f'.//{W}em')
    require(em is not None and em.get(W + 'val') == 'dot', 'emphasis as w:em dot')
    require(cf.paragraph_text(paras[3]) == '　傍点と<&>"\'\tタブ', f'special characters {cf.paragraph_text(paras[3])!r}')
    return paras


def check_editor(runner, work):
    d = work / 'editor'
    d.mkdir(parents=True, exist_ok=True)
    runner.env.pop('FDTP_WORD_TEMPLATE', None)
    rc = runner.home / '.fdtprc'
    rc.parent.mkdir(parents=True, exist_ok=True)
    rc.write_text('set NOVEL_WORD_RUBY=1\n', encoding='utf-8')
    require(not (runner.home / '.fdtp' / 'template').exists(), 'no template in HOME')
    novel = d / 'novel.txt'
    novel.write_text('　｜漢字《かんじ》です。\n', encoding='utf-8')
    dest = d / 'novel.docx'
    for name, missing in (('none', None), ('env', d / 'missing.docx')):
        if missing:
            runner.env['FDTP_WORD_TEMPLATE'] = str(missing)
        if dest.exists():
            dest.unlink()
        s = runner.run(novel, cf.keys(d / f'{name}.keys', 'F2 UP RET SNAP "y" SNAP ESC\n'), f'word-{name}')
        require(s[0]['widget'] == cf.WIDGET_CONFIRM, f'{name}: y/n, not an error ({s[0]["widget"]}, {s[0].get("notice")!r})')
        require(s[1]['notice'] == 'novel.docx に既定の雛形で書き出しました（ルビ: Word のルビ）', f'{name}: notice {s[1]["notice"]!r}')
        with zipfile.ZipFile(dest) as z:
            require(z.testzip() is None and b'<w:ruby>' in z.read('word/document.xml')
                    and b'tbRl' in z.read('word/document.xml'), f'{name}: written')
    runner.env.pop('FDTP_WORD_TEMPLATE', None)
    rc.write_text('', encoding='utf-8')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--no-sanitize', action='store_true')
    args = parser.parse_args()
    directory = args.build.resolve()
    variants = [('plain', directory, False)]
    if not args.no_sanitize:
        variants.append(('asan', directory / 'novel-asan', True))
    for label, build_dir, sanitize in variants:
        harness, tool_path = cf.build(build_dir, sanitize)
        work = directory / 'novel-docx-check' / label
        if work.exists():
            shutil.rmtree(work)
        (work / 'tmp').mkdir(parents=True)
        tool = cf.Tool(tool_path, work)
        sample = work / 'sample.txt'
        sample.write_text(SAMPLE, encoding='utf-8')
        for ruby in (0, 1):
            out = work / f'sample-{ruby}.docx'
            tool.run('docx', ruby, sample, '-', out)
            check_package(out, ruby)
        runner = cn.Runner(work, harness, harness, label)
        check_editor(runner, work)
        if label == 'plain':
            shutil.copyfile(work / 'sample-1.docx', directory / 'novel-default-sample.docx')
        print(f'{label}: built-in template ok (tool 2, editor {runner.runs})')
    print(f'sample: {directory / "novel-default-sample.docx"}')


if __name__ == '__main__':
    try:
        main()
    except (Failure, OSError) as error:
        print(f'FAIL: {error}', file=sys.stderr)
        sys.exit(1)
