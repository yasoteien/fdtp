#!/usr/bin/env python3
# This file is part of FDTP (File & Directory Tool for Pomera).
# SPDX-FileCopyrightText: 2026 yasoteien
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Validate FDTP translations and generate its UTF-8, strcmp-sorted table."""
from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path
import re
import sys
import tempfile


JAPANESE = re.compile(r"[\u3000-\u30ff\u31f0-\u31ff\u3400-\u4dbf\u4e00-\u9fff\uf900-\ufaff\uff01-\uffef\U00020000-\U000323af]")
PRINTF = re.compile(r"%(?:[1-9]\d*\$)?[-+ #0']*(?:\d+|\*(?:[1-9]\d*\$)?)?(?:\.(?:\d*|\*(?:[1-9]\d*\$)?))?(?:hh|ll|[hljztL])?[diouxXfFeEgGaAcspn%m]")


@dataclass
class Token:
    kind: str
    value: str
    line: int
    end_line: int


def c_string_bytes(raw: str) -> bytes:
    """Decode C escapes without corrupting literal UTF-8 Japanese characters."""
    result = bytearray()
    i = 0
    escapes = {"a": 7, "b": 8, "f": 12, "n": 10, "r": 13, "t": 9,
               "v": 11, "\\": 92, '"': 34, "'": 39, "?": 63}
    while i < len(raw):
        if raw[i] != "\\":
            result.extend(raw[i].encode("utf-8"))
            i += 1
            continue
        i += 1
        if i >= len(raw):
            raise ValueError("unterminated C escape")
        ch = raw[i]
        i += 1
        if ch == "\n":
            continue
        if ch == "\r" and i < len(raw) and raw[i] == "\n":
            i += 1
            continue
        if ch in escapes:
            result.append(escapes[ch])
        elif ch in "01234567":
            end = i
            while end < min(i + 2, len(raw)) and raw[end] in "01234567":
                end += 1
            result.append(int(ch + raw[i:end], 8) & 255)
            i = end
        elif ch == "x":
            match = re.match(r"[0-9a-fA-F]+", raw[i:])
            if not match:
                raise ValueError("invalid C hexadecimal escape")
            result.append(int(match[0], 16) & 255)
            i += len(match[0])
        elif ch in "uU":
            size = 4 if ch == "u" else 8
            digits = raw[i:i + size]
            if len(digits) != size or not re.fullmatch(r"[0-9a-fA-F]+", digits):
                raise ValueError("invalid C Unicode escape")
            result.extend(chr(int(digits, 16)).encode("utf-8"))
            i += size
        else:
            raise ValueError(f"unsupported C escape: \\{ch}")
    return bytes(result)


def tokens(source: str, keep_lines: set[int] | None = None) -> list[Token]:
    """Tokenize strings, identifiers and punctuation; skip comments and chars."""
    found = []
    i = 0
    line = 1
    while i < len(source):
        start = i
        first_line = line
        if source[i].isspace():
            line += source[i] == "\n"
            i += 1
            continue
        if source.startswith("\\\n", i):
            line += 1
            i += 2
            continue
        if source.startswith("//", i):
            end = source.find("\n", i)
            while end >= 0 and source[:end].rstrip("\r").endswith("\\"):
                end = source.find("\n", end + 1)
            i = len(source) if end < 0 else end
            line += source[start:i].count("\n")
            continue
        if source.startswith("/*", i):
            end = source.find("*/", i + 2)
            if end < 0:
                raise ValueError(f"line {line}: unterminated comment")
            i = end + 2
            if (keep_lines is not None
                    and re.fullmatch(r"/\*\s*i18n:keep\s*\*/", source[start:i])
                    and not source[i:source.find("\n", i) if "\n" in source[i:] else len(source)].strip()):
                keep_lines.add(first_line)
            line += source[start:i].count("\n")
            continue
        literal = re.match(r'(?:u8|[uUL])?(["\'])', source[i:])
        if literal:
            quote = literal[1]
            begin = i + len(literal[0])
            i = begin
            while i < len(source):
                if source[i] == "\\":
                    i += 2
                elif source[i] == quote:
                    break
                else:
                    i += 1
            if i >= len(source):
                raise ValueError(f"line {first_line}: unterminated literal")
            raw = source[begin:i]
            i += 1
            line += source[start:i].count("\n")
            found.append(Token("string" if quote == '"' else "char", raw, first_line, line))
        elif source[i].isalpha() or source[i] == "_":
            i += 1
            while i < len(source) and (source[i].isalnum() or source[i] == "_"):
                i += 1
            found.append(Token("id", source[start:i], line, line))
        else:
            found.append(Token("punct", source[i], line, line))
            i += 1
    return found


def scan(path: Path) -> tuple[set[str], list[tuple[int, str]]]:
    source = path.read_text(encoding="utf-8")
    keep_lines: set[int] = set()
    stream = tokens(source, keep_lines)
    wrapped = set()
    marked = set()
    for i, token in enumerate(stream):
        if token.kind != "id" or token.value not in ("T", "N_"):
            continue
        if i + 1 >= len(stream) or stream[i + 1].value != "(":
            continue
        j = i + 2
        first = j
        while j < len(stream) and stream[j].kind == "string":
            j += 1
        if j > first and j < len(stream) and stream[j].value == ")":
            wrapped.add(b"".join(c_string_bytes(t.value) for t in stream[first:j]).decode("utf-8", errors="surrogateescape"))
            marked.update(range(first, j))
    unwrapped = []
    i = 0
    while i < len(stream):
        if stream[i].kind != "string":
            i += 1
            continue
        first = i
        while i < len(stream) and stream[i].kind == "string":
            i += 1
        group = stream[first:i]
        value = b"".join(c_string_bytes(t.value) for t in group).decode("utf-8", errors="surrogateescape")
        if (JAPANESE.search(value) and not all(j in marked for j in range(first, i))
                and not any(line in keep_lines for t in group
                            for line in range(t.line, t.end_line + 1))):
            unwrapped.append((group[0].line, value))
    return wrapped, unwrapped


def tsv_unescape(value: str) -> str:
    def replace(match: re.Match) -> str:
        escape = match[1]
        if escape not in ("t", "n", "\\"):
            raise ValueError(f"unsupported TSV escape: \\{escape}")
        return {"t": "\t", "n": "\n", "\\": "\\"}[escape]
    if len(value) - len(value.rstrip("\\")) & 1:
        raise ValueError("trailing TSV backslash")
    return re.sub(r"\\(.)", replace, value)


def read_translations(root: Path) -> tuple[dict[str, str], list[str]]:
    translations = {}
    errors = []
    for path in sorted((root / "i18n/en").glob("*.tsv")):
        for line, text in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            if not text or text.startswith("#"):
                continue
            try:
                if text.count("\t") != 1:
                    raise ValueError("expected Japanese<TAB>English")
                ja, en = map(tsv_unescape, text.split("\t"))
                if not ja or not en:
                    raise ValueError("empty Japanese or English translation")
                if "\0" in ja or "\0" in en:
                    raise ValueError("embedded NUL in translation")
                if ja in translations and translations[ja] != en:
                    raise ValueError(f"conflicting translation for {ja!r}")
                translations[ja] = en
            except ValueError as error:
                errors.append(f"{path}:{line}: {error}")
    return translations, errors


def formats(value: str) -> list[str]:
    # Literal percent signs (%%) do not consume arguments.
    return [match[0] for match in PRINTF.finditer(value) if match[0] != "%%"]


def c_quote(value: str) -> str:
    # Octal escapes have bounded length, unlike C hexadecimal escapes.
    return '"' + "".join('\\"' if c == '"' else "\\\\" if c == "\\" else
                         f"\\{ord(c) - 0xdc00:03o}" if 0xdc80 <= ord(c) <= 0xdcff else
                         f"\\{ord(c):03o}" if ord(c) < 32 or ord(c) == 127 else c
                         for c in value) + '"'


def generate(translations: dict[str, str], used: set[str]) -> str:
    keys = sorted(used & translations.keys(), key=lambda key: key.encode("utf-8"))
    rows = ["/* Generated by tools/gen_i18n.py; do not edit. */",
            "static const struct { const char *ja, *en; } i18n_en[] = {"]
    rows.extend(f"    {{ {c_quote(key)}, {c_quote(translations[key])} }}," for key in keys)
    if not keys:
        rows.append("    { 0, 0 }, /* ISO C does not allow an empty array. */")
    rows.extend(["};", f"static const unsigned long i18n_en_count = {len(keys)};", ""])
    return "\n".join(rows)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parent.parent,
                        help="FDTP directory containing src/ and i18n/en/")
    parser.add_argument("--output", type=Path, help="generated include (stdout if omitted, except --report)")
    parser.add_argument("--report", action="store_true", help="report unwrapped Japanese; fail if any remain")
    args = parser.parse_args(argv)
    used = set()
    unwrapped = []
    translations, errors = read_translations(args.root)
    for path in sorted(p for p in (args.root / "src").iterdir()
                       if p.suffix in (".c", ".h")):
        try:
            strings, missing = scan(path)
            used.update(strings)
            unwrapped.extend((path, line, text) for line, text in missing)
        except (ValueError, UnicodeError) as error:
            errors.append(f"{path}: {error}")
    for ja in sorted(used):
        if JAPANESE.search(ja) and ja not in translations:
            errors.append(f"missing English translation: {ja!r}")
    for ja, en in sorted(translations.items()):
        if formats(ja) != formats(en):
            errors.append(f"printf format mismatch: {ja!r} {formats(ja)!r} != {en!r} {formats(en)!r}")
        if ja not in used:
            print(f"warning: unused translation: {ja!r}", file=sys.stderr)
    if args.report:
        for path, line, text in unwrapped:
            print(f"{path}:{line}: unwrapped Japanese: {text!r}")
        print(f"Unwrapped Japanese literals: {len(unwrapped)}")
    for error in errors:
        print(f"error: {error}", file=sys.stderr)
    if errors or (args.report and unwrapped):
        return 1
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        content = generate(translations, used)
        if not args.output.exists() or args.output.read_text(encoding="utf-8") != content:
            temporary = None
            try:
                with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8",
                                                 dir=args.output.parent, delete=False) as target:
                    temporary = Path(target.name)
                    target.write(content)
                temporary.replace(args.output)
            finally:
                if temporary is not None:
                    temporary.unlink(missing_ok=True)
    elif not args.report:
        sys.stdout.write(generate(translations, used))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
