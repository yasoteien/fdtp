#!/usr/bin/env python3
# This file is part of FDTP (File & Directory Tool for Pomera).
# SPDX-FileCopyrightText: 2026 yasoteien
# SPDX-License-Identifier: AGPL-3.0-or-later
"""A stand-in for fdtp-dict (src/dict_ui.h) in the tests: made-up entries,
the same calls and output (look, books, atok). $FDTP_DICT_FAKE_BOOKS sets the books that "pass
the gate" (default 15: all four)."""
import os
import sys

MARK, END, LINK = '\x01', '\x02', '\x03'
BOOKS = {'kr': 0, 'mk': 1, 'ej': 2, 'je': 3}
ENTRIES = {
    'mk': {'ことば': [('ことば【言葉】', MARK + '[名]' + END + '\n作り物の説明の一行目。\n二行目。')],
           'かき': [('かき【一】', '一つ目。'), ('かき【二】', '二つ目。'), ('かき【三】', '三つ目。')]},
    'ej': {'apple': [('apple', MARK + '試験' + END + 'の訳。\n' + LINK + '参照' + END)]},
}


def find(book, text, how):
    words = ENTRIES.get(book, {})
    if how == 'exact':
        return text, words.get(text.lower() if text.isascii() else text, [])
    pieces = [text[:n] for n in range(len(text), 0, -1)] if how == 'from' else [text[-n:] for n in range(len(text), 0, -1)]
    for piece in pieces:
        if piece in words:
            return piece, words[piece]
    return text, []


ATOK = {'sports': [('てすとせんしゅ', '試験選手', 5), ('てすとちーむ', 'テストチーム', 8)],
        'anime': [('てすときゃら', '試験キャラ', 5)],
        'entertainment': [('てすとばんど', '試験バンド', 8)],
        'names': [('てすとめい', '試験名', 7), ('てすとせい', '試験姓', 6), ('てすとし', '試験氏', 6)]}


def main():
    books = int(os.environ.get('FDTP_DICT_FAKE_BOOKS', '15'))
    if len(sys.argv) == 3 and sys.argv[1] == 'atok' and sys.argv[2] in ATOK:
        if not books:
            return 2
        sys.stdout.write(''.join('%s\t%s\t%d\n' % row for row in ATOK[sys.argv[2]]))
        return 0
    if sys.argv[1:] == ['books']:
        print(books)
        return 0
    if len(sys.argv) != 6 or sys.argv[1] != 'look' or sys.argv[2] not in BOOKS:
        return 1
    book, how, index, text = sys.argv[2], sys.argv[3], int(sys.argv[4]), sys.argv[5]
    if not books & 1 << BOOKS[book]:
        return 2
    word, entries = find(book, text, how)
    heading, body = entries[index] if index < len(entries) else ('', '')
    sys.stdout.write(f'word\t{word}\ncount\t{len(entries)}\nheading\t{heading}\n\n{body}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
