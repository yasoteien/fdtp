# 使っている物の許諾

FDTP のソース（`src/` などと、描画層の `lib/rmp-platform/`）は、同じ作者が書いた物で、全体を GNU Affero General Public License v3.0 以降（AGPL-3.0-or-later）で配布します。全文は [LICENSE](LICENSE) です。

`lib/rmp-platform/` は、同じ作者の別の作品と共用している描画・入力の層で、FDTP のビルドに要るためここに含めています。

## このリポジトリに含めている物

| 物 | 場所 | 許諾 |
|---|---|---|
| 東雲フォント（shnm8x16a・shnm8x16r・shnmk16・shnmk12・shnm6x12a） | `data/fonts/` | Public Domain（作者が権利を行使しないと宣言）。Debian の xfonts-shinonome の copyright を `data/fonts/COPYRIGHT.xfonts-shinonome` に同梱 |

アイコン（`data/icons/`）は `tools/gen_icon.py` が描いた物で、FDTP と同じ許諾です。

## ビルド時・実行時に使うライブラリとコマンド

これらはリポジトリには含めていません。各環境の物（Debian のパッケージなど）を使います。

| 物 | 用途 | 許諾 |
|---|---|---|
| MuPDF（libmupdf） | PDF の表示・書き込み、pptx の表示 | AGPL-3.0-or-later（一部 BSD など） |
| jbig2dec（MuPDF が使う） | 〃 | AGPL-3.0-or-later ほか |
| gumbo・MuJS（MuPDF が使う） | 〃 | Apache-2.0・ISC 系 |
| expat | docx・xlsx の XML | MIT |
| zlib | docx・xlsx・zip | zlib |
| libjpeg-turbo・libpng・giflib・libwebp | 画像 | BSD 系・libpng・MIT・BSD |
| uim（libuim） | 日本語入力 | BSD-3-Clause（一部 GPL・LGPL の部分あり） |
| libdrm・Mesa（GBM・EGL・GLES2）・libxcb | 画面 | MIT 系 |
| lhasa | LHa の解凍 | ISC |
| zip・unzip（Info-ZIP） | zip の圧縮・解凍 | Info-ZIP License（BSD 系） |

## 実行ファイルにライブラリを添えて配る時

ビルドした `fdtp` と、ポメラに無いライブラリ（libmupdf など）やコマンドをまとめて配る場合は、各ライブラリの許諾の文書（Debian なら `/usr/share/doc/<パッケージ>/copyright`）を添えてください。AGPL の物（MuPDF・jbig2dec）は、ソースの入手先（Debian のソースパッケージなど）も示してください。このリポジトリのようにソースだけを配り、ライブラリは利用者が Debian から取る形なら、この手間はありません。
