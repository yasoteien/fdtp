# FDTP 英訳用語集

米国英語を使い、操作名は短い命令形にする。画面のラベルは先頭を大文字にする。
共有する原文は `en/common.tsv` を参照し、各ファイルの TSV に重ねて書かない。
printf の書式指定子は数・種類・順番を維持し、括弧・区切りなどは ASCII にする。
変数名、キー名、ファイル名、パスは変えない。日本語を解析するための文字列など、
データとしての日本語は、共通表に同じ原文があっても `T()` で包まない。

| 日本語 | 英語 | 用途・補足 |
| --- | --- | --- |
| ファイル | file | 操作対象 |
| ディレクトリ | directory | folder と混在させない |
| マーク | mark | 選択用の印 |
| 書庫・アーカイブ | archive | 圧縮ファイル |
| 属性 | attributes | 短い操作ラベルは Attr |
| 削除 | delete | 操作名 Delete |
| 移動 | move | 操作名 Move |
| 複写・コピー | copy | 操作名 Copy |
| 名前の変更 | rename | 操作名 Rename |
| ディレクトリ作成 | make directory | 短い操作ラベルは Mkdir |
| ツリー | tree | ディレクトリ階層 |
| 検索 | find | 操作名 Find |
| 検索文字 | search text | 入力する検索語 |
| 検索表示 | find results | 検索結果の表示 |
| 情報 | info | 短い操作ラベルは Info |
| 実行 | execute | 短い操作ラベルは Exec |
| ログディレクトリ | log directory | 短い操作ラベルは Logdir |
| シェル | shell | 操作名 Shell |
| 開く | open | 操作名 Open |
| 表示 | view | 操作名 View |
| 保存 | save | 操作名 Save、完了は Saved |
| 終了 | quit | 操作名 Quit |
| 戻る | back | 直前の画面へ戻る |
| キャンセル・中止 | cancel | 米国式 canceled |
| 取り消し | undo | 編集の取り消し。完了は Undone |
| やり直し | redo | 編集の再適用。完了は Redone |
| 選択 | select | 操作名 Select |
| 決定 | OK | 選択・入力の確定 |
| 並べ替え | sort | 操作名 Sort |
| 圧縮 | compress | 操作名 Compress。書庫作成は Pack |
| 解凍・取り出し | extract | 書庫の操作ラベルは Unpack |
| 分割 | split | 操作名 Split |
| 結合・連結 | join | 操作名 Join |
| 名前 | name | ファイルなどの名前 |
| 拡張子 | extension | ファイルの拡張子 |
| サイズ | size | ファイルサイズなど |
| 種類 | type | ファイルなどの種類 |
| 移動先・行先 | destination | コピー・移動の行先 |
| 文字コード | encoding | 文字エンコーディング |
| 自動 | auto | ラベルは Auto |
| 履歴 | history | 入力履歴・編集履歴 |
| 変更あり | modified | 状態ラベルは Modified |
| ページ | page | 文書・PDF |
| スライド | slide | プレゼンテーション |
| シート | sheet | 表計算 |
| セル | cell | 表計算 |
| 段落 | paragraph | 文書・スライドの段落 |
| 見出し | heading | 文書・一覧の見出し |
| 表 | table | 文書内の表 |
| 画像 | image | 文書内画像・画像ファイル |
| 図 | figure | 文書内の図 |
| ルビ | ruby | 日本語原稿の読み仮名 |
| 傍点 | emphasis dots | 日本語原稿の強調点 |
| 縦書き | vertical writing | 原稿の組み方向 |
| 出版社の書式 | publisher's format | 原稿の書式 |
| 全体 | fit | 全体が収まる表示。ラベルは Fit |
| 先頭へ | top | 操作ラベルは Top |
| 次 | next | 操作ラベルは Next |
| 前 | previous | 短い操作ラベルは Prev |
| システム情報 | system info | 画面名は System info |

`取消` はディレクトリ選択ではキャンセル、スライドでは編集の取り消しに使われる。
共通表では短い `Cancel` に統一する。新しい文言では編集操作を `Undo` とする。
