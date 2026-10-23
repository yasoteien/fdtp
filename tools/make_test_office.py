#!/usr/bin/env python3
# This file is part of FDTP (File & Directory Tool for Pomera).
# SPDX-FileCopyrightText: 2026 yasoteien
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Test fixtures for the Office viewers, built with the standard library.

OOXML packages are assembled part by part with zipfile: package() writes the
parts in order, rels() and content_types() make the relationship and type
parts, so docx/pptx fixtures (stages 5 and 6) can reuse them. Only the xlsx
and csv fixtures for stage 4 are made here.

    python3 tools/make_test_office.py OUTDIR
"""
import sys
import zipfile
from pathlib import Path

NS_MAIN = 'http://schemas.openxmlformats.org/spreadsheetml/2006/main'
NS_REL = 'http://schemas.openxmlformats.org/officeDocument/2006/relationships'
NS_PKG_REL = 'http://schemas.openxmlformats.org/package/2006/relationships'
REL = 'http://schemas.openxmlformats.org/officeDocument/2006/relationships/'
XML_DECL = '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\r\n'


def package(path, parts, stored=()):
    """parts: [(name, bytes|str)] in archive order; names in stored use STORE."""
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(path, 'w') as z:
        for name, data in parts:
            if isinstance(data, str):
                data = data.encode('utf-8')
            info = zipfile.ZipInfo(name, date_time=(2024, 1, 2, 3, 4, 6))
            info.compress_type = zipfile.ZIP_STORED if name in stored else zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            z.writestr(info, data)
    return path


def rels(items):
    """items: [(id, type_suffix, target)]."""
    body = ''.join(f'<Relationship Id="{i}" Type="{REL}{t}" Target="{target}"/>' for i, t, target in items)
    return f'{XML_DECL}<Relationships xmlns="{NS_PKG_REL}">{body}</Relationships>'


def content_types(overrides, defaults=(('rels', 'application/vnd.openxmlformats-package.relationships+xml'), ('xml', 'application/xml'))):
    d = ''.join(f'<Default Extension="{e}" ContentType="{t}"/>' for e, t in defaults)
    o = ''.join(f'<Override PartName="{p}" ContentType="{t}"/>' for p, t in overrides)
    return f'{XML_DECL}<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">{d}{o}</Types>'


def core_properties(title='FDTP test'):
    return (f'{XML_DECL}<cp:coreProperties xmlns:cp="http://schemas.openxmlformats.org/package/2006/metadata/core-properties" '
            'xmlns:dc="http://purl.org/dc/elements/1.1/" xmlns:dcterms="http://purl.org/dc/terms/" '
            'xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance">'
            f'<dc:title>{title}</dc:title><dc:creator>fdtp</dc:creator>'
            '<dcterms:created xsi:type="dcterms:W3CDTF">2024-01-02T03:04:05Z</dcterms:created></cp:coreProperties>')


SML = 'application/vnd.openxmlformats-officedocument.spreadsheetml.'
SHEET_HEAD = (f'<worksheet xmlns="{NS_MAIN}" xmlns:r="{NS_REL}" '
              'xmlns:mc="http://schemas.openxmlformats.org/markup-compatibility/2006" '
              'xmlns:x14ac="http://schemas.microsoft.com/office/spreadsheetml/2009/9/ac" mc:Ignorable="x14ac">')

SHARED = ['品名', '数量', '単価', 'りんご',
          '<r><t>み</t></r><r><rPr><b/></rPr><t>かん</t></r>',
          '<t>漢字</t><rPh sb="0" eb="2"><t>カンジ</t></rPh><phoneticPr fontId="1"/>',
          '合計', '結合見出し', 'A_x000D_B']


def shared_strings():
    items = []
    for s in SHARED:
        items.append(f'<si>{s}</si>' if s.startswith('<') else f'<si><t>{s}</t></si>')
    return f'{XML_DECL}<sst xmlns="{NS_MAIN}" count="{len(SHARED) + 2}" uniqueCount="{len(SHARED)}">{"".join(items)}</sst>'


def styles():
    xfs = [0, 14, 164, 2, 165, 10, 22, 20]
    xf = ''.join(f'<xf numFmtId="{n}" fontId="0" fillId="0" borderId="0" xfId="0"{" applyNumberFormat=\"1\"" if n else ""}/>' for n in xfs)
    return (f'{XML_DECL}<styleSheet xmlns="{NS_MAIN}">'
            '<numFmts count="2"><numFmt numFmtId="164" formatCode="yyyy/mm/dd"/><numFmt numFmtId="165" formatCode="0.000"/></numFmts>'
            '<fonts count="2"><font><sz val="11"/><name val="Calibri"/></font><font><sz val="6"/><name val="MS PGothic"/></font></fonts>'
            '<fills count="1"><fill><patternFill patternType="none"/></fill></fills>'
            '<borders count="1"><border><left/><right/><top/><bottom/><diagonal/></border></borders>'
            '<cellStyleXfs count="1"><xf numFmtId="49" fontId="0" fillId="0" borderId="0"/></cellStyleXfs>'
            f'<cellXfs count="{len(xfs)}">{xf}</cellXfs>'
            '<cellStyles count="1"><cellStyle name="Normal" xfId="0" builtinId="0"/></cellStyles></styleSheet>')


def s(ref, index, style=None):
    st = f' s="{style}"' if style is not None else ''
    return f'<c r="{ref}"{st} t="s"><v>{index}</v></c>'


def n(ref, value, style=None):
    st = f' s="{style}"' if style is not None else ''
    return f'<c r="{ref}"{st}><v>{value}</v></c>'


def inline(ref, text):
    return f'<c r="{ref}" t="inlineStr"><is><t>{text}</t></is></c>'


def f(ref, formula, value, style=None, extra=''):
    st = f' s="{style}"' if style is not None else ''
    return f'<c r="{ref}"{st}><f{extra}>{formula}</f><v>{value}</v></c>'


def sheet1():
    rows = [
        ('1', ' spans="1:6" x14ac:dyDescent="0.25"', s('A1', 0) + s('B1', 1) + s('C1', 2) + inline('D1', '金額') + s('E1', 7) + '<c r="F1" s="0"/>'),
        ('2', ' spans="1:6"', s('A2', 3) + n('B2', 10) + n('C2', 120.5, 3) + f('D2', 'B2*C2', 1205) + n('E2', 45292, 1)),
        ('3', ' spans="1:6"', s('A3', 4) + n('B3', 20) + n('C3', 80, 3) + f('D3', 'B3*C3', 1600, extra=' t="shared" ref="D3:D4" si="0"') + n('E3', 45323.5, 6)),
        ('4', ' spans="1:6"', s('A4', 5) + n('B4', 5) + n('C4', 200, 3) + '<c r="D4"><f t="shared" si="0"/><v>1000</v></c>' + n('E4', 0.5, 7)),
        ('6', '', s('A6', 6) + f('B6', 'SUM(B2:B4)', 35) + f('C6', 'AVERAGE(C2:C4)', 133.5, 3) + f('D6', 'SUM(D2:D4)', 3805)
         + f('E6', 'VLOOKUP(&quot;りんご&quot;,A2:D4,4,FALSE)', 1205)),
        ('7', '', inline('A7', '最小') + f('B7', 'MIN(B2:B4)', 5) + f('C7', 'MAX(B2:B4)', 20) + f('D7', 'COUNT(A2:D4)', 9) + f('E7', '(B2+B3)*2-B4/5', 59)),
        ('8', '', '<c r="A8" t="b"><v>1</v></c><c r="B8" s="2"/>' + n('C8', 3.14159, 4) + n('D8', 0.256, 5) + s('E8', 8)),
        ('9', ' spans="1:3"', inline('A9', 'x') + n('C9', 3)),
    ]
    body = ''.join(f'<row r="{r}"{attrs}>{cells}</row>' for r, attrs, cells in rows)
    body += '<row r="10" ht="30" customHeight="1"/>'
    return (f'{XML_DECL}{SHEET_HEAD}<sheetPr><pageSetUpPr fitToPage="1"/></sheetPr><dimension ref="A1:F10"/>'
            '<sheetViews><sheetView tabSelected="1" workbookViewId="0"><selection activeCell="A1" sqref="A1"/></sheetView></sheetViews>'
            '<sheetFormatPr defaultRowHeight="15" x14ac:dyDescent="0.25"/><cols><col min="1" max="1" width="12.7109375" customWidth="1"/></cols>'
            f'<sheetData>{body}</sheetData><mergeCells count="1"><mergeCell ref="E1:F1"/></mergeCells>'
            '<pageMargins left="0.7" right="0.7" top="0.75" bottom="0.75" header="0.3" footer="0.3"/></worksheet>')


def sheet2():
    # A BOM before the declaration, CRLF between rows, and cells without r.
    body = ('<row r="1">' + inline('A1', '予定') + '</row>\r\n'
            '<row r="2">' + n('B2', 1) + '</row>\r\n'
            '<row r="3">' + f('B3', 'B2+1', 2) + '</row>\r\n'
            '<row r="5"><c t="inlineStr"><is><t>無番地</t></is></c><c><v>7</v></c></row>\r\n')
    return '﻿' + f'{XML_DECL}{SHEET_HEAD}<dimension ref="A1:B5"/><sheetData>\r\n{body}</sheetData></worksheet>'


def sheet3():
    return f'{XML_DECL}{SHEET_HEAD}<dimension ref="A1"/><sheetData/><pageMargins left="0.7" right="0.7" top="0.75" bottom="0.75" header="0.3" footer="0.3"/></worksheet>'


CALC_CHAIN = ('<c r="D2" i="1"/><c r="D3"/><c r="D4"/><c r="B6"/><c r="C6"/><c r="D6"/><c r="E6"/>'
              '<c r="B7"/><c r="C7"/><c r="D7"/><c r="E7"/><c r="B3" i="2"/>')


def xlsx_parts():
    workbook = (f'{XML_DECL}<workbook xmlns="{NS_MAIN}" xmlns:r="{NS_REL}"><workbookPr defaultThemeVersion="124226"/>'
                '<bookViews><workbookView xWindow="0" yWindow="0" windowWidth="16000" windowHeight="9000"/></bookViews>'
                '<sheets><sheet name="売上" sheetId="1" r:id="rId1"/><sheet name="予定" sheetId="2" r:id="rId2"/>'
                '<sheet name="空" sheetId="3" r:id="rId3"/></sheets><calcPr calcId="191029"/></workbook>')
    overrides = [('/xl/workbook.xml', SML + 'sheet.main+xml')]
    overrides += [(f'/xl/worksheets/sheet{i}.xml', SML + 'worksheet+xml') for i in (1, 2, 3)]
    overrides += [('/xl/theme/theme1.xml', 'application/vnd.openxmlformats-officedocument.theme+xml'),
                  ('/xl/styles.xml', SML + 'styles+xml'), ('/xl/sharedStrings.xml', SML + 'sharedStrings+xml'),
                  ('/xl/calcChain.xml', SML + 'calcChain+xml'),
                  ('/docProps/core.xml', 'application/vnd.openxmlformats-package.core-properties+xml')]
    return [
        ('[Content_Types].xml', content_types(overrides)),
        ('_rels/.rels', rels([('rId1', 'officeDocument', 'xl/workbook.xml'),
                              ('rId2', 'metadata/core-properties', 'docProps/core.xml')]).replace(
            REL + 'metadata/core-properties', 'http://schemas.openxmlformats.org/package/2006/relationships/metadata/core-properties')),
        ('docProps/core.xml', core_properties()),
        ('xl/workbook.xml', workbook),
        ('xl/_rels/workbook.xml.rels', rels([('rId1', 'worksheet', 'worksheets/sheet1.xml'), ('rId2', 'worksheet', 'worksheets/sheet2.xml'),
                                             ('rId3', 'worksheet', 'worksheets/sheet3.xml'), ('rId4', 'theme', 'theme/theme1.xml'),
                                             ('rId5', 'styles', 'styles.xml'), ('rId6', 'sharedStrings', 'sharedStrings.xml'),
                                             ('rId7', 'calcChain', 'calcChain.xml')])),
        ('xl/worksheets/sheet1.xml', sheet1()),
        ('xl/worksheets/sheet2.xml', sheet2()),
        ('xl/worksheets/sheet3.xml', sheet3()),
        ('xl/theme/theme1.xml', f'{XML_DECL}<a:theme xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main" name="Office Theme"><a:themeElements/></a:theme>'),
        ('xl/styles.xml', styles()),
        ('xl/sharedStrings.xml', shared_strings()),
        ('xl/calcChain.xml', f'{XML_DECL}<calcChain xmlns="{NS_MAIN}">{CALC_CHAIN}</calcChain>'),
    ]


def make_xlsx(path):
    return package(path, xlsx_parts(), stored=('xl/theme/theme1.xml', 'docProps/core.xml'))


def make_formula_only_chain(path):
    """A workbook whose calcChain has one entry: removing that formula must
    remove the part, its relationship and its content type."""
    parts = [p for p in xlsx_parts() if not p[0].startswith('xl/worksheets/sheet')]
    sheet = (f'{XML_DECL}{SHEET_HEAD}<dimension ref="A1:B1"/><sheetData><row r="1">{n("A1", 2)}{f("B1", "A1*2", 4)}</row></sheetData></worksheet>')
    out = []
    for name, data in parts:
        if name == 'xl/calcChain.xml':
            data = f'{XML_DECL}<calcChain xmlns="{NS_MAIN}"><c r="B1" i="1"/></calcChain>'
        if name == 'xl/workbook.xml':
            data = data.replace('<sheet name="予定" sheetId="2" r:id="rId2"/><sheet name="空" sheetId="3" r:id="rId3"/>', '')
        if name == 'xl/_rels/workbook.xml.rels':
            data = rels([('rId1', 'worksheet', 'worksheets/sheet1.xml'), ('rId4', 'theme', 'theme/theme1.xml'),
                         ('rId5', 'styles', 'styles.xml'), ('rId6', 'sharedStrings', 'sharedStrings.xml'),
                         ('rId7', 'calcChain', 'calcChain.xml')])
        if name == '[Content_Types].xml':
            data = data.replace(f'<Override PartName="/xl/worksheets/sheet2.xml" ContentType="{SML}worksheet+xml"/>', '').replace(
                f'<Override PartName="/xl/worksheets/sheet3.xml" ContentType="{SML}worksheet+xml"/>', '')
        out.append((name, data))
        if name == 'xl/workbook.xml':
            out.append(('xl/worksheets/sheet1.xml', sheet))
    return package(path, out)


CSV = {
    'utf8.csv': '名前,値\n'
                'りんご,100\n'
                '"引用,あり","改行\nあり"\n'
                '"二重""引用",\n'.encode('utf-8'),
    'bom.csv': b'\xef\xbb\xbf' + 'a,b\r\n1,2'.encode('utf-8'),
    'sjis.csv': '品名,数量\r\nりんご,3\r\n"ﾊﾝｶｸ",①\r\n'.encode('cp932'),
    'data.tsv': b'x\ty\n1\t2\n',
}


def make_csv(directory):
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    for name, data in CSV.items():
        (directory / name).write_bytes(data)


# ---- pptx (stage 6) ----
PML = 'http://schemas.openxmlformats.org/presentationml/2006/main'
DML = 'http://schemas.openxmlformats.org/drawingml/2006/main'
PPTX_CT = 'application/vnd.openxmlformats-officedocument.presentationml.'
PPTX_NS = f'xmlns:a="{DML}" xmlns:r="{NS_REL}" xmlns:p="{PML}"'
PPTX_CX, PPTX_CY = 12192000, 6858000
# Theme colours of the fixture (accent1 is not Office's, to see it is read).
PPTX_THEME = {'dk1': '000000', 'lt1': 'FFFFFF', 'dk2': '1F3864', 'lt2': 'E7E6E6', 'accent1': '00B050', 'accent2': 'ED7D31',
              'accent3': 'A5A5A5', 'accent4': 'FFC000', 'accent5': '5B9BD5', 'accent6': '70AD47', 'hlink': '0563C1',
              'folHlink': '954F72'}


def png_bytes(w, h, pixel):
    """A truecolour PNG; pixel(x, y) -> (r, g, b)."""
    import struct
    import zlib

    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data) & 0xffffffff)
    raw = b''.join(b'\0' + bytes(c for x in range(w) for c in pixel(x, y)) for y in range(h))
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))


def pptx_theme():
    clr = ''.join(f'<a:{k}><a:srgbClr val="{v}"/></a:{k}>' for k, v in PPTX_THEME.items())
    fill = '<a:solidFill><a:schemeClr val="phClr"/></a:solidFill>'
    ln = '<a:ln w="9525"><a:solidFill><a:schemeClr val="phClr"/></a:solidFill></a:ln>'
    font = ('<a:latin typeface="{0}"/><a:ea typeface=""/><a:cs typeface=""/>'
            '<a:font script="Jpan" typeface="游ゴシック"/>')
    return (f'{XML_DECL}<a:theme xmlns:a="{DML}" name="FDTP"><a:themeElements><a:clrScheme name="FDTP">{clr}</a:clrScheme>'
            f'<a:fontScheme name="FDTP"><a:majorFont>{font.format("Calibri Light")}</a:majorFont>'
            f'<a:minorFont>{font.format("Calibri")}</a:minorFont></a:fontScheme>'
            f'<a:fmtScheme name="FDTP"><a:fillStyleLst>{fill * 3}</a:fillStyleLst><a:lnStyleLst>{ln * 3}</a:lnStyleLst>'
            '<a:effectStyleLst>' + '<a:effectStyle><a:effectLst/></a:effectStyle>' * 3 + '</a:effectStyleLst>'
            f'<a:bgFillStyleLst>{fill * 3}</a:bgFillStyleLst></a:fmtScheme></a:themeElements></a:theme>')


def pptx_xfrm(x, y, cx, cy, extra=''):
    return f'<a:xfrm{extra}><a:off x="{x}" y="{y}"/><a:ext cx="{cx}" cy="{cy}"/></a:xfrm>'


def pptx_run(text, size=None, color=None):
    attrs = f' lang="ja-JP" altLang="en-US"' + (f' sz="{size}"' if size else '')
    fill = f'<a:solidFill><a:srgbClr val="{color}"/></a:solidFill>' if color else ''
    return f'<a:r><a:rPr{attrs} dirty="0">{fill}</a:rPr><a:t>{text}</a:t></a:r>'


def pptx_para(runs, ppr=''):
    """runs: a string (one run) or a list of pptx_run() results."""
    body = pptx_run(runs) if isinstance(runs, str) else ''.join(runs)
    return f'<a:p>{ppr}{body}</a:p>'


def pptx_sp(sid, name, paras=None, xfrm=None, ph=None, fill=None, line=None, style=None, body_pr='', lst=''):
    """A p:sp. ph: '<p:ph .../>' text; paras: list of pptx_para(); fill/line: hex."""
    sppr = xfrm or ''
    if xfrm:
        sppr += '<a:prstGeom prst="rect"><a:avLst/></a:prstGeom>'
    if fill:
        sppr += f'<a:solidFill><a:srgbClr val="{fill}"/></a:solidFill>'
    if line:
        sppr += f'<a:ln w="{line[1]}"><a:solidFill><a:srgbClr val="{line[0]}"/></a:solidFill></a:ln>'
    tx = ''
    if paras is not None:
        tx = f'<p:txBody><a:bodyPr{body_pr}/><a:lstStyle>{lst}</a:lstStyle>{"".join(paras) or "<a:p><a:endParaRPr/></a:p>"}</p:txBody>'
    nvpr = f'<p:nvPr>{ph}</p:nvPr>' if ph else '<p:nvPr/>'
    locks = '<a:spLocks noGrp="1"/>' if ph else ''
    return (f'<p:sp><p:nvSpPr><p:cNvPr id="{sid}" name="{name}"/><p:cNvSpPr>{locks}</p:cNvSpPr>{nvpr}</p:nvSpPr>'
            f'<p:spPr>{sppr}</p:spPr>{style or ""}{tx}</p:sp>')


def pptx_pic(sid, rid, xfrm, crop='', flip=''):
    src = f'<a:srcRect {crop}/>' if crop else ''
    return (f'<p:pic><p:nvPicPr><p:cNvPr id="{sid}" name="図 {sid}"/><p:cNvPicPr><a:picLocks noChangeAspect="1"/></p:cNvPicPr><p:nvPr/></p:nvPicPr>'
            f'<p:blipFill><a:blip r:embed="{rid}"/>{src}<a:stretch><a:fillRect/></a:stretch></p:blipFill>'
            f'<p:spPr>{pptx_xfrm(*xfrm, extra=flip)}<a:prstGeom prst="rect"><a:avLst/></a:prstGeom></p:spPr></p:pic>')


def pptx_tree(shapes):
    return ('<p:spTree><p:nvGrpSpPr><p:cNvPr id="1" name=""/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr>'
            '<p:grpSpPr><a:xfrm><a:off x="0" y="0"/><a:ext cx="0" cy="0"/><a:chOff x="0" y="0"/><a:chExt cx="0" cy="0"/></a:xfrm></p:grpSpPr>'
            f'{"".join(shapes)}</p:spTree>')


def pptx_slide(shapes, bg=None):
    bgx = f'<p:bg><p:bgPr><a:solidFill><a:srgbClr val="{bg}"/></a:solidFill><a:effectLst/></p:bgPr></p:bg>' if bg else ''
    return (f'{XML_DECL}<p:sld {PPTX_NS}><p:cSld>{bgx}{pptx_tree(shapes)}</p:cSld>'
            '<p:clrMapOvr><a:masterClrMapping/></p:clrMapOvr></p:sld>')


def pptx_layout(name, shapes, bg=None, kind='obj'):
    bgx = f'<p:bg><p:bgPr><a:solidFill><a:srgbClr val="{bg}"/></a:solidFill><a:effectLst/></p:bgPr></p:bg>' if bg else ''
    return (f'{XML_DECL}<p:sldLayout {PPTX_NS} type="{kind}" preserve="1"><p:cSld name="{name}">{bgx}{pptx_tree(shapes)}</p:cSld>'
            '<p:clrMapOvr><a:masterClrMapping/></p:clrMapOvr></p:sldLayout>')


# Where the master and the layouts put their placeholders (EMU), for the checks.
PPTX_BOX = {
    'master_title': (838200, 365125, 10515600, 1325563),
    'master_body': (838200, 1825625, 10515600, 4351338),
    'layout1_title': (1524000, 1122363, 9144000, 2387600),
    'layout1_sub': (1524000, 3602038, 9144000, 1655762),
    'layout2_body': (838200, 1825625, 10515600, 4351338),
}


def pptx_master():
    def lvl(n, size, marl, bullet=True):
        bu = '<a:buFont typeface="Arial"/><a:buChar char="•"/>' if bullet else '<a:buNone/>'
        return (f'<a:lvl{n}pPr marL="{marl}" indent="{-228600 if bullet else 0}" algn="l">{bu}'
                f'<a:defRPr sz="{size}"><a:solidFill><a:schemeClr val="tx1"/></a:solidFill></a:defRPr></a:lvl{n}pPr>')
    title_style = (f'<p:titleStyle><a:lvl1pPr algn="l"><a:buNone/><a:defRPr sz="4400"><a:solidFill><a:schemeClr val="tx1"/>'
                   '</a:solidFill></a:defRPr></a:lvl1pPr></p:titleStyle>')
    body_style = f'<p:bodyStyle>{lvl(1, 2800, 228600)}{lvl(2, 2400, 685800)}</p:bodyStyle>'
    other_style = f'<p:otherStyle>{lvl(1, 1800, 0, False)}</p:otherStyle>'
    shapes = [pptx_sp(2, 'Title Placeholder 1', [pptx_para('マスタのタイトル')], pptx_xfrm(*PPTX_BOX['master_title']), '<p:ph type="title"/>',
                      body_pr=' anchor="ctr"'),
              pptx_sp(3, 'Text Placeholder 2', [pptx_para('マスタの本文')], pptx_xfrm(*PPTX_BOX['master_body']), '<p:ph type="body" idx="1"/>')]
    ids = ''.join(f'<p:sldLayoutId id="{2147483649 + i}" r:id="rId{i + 1}"/>' for i in range(3))
    return (f'{XML_DECL}<p:sldMaster {PPTX_NS}><p:cSld>{pptx_tree(shapes)}</p:cSld>'
            '<p:clrMap bg1="lt1" tx1="dk1" bg2="lt2" tx2="dk2" accent1="accent1" accent2="accent2" accent3="accent3" '
            'accent4="accent4" accent5="accent5" accent6="accent6" hlink="hlink" folHlink="folHlink"/>'
            f'<p:sldLayoutIdLst>{ids}</p:sldLayoutIdLst><p:txStyles>{title_style}{body_style}{other_style}</p:txStyles></p:sldMaster>')


def pptx_layouts():
    title = pptx_layout('タイトル スライド', [
        pptx_sp(2, 'Title 1', [pptx_para('タイトル')], pptx_xfrm(*PPTX_BOX['layout1_title']), '<p:ph type="ctrTitle"/>',
                body_pr=' anchor="b"', lst='<a:lvl1pPr algn="ctr"><a:defRPr sz="6000"/></a:lvl1pPr>'),
        pptx_sp(3, 'Subtitle 2', [pptx_para('サブタイトル')], pptx_xfrm(*PPTX_BOX['layout1_sub']), '<p:ph type="subTitle" idx="1"/>',
                lst='<a:lvl1pPr marL="0" indent="0" algn="ctr"><a:buNone/><a:defRPr sz="2400"/></a:lvl1pPr>')], kind='title')
    # The content layout's title has no position: the master's is used.
    content = pptx_layout('タイトルとコンテンツ', [
        pptx_sp(2, 'Title 1', [pptx_para('タイトル')], None, '<p:ph type="title"/>'),
        pptx_sp(3, 'Content 2', [pptx_para('本文')], pptx_xfrm(*PPTX_BOX['layout2_body']), '<p:ph idx="1"/>')])
    blank = pptx_layout('白紙', [], bg='DDEBF7', kind='blank')
    return [title, content, blank]


def pptx_style_ref(idx=1, scheme='accent1'):
    return (f'<p:style><a:lnRef idx="0"><a:schemeClr val="{scheme}"/></a:lnRef><a:fillRef idx="{idx}"><a:schemeClr val="{scheme}"/></a:fillRef>'
            '<a:effectRef idx="0"><a:schemeClr val="accent1"/></a:effectRef><a:fontRef idx="minor"><a:schemeClr val="lt1"/></a:fontRef></p:style>')


# Fixture geometry (EMU) that tests/check_slide.py looks at.
PPTX_GEOM = {
    'sizes': [(1800, (600000, 1600000)), (2400, (3200000, 1600000)), (3200, (5800000, 1600000)), (5400, (8400000, 1600000))],
    'size_box': (2400000, 1600000),
    'rect': (600000, 4000000, 2400000, 1600000),
    'style_rect': (3600000, 4000000, 2400000, 1600000),
    'pic_full': (600000, 1500000, 2400000, 2400000),
    'pic_crop': (3400000, 1500000, 2400000, 2400000),
    'pic_jpeg': (6200000, 1500000, 2400000, 2400000),
    'pic_flip': (9000000, 1500000, 2400000, 2400000),
    'group': (1000000, 1000000, 4000000, 2000000, 8000000, 4000000),   # off, ext, chExt (chOff 0)
    'group_child': (2000000, 2000000, 2000000, 2000000),
    'connector': (6000000, 5000000, 5000000, 0),
    'table': (1000000, 1500000, 6000000, 1600000),
    'chart': (7600000, 1500000, 3600000, 2400000),
    'choice': (600000, 4500000, 1800000, 1200000),
    'fallback': (3000000, 4500000, 1800000, 1200000),
}


def pptx_slides():
    g = PPTX_GEOM
    s1 = pptx_slide([
        pptx_sp(2, 'Title 1', [pptx_para('日本語のタイトル')], None, '<p:ph type="ctrTitle"/>'),
        pptx_sp(3, 'Subtitle 2', [pptx_para('副題 Subtitle')], None, '<p:ph type="subTitle" idx="1"/>')], bg='FFF2CC')
    boxes = [pptx_sp(10 + i, f'TextBox {i}', [pptx_para([pptx_run('■', size)])], pptx_xfrm(*pos, *g['size_box']),
                     body_pr=' wrap="square" lIns="0" tIns="0" rIns="0" bIns="0"') for i, (size, pos) in enumerate(g['sizes'])]
    s2 = pptx_slide([pptx_sp(2, 'Title 1', [pptx_para([pptx_run('大きさの違う文字', 3200)])], pptx_xfrm(600000, 300000, 10000000, 1000000), '<p:ph type="title"/>')]
                    + boxes + [
        pptx_sp(20, 'Rect', [], pptx_xfrm(*g['rect']), fill='FF0000', line=('0000FF', 38100)),
        pptx_sp(21, 'Styled', [pptx_para('図形の文字')], pptx_xfrm(*g['style_rect']), style=pptx_style_ref(), body_pr=' anchor="ctr"')])
    s3 = pptx_slide([
        pptx_sp(2, 'Title 1', [pptx_para('マスタの位置の題')], None, '<p:ph type="title"/>'),
        pptx_sp(3, 'Content 2', [pptx_para('項目一'), pptx_para('項目二', '<a:pPr lvl="1"/>')], None, '<p:ph idx="1"/>')])
    s4 = pptx_slide([
        pptx_pic(2, 'rId2', g['pic_full']), pptx_pic(3, 'rId2', g['pic_crop'], crop='l="50000"'),
        pptx_pic(4, 'rId3', g['pic_jpeg']), pptx_pic(5, 'rId2', g['pic_flip'], flip=' flipH="1"')])
    off, ext, chext = g['group'][:2], g['group'][2:4], g['group'][4:]
    group = ('<p:grpSp><p:nvGrpSpPr><p:cNvPr id="2" name="Group 1"/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr>'
             f'<p:grpSpPr><a:xfrm><a:off x="{off[0]}" y="{off[1]}"/><a:ext cx="{ext[0]}" cy="{ext[1]}"/>'
             f'<a:chOff x="0" y="0"/><a:chExt cx="{chext[0]}" cy="{chext[1]}"/></a:xfrm></p:grpSpPr>'
             + pptx_sp(3, 'Child', [], pptx_xfrm(*g['group_child']), fill='7030A0') + '</p:grpSp>')
    cx = g['connector']
    connector = ('<p:cxnSp><p:nvCxnSpPr><p:cNvPr id="4" name="Connector"/><p:cNvCxnSpPr/><p:nvPr/></p:nvCxnSpPr>'
                 f'<p:spPr>{pptx_xfrm(*cx)}<a:prstGeom prst="line"><a:avLst/></a:prstGeom>'
                 '<a:ln w="38100"><a:solidFill><a:srgbClr val="FF6600"/></a:solidFill></a:ln></p:spPr></p:cxnSp>')
    s5 = pptx_slide([group, connector])

    def tc(text, fill=None):
        f_ = f'<a:solidFill><a:srgbClr val="{fill}"/></a:solidFill>' if fill else ''
        return f'<a:tc><a:txBody><a:bodyPr/><a:lstStyle/>{pptx_para([pptx_run(text, 1800)])}</a:txBody><a:tcPr>{f_}</a:tcPr></a:tc>'
    tx, ty, tw, th = g['table']
    table = ('<p:graphicFrame><p:nvGraphicFramePr><p:cNvPr id="2" name="表 1"/><p:cNvGraphicFramePr><a:graphicFrameLocks noGrp="1"/>'
             f'</p:cNvGraphicFramePr><p:nvPr/></p:nvGraphicFramePr><p:xfrm><a:off x="{tx}" y="{ty}"/><a:ext cx="{tw}" cy="{th}"/></p:xfrm>'
             '<a:graphic><a:graphicData uri="http://schemas.openxmlformats.org/drawingml/2006/table"><a:tbl><a:tblPr firstRow="1"/>'
             '<a:tblGrid>' + '<a:gridCol w="2000000"/>' * 3 + '</a:tblGrid>'
             f'<a:tr h="800000">{tc("見出し", "FFC000")}{tc("列B")}{tc("列C")}</a:tr>'
             f'<a:tr h="800000">{tc("行2")}{tc("数値")}{tc("")}</a:tr></a:tbl></a:graphicData></a:graphic></p:graphicFrame>')
    chx, chy, chw, chh = g['chart']
    chart = ('<p:graphicFrame><p:nvGraphicFramePr><p:cNvPr id="3" name="グラフ 2"/><p:cNvGraphicFramePr/><p:nvPr/></p:nvGraphicFramePr>'
             f'<p:xfrm><a:off x="{chx}" y="{chy}"/><a:ext cx="{chw}" cy="{chh}"/></p:xfrm><a:graphic>'
             '<a:graphicData uri="http://schemas.openxmlformats.org/drawingml/2006/chart">'
             '<c:chart xmlns:c="http://schemas.openxmlformats.org/drawingml/2006/chart" r:id="rId2"/></a:graphicData></a:graphic></p:graphicFrame>')
    alt = ('<mc:AlternateContent xmlns:mc="http://schemas.openxmlformats.org/markup-compatibility/2006">'
           # A namespace no reader knows: PowerPoint takes the fallback as well.
           '<mc:Choice xmlns:fdtpx="urn:fdtp:test-extension" Requires="fdtpx">'
           + pptx_sp(4, 'Choice', [], pptx_xfrm(*g['choice']), fill='FF0000') + '</mc:Choice><mc:Fallback>'
           + pptx_sp(5, 'Fallback', [], pptx_xfrm(*g['fallback']), fill='00B0F0') + '</mc:Fallback></mc:AlternateContent>')
    s6 = pptx_slide([table, chart, alt])
    return [s1, s2, s3, s4, s5, s6]


def pptx_chart():
    c = 'http://schemas.openxmlformats.org/drawingml/2006/chart'
    lit = lambda kind, vals: (f'<c:{kind}><c:ptCount val="{len(vals)}"/>' + ''.join(f'<c:pt idx="{i}"><c:v>{v}</c:v></c:pt>' for i, v in enumerate(vals)) + f'</c:{kind}>')
    ax = '<c:scaling><c:orientation val="minMax"/></c:scaling><c:delete val="0"/>'
    return (f'{XML_DECL}<c:chartSpace xmlns:c="{c}" xmlns:a="{DML}" xmlns:r="{NS_REL}"><c:chart><c:autoTitleDeleted val="1"/><c:plotArea><c:layout/>'
            '<c:barChart><c:barDir val="col"/><c:grouping val="clustered"/><c:varyColors val="0"/>'
            f'<c:ser><c:idx val="0"/><c:order val="0"/><c:cat>{lit("strLit", ["A", "B"])}</c:cat><c:val>{lit("numLit", [3, 5])}</c:val></c:ser>'
            '<c:axId val="1"/><c:axId val="2"/></c:barChart>'
            f'<c:catAx><c:axId val="1"/>{ax}<c:axPos val="b"/><c:crossAx val="2"/></c:catAx>'
            f'<c:valAx><c:axId val="2"/>{ax}<c:axPos val="l"/><c:crossAx val="1"/></c:valAx></c:plotArea><c:plotVisOnly val="1"/></c:chart></c:chartSpace>')


def pptx_png():
    """16x16, left half red, right half green."""
    return png_bytes(16, 16, lambda x, y: (255, 0, 0) if x < 8 else (0, 255, 0))


def pptx_parts(jpeg, slides=None, layouts_of=None, extra_slide_rels=None, media=None):
    """The whole package. jpeg: bytes of a JPEG (top blue, bottom yellow) for
    slide 4; slides/layouts_of override the slide XML and their layout numbers."""
    slides = slides if slides is not None else pptx_slides()
    layouts_of = layouts_of or [1, 2, 2, 2, 2, 3][:len(slides)] + [2] * max(0, len(slides) - 6)
    extra_slide_rels = extra_slide_rels or {4: [('rId2', 'image', '../media/image1.png'), ('rId3', 'image', '../media/image2.jpeg')],
                                            6: [('rId2', 'chart', '../charts/chart1.xml')]}
    media = media if media is not None else [('ppt/media/image1.png', pptx_png()), ('ppt/media/image2.jpeg', jpeg)]
    count = len(slides)
    overrides = [('/ppt/presentation.xml', PPTX_CT + 'presentation.main+xml'), ('/ppt/slideMasters/slideMaster1.xml', PPTX_CT + 'slideMaster+xml'),
                 ('/ppt/theme/theme1.xml', 'application/vnd.openxmlformats-officedocument.theme+xml'),
                 ('/docProps/core.xml', 'application/vnd.openxmlformats-package.core-properties+xml')]
    overrides += [(f'/ppt/slideLayouts/slideLayout{i}.xml', PPTX_CT + 'slideLayout+xml') for i in (1, 2, 3)]
    overrides += [(f'/ppt/slides/slide{i + 1}.xml', PPTX_CT + 'slide+xml') for i in range(count)]
    if any(t == 'chart' for rels_ in extra_slide_rels.values() for _, t, _ in rels_):
        overrides.append(('/ppt/charts/chart1.xml', 'application/vnd.openxmlformats-officedocument.drawingml.chart+xml'))
    defaults = (('rels', 'application/vnd.openxmlformats-package.relationships+xml'), ('xml', 'application/xml'),
                ('png', 'image/png'), ('jpeg', 'image/jpeg'))
    pres_rels = [('rId1', 'slideMaster', 'slideMasters/slideMaster1.xml'), ('rId2', 'theme', 'theme/theme1.xml')]
    pres_rels += [(f'rId{10 + i}', 'slide', f'slides/slide{i + 1}.xml') for i in range(count)]
    ids = ''.join(f'<p:sldId id="{256 + i}" r:id="rId{10 + i}"/>' for i in range(count))
    presentation = (f'{XML_DECL}<p:presentation {PPTX_NS} saveSubsetFonts="1">'
                    '<p:sldMasterIdLst><p:sldMasterId id="2147483648" r:id="rId1"/></p:sldMasterIdLst>'
                    f'<p:sldIdLst>{ids}</p:sldIdLst><p:sldSz cx="{PPTX_CX}" cy="{PPTX_CY}"/><p:notesSz cx="6858000" cy="9144000"/>'
                    '<p:defaultTextStyle><a:defPPr><a:defRPr lang="ja-JP"/></a:defPPr><a:lvl1pPr marL="0" algn="l">'
                    '<a:defRPr sz="1800"><a:solidFill><a:schemeClr val="tx1"/></a:solidFill></a:defRPr></a:lvl1pPr></p:defaultTextStyle></p:presentation>')
    parts = [
        ('[Content_Types].xml', content_types(overrides, defaults)),
        ('_rels/.rels', rels([('rId1', 'officeDocument', 'ppt/presentation.xml'), ('rId2', 'metadata/core-properties', 'docProps/core.xml')]).replace(
            REL + 'metadata/core-properties', 'http://schemas.openxmlformats.org/package/2006/relationships/metadata/core-properties')),
        ('docProps/core.xml', core_properties('FDTP slides')),
        ('ppt/presentation.xml', presentation),
        ('ppt/_rels/presentation.xml.rels', rels(pres_rels)),
        ('ppt/slideMasters/slideMaster1.xml', pptx_master()),
        ('ppt/slideMasters/_rels/slideMaster1.xml.rels', rels([(f'rId{i}', 'slideLayout', f'../slideLayouts/slideLayout{i}.xml') for i in (1, 2, 3)]
                                                               + [('rId4', 'theme', '../theme/theme1.xml')])),
        ('ppt/theme/theme1.xml', pptx_theme()),
    ]
    for i, layout in enumerate(pptx_layouts(), 1):
        parts.append((f'ppt/slideLayouts/slideLayout{i}.xml', layout))
        parts.append((f'ppt/slideLayouts/_rels/slideLayout{i}.xml.rels', rels([('rId1', 'slideMaster', '../slideMasters/slideMaster1.xml')])))
    for i, slide in enumerate(slides, 1):
        parts.append((f'ppt/slides/slide{i}.xml', slide))
        parts.append((f'ppt/slides/_rels/slide{i}.xml.rels', rels([('rId1', 'slideLayout', f'../slideLayouts/slideLayout{layouts_of[i - 1]}.xml')]
                                                                    + extra_slide_rels.get(i, []))))
    if any(t == 'chart' for rels_ in extra_slide_rels.values() for _, t, _ in rels_):
        parts.append(('ppt/charts/chart1.xml', pptx_chart()))
    return parts + list(media)


def make_pptx(path, jpeg):
    """Six slides for the stage 6 checks (and for comparing with PowerPoint)."""
    return package(path, pptx_parts(jpeg), stored=('ppt/media/image1.png', 'ppt/media/image2.jpeg'))


def make_broken_pptx(directory, jpeg):
    """Damaged or odd packages; the viewer must report them, never crash.
    Returns {name: path}."""
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    out = {}
    good = make_pptx(directory / 'good-base.pptx', jpeg)
    data = good.read_bytes()
    (directory / 'not-zip.pptx').write_bytes(b'This is not a zip file.\n' * 10)
    out['not-zip'] = directory / 'not-zip.pptx'
    for n in (30, len(data) // 3, len(data) // 2, len(data) - 30):
        p = directory / f'truncated-{n}.pptx'
        p.write_bytes(data[:n])
        out[f'truncated-{n}'] = p
    flipped = bytearray(data)
    for i in range(200, len(flipped) - 200, 397):
        flipped[i] ^= 0x5a
    (directory / 'bitflip.pptx').write_bytes(bytes(flipped))
    out['bitflip'] = directory / 'bitflip.pptx'
    parts = pptx_parts(jpeg)
    out['no-presentation'] = package(directory / 'no-presentation.pptx', [p for p in parts if p[0] != 'ppt/presentation.xml'])
    out['no-slides'] = package(directory / 'no-slides.pptx', pptx_parts(jpeg, slides=[], extra_slide_rels={9: []}))
    out['bad-presentation'] = package(directory / 'bad-presentation.pptx',
                                      [(n_, d[:len(d) // 2] if n_ == 'ppt/presentation.xml' else d) for n_, d in parts])
    # Slide 2 is broken XML, slide 3 lacks its layout part, the picture is
    # not an image, numbers are huge and groups nest deeply.
    deep = '<p:grpSp><p:nvGrpSpPr><p:cNvPr id="9" name="g"/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr><p:grpSpPr/>' * 60
    deep += pptx_sp(10, 'deep', [pptx_para('深い')], pptx_xfrm(0, 0, 1000000, 1000000), fill='00FF00') + '</p:grpSp>' * 60
    odd = pptx_slide([pptx_pic(2, 'rId2', (100000, 100000, 3000000, 3000000)), pptx_pic(3, 'rId9', (4000000, 100000, 3000000, 3000000)),
                      pptx_sp(4, 'huge', [pptx_para('巨大')], pptx_xfrm(9223372036854775807, -9223372036854775807, 9223372036854775807, 9223372036854775807)),
                      pptx_sp(5, 'negative', [pptx_para('負')], pptx_xfrm(100, 100, -5000, -5000), fill='FF0000'), deep,
                      '<p:graphicFrame><p:nvGraphicFramePr><p:cNvPr id="6" name="t"/><p:cNvGraphicFramePr/><p:nvPr/></p:nvGraphicFramePr>'
                      '<p:xfrm><a:off x="0" y="4000000"/><a:ext cx="4000000" cy="1000000"/></p:xfrm><a:graphic>'
                      '<a:graphicData uri="http://schemas.openxmlformats.org/drawingml/2006/table"><a:tbl><a:tr h="0"><a:tc rowSpan="999" gridSpan="999"/></a:tr></a:tbl>'
                      '</a:graphicData></a:graphic></p:graphicFrame>'])
    slides = [pptx_slides()[0], pptx_slide([pptx_sp(2, 't', [pptx_para('途中')], pptx_xfrm(0, 0, 100, 100))])[:300], pptx_slides()[2], odd]
    odd_parts = pptx_parts(jpeg, slides=slides, layouts_of=[1, 2, 9, 2],
                           extra_slide_rels={4: [('rId2', 'image', '../media/bad.png')]},
                           media=[('ppt/media/bad.png', b'\x89PNG\r\n\x1a\nbroken')])
    out['odd-slides'] = package(directory / 'odd-slides.pptx', odd_parts)
    return out



# ---- docx (stage 5) ----
WML = 'http://schemas.openxmlformats.org/wordprocessingml/2006/main'
WPML = 'application/vnd.openxmlformats-officedocument.wordprocessingml.'
# The root carries the namespaces Word writes; the saved file must keep them.
DOCX_ROOT = ('<w:document xmlns:wpc="http://schemas.microsoft.com/office/word/2010/wordprocessingCanvas" '
             'xmlns:mc="http://schemas.openxmlformats.org/markup-compatibility/2006" '
             'xmlns:o="urn:schemas-microsoft-com:office:office" '
             f'xmlns:r="{NS_REL}" '
             'xmlns:m="http://schemas.openxmlformats.org/officeDocument/2006/math" '
             'xmlns:v="urn:schemas-microsoft-com:vml" '
             'xmlns:wp="http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing" '
             'xmlns:w10="urn:schemas-microsoft-com:office:word" '
             f'xmlns:w="{WML}" '
             'xmlns:w14="http://schemas.microsoft.com/office/word/2010/wordml" '
             'xmlns:w15="http://schemas.microsoft.com/office/word/2012/wordml" '
             'xmlns:wpg="http://schemas.microsoft.com/office/word/2010/wordprocessingGroup" '
             'xmlns:wps="http://schemas.microsoft.com/office/word/2010/wordprocessingShape" '
             'mc:Ignorable="w14 w15">')


def docx_png():
    """A 4x4 PNG (red and blue quarters)."""
    import struct
    import zlib
    rows = b''
    for y in range(4):
        rows += b'\x00' + b''.join(bytes([255, 0, 0] if (x < 2) == (y < 2) else [0, 0, 255]) for x in range(4))

    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data) & 0xffffffff)
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 4, 4, 8, 2, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))


def w_run(text, rpr='', attrs=''):
    space = ' xml:space="preserve"' if text != text.strip() else ''
    rp = f'<w:rPr>{rpr}</w:rPr>' if rpr else ''
    return f'<w:r{attrs}>{rp}<w:t{space}>{text}</w:t></w:r>'


def w_para(body, ppr='', attrs=''):
    pp = f'<w:pPr>{ppr}</w:pPr>' if ppr else ''
    return f'<w:p{attrs}>{pp}{body}</w:p>'


DOCX_DRAWING = ('<w:r><w:rPr><w:noProof/></w:rPr><w:drawing><wp:inline distT="0" distB="0" distL="0" distR="0">'
                '<wp:extent cx="381000" cy="381000"/><wp:docPr id="1" name="図 1"/>'
                '<a:graphic xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main">'
                '<a:graphicData uri="http://schemas.openxmlformats.org/drawingml/2006/picture">'
                '<pic:pic xmlns:pic="http://schemas.openxmlformats.org/drawingml/2006/picture">'
                '<pic:nvPicPr><pic:cNvPr id="0" name="image1.png"/><pic:cNvPicPr/></pic:nvPicPr>'
                '<pic:blipFill><a:blip r:embed="rId3"/><a:stretch><a:fillRect/></a:stretch></pic:blipFill>'
                '<pic:spPr><a:xfrm><a:off x="0" y="0"/><a:ext cx="381000" cy="381000"/></a:xfrm>'
                '<a:prstGeom prst="rect"><a:avLst/></a:prstGeom></pic:spPr></pic:pic></a:graphicData></a:graphic>'
                '</wp:inline></w:drawing></w:r>')

LONG_TEXT = ('長い段落の折り返しを確かめるための文です。' * 6) + 'ここで終わり。'


def tc(text, width, extra=''):
    paras = ''.join(w_para(w_run(t)) for t in text.split('\n')) if text else '<w:p/>'
    return f'<w:tc><w:tcPr><w:tcW w:w="{width}" w:type="dxa"/>{extra}</w:tcPr>{paras}</w:tc>'


def docx_table():
    grid = '<w:tblGrid><w:gridCol w:w="2000"/><w:gridCol w:w="3000"/><w:gridCol w:w="3000"/></w:tblGrid>'
    rows = [
        tc('品名', 2000) + tc('説明', 3000) + tc('価格', 3000),
        tc('りんご', 2000) + tc('赤い果物\n二つ目の段落', 3000) + tc('120', 3000),
        tc('結合', 2000) + tc('二列にまたがる', 6000, '<w:gridSpan w:val="2"/>'),
    ]
    body = ''.join(f'<w:tr w:rsidR="00A10001">{r}</w:tr>' for r in rows)
    return ('<w:tbl><w:tblPr><w:tblStyle w:val="TableGrid"/><w:tblW w:w="0" w:type="auto"/>'
            '<w:tblLook w:val="04A0" w:firstRow="1" w:lastRow="0" w:firstColumn="1" w:lastColumn="0" w:noHBand="0" w:noVBand="1"/>'
            f'</w:tblPr>{grid}{body}</w:tbl>')


# (label, xml) in body order; tests/check_doc.py refers to the labels.
def docx_body():
    rsid = ' w:rsidR="00A10001" w:rsidRDefault="00A10001"'
    return [
        ('heading', w_para(w_run('第一章　はじめに'), '<w:pStyle w:val="1"/>', ' w14:paraId="1A2B3C01" w14:textId="77777777"' + rsid)),
        ('body', w_para(w_run('これは本文の段落です。', '<w:rFonts w:hint="eastAsia"/>', ' w:rsidRPr="00B20002"'),
                        '<w:jc w:val="both"/><w:rPr><w:rFonts w:hint="eastAsia"/></w:rPr>', ' w14:paraId="1A2B3C02" w14:textId="77777777"' + rsid)),
        ('mixed', w_para(w_run('太字', '<w:b/><w:bCs/>') + '<w:proofErr w:type="spellStart"/>' + w_run('と普通')
                         + '<w:proofErr w:type="spellEnd"/>' + w_run(' mixed ', '<w:i/>') + w_run('&amp; &lt;記号&gt;'))),
        ('bookmark', w_para('<w:bookmarkStart w:id="0" w:name="mark1"/>' + w_run('ブックマーク') + '<w:bookmarkEnd w:id="0"/>'
                            + w_run('の段落'))),
        ('field', w_para(w_run('ページ ') + '<w:r><w:fldChar w:fldCharType="begin"/></w:r><w:r><w:instrText xml:space="preserve"> PAGE </w:instrText></w:r>'
                         '<w:r><w:fldChar w:fldCharType="separate"/></w:r>' + w_run('1') + '<w:r><w:fldChar w:fldCharType="end"/></w:r>')),
        ('table', docx_table()),
        ('pagebreak', w_para('<w:r><w:br w:type="page"/></w:r>')),
        ('image', w_para(w_run('図：') + DOCX_DRAWING)),
        ('heading2', w_para(w_run('第二章'), '<w:pStyle w:val="MyHead"/><w:pageBreakBefore/>')),
        ('comment', w_para('<w:commentRangeStart w:id="0"/>' + w_run('コメント付き') + '<w:commentRangeEnd w:id="0"/>'
                           '<w:r><w:rPr><w:rStyle w:val="CommentReference"/></w:rPr><w:commentReference w:id="0"/></w:r>')),
        ('link', w_para(w_run('リンク: ') + '<w:hyperlink r:id="rId5" w:history="1">' + w_run('example', '<w:rStyle w:val="Hyperlink"/>') + '</w:hyperlink>')),
        ('long', w_para(w_run(LONG_TEXT))),
        ('tracked', w_para(w_run('変更') + '<w:ins w:id="1" w:author="test" w:date="2024-01-02T03:04:00Z">' + w_run('履歴') + '</w:ins>')),
        ('tabs', w_para('<w:r><w:t>A</w:t><w:tab/><w:t>B</w:t><w:br/><w:t>C</w:t></w:r>')),
        ('empty', '<w:p w:rsidR="00A10001"/>'),
        ('section', w_para(w_run('節の終わり'), '<w:sectPr><w:pgSz w:w="11906" w:h="16838"/><w:pgMar w:top="1985" w:right="1701" w:bottom="1701" w:left="1701" w:header="851" w:footer="992" w:gutter="0"/></w:sectPr>')),
        ('last', w_para(w_run('最後の段落'))),
    ]


DOCX_SECT = ('<w:sectPr w:rsidR="00A10001"><w:pgSz w:w="11906" w:h="16838"/>'
             '<w:pgMar w:top="1985" w:right="1701" w:bottom="1701" w:left="1701" w:header="851" w:footer="992" w:gutter="0"/>'
             '<w:cols w:space="425"/><w:docGrid w:type="lines" w:linePitch="360"/></w:sectPr>')


def docx_document(body=None):
    items = docx_body() if body is None else body
    # Line breaks between the body children, as some writers put them.
    inner = '\r\n'.join(x for _, x in items)
    return f'{XML_DECL}{DOCX_ROOT}<w:body>{inner}{DOCX_SECT}</w:body></w:document>'


def docx_styles():
    def style(sid, name, based='', extra=''):
        b = f'<w:basedOn w:val="{based}"/>' if based else ''
        return f'<w:style w:type="paragraph" w:styleId="{sid}"><w:name w:val="{name}"/>{b}{extra}</w:style>'
    return (f'{XML_DECL}<w:styles xmlns:w="{WML}">'
            + style('a', 'Normal', extra='<w:qFormat/>')
            + style('1', 'heading 1', 'a', '<w:pPr><w:outlineLvl w:val="0"/></w:pPr><w:rPr><w:b/></w:rPr>')
            + style('MyHead', '独自の章題', '1')
            + style('Quote', 'Quote', 'a')
            + '<w:style w:type="character" w:styleId="Hyperlink"><w:name w:val="Hyperlink"/></w:style>'
            + '</w:styles>')


def docx_parts(document=None):
    overrides = [('/word/document.xml', WPML + 'document.main+xml'), ('/word/styles.xml', WPML + 'styles+xml'),
                 ('/word/comments.xml', WPML + 'comments+xml'),
                 ('/docProps/core.xml', 'application/vnd.openxmlformats-package.core-properties+xml')]
    defaults = (('rels', 'application/vnd.openxmlformats-package.relationships+xml'), ('xml', 'application/xml'), ('png', 'image/png'))
    comments = (f'{XML_DECL}<w:comments xmlns:w="{WML}"><w:comment w:id="0" w:author="test" w:date="2024-01-02T03:04:00Z" w:initials="t">'
                '<w:p><w:r><w:t>コメントの本文</w:t></w:r></w:p></w:comment></w:comments>')
    return [
        ('[Content_Types].xml', content_types(overrides, defaults)),
        ('_rels/.rels', rels([('rId1', 'officeDocument', 'word/document.xml'),
                              ('rId2', 'metadata/core-properties', 'docProps/core.xml')]).replace(
            REL + 'metadata/core-properties', 'http://schemas.openxmlformats.org/package/2006/relationships/metadata/core-properties')),
        ('docProps/core.xml', core_properties('FDTP docx test')),
        ('word/document.xml', docx_document() if document is None else document),
        ('word/_rels/document.xml.rels', rels([('rId1', 'styles', 'styles.xml'), ('rId3', 'image', 'media/image1.png'),
                                               ('rId4', 'comments', 'comments.xml')]).replace(
            '</Relationships>', f'<Relationship Id="rId5" Type="{REL}hyperlink" Target="https://example.com/" TargetMode="External"/></Relationships>')),
        ('word/styles.xml', docx_styles()),
        ('word/comments.xml', comments),
        ('word/media/image1.png', docx_png()),
    ]


def make_docx(path, document=None):
    return package(path, docx_parts(document), stored=('word/media/image1.png',))


def make_broken_docx(directory):
    """Files that must give a notice, not a crash."""
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    (directory / 'notzip.docx').write_bytes(b'This is not a zip file at all.\n')
    parts = [p for p in docx_parts() if p[0] != 'word/document.xml']
    package(directory / 'nodocument.docx', parts)
    make_docx(directory / 'badxml.docx', f'{XML_DECL}{DOCX_ROOT}<w:body><w:p><w:r><w:t>閉じていない</w:r></w:p></w:body></w:document>')
    make_docx(directory / 'notword.docx', f'{XML_DECL}<root><p>x</p></root>')
    whole = make_docx(directory / 'whole.docx').read_bytes()
    (directory / 'truncated.docx').write_bytes(whole[:len(whole) // 2])


def main():
    out = Path(sys.argv[1] if len(sys.argv) > 1 else 'build/office-fixtures')
    make_xlsx(out / 'book.xlsx')
    make_formula_only_chain(out / 'chain.xlsx')
    make_csv(out)
    make_docx(out / 'doc.docx')
    print(out)


if __name__ == '__main__':
    main()
