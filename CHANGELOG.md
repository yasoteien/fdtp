# Changelog

All notable changes to FDTP are recorded in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/). Versions have two decimal places (0.01, 0.02, ... 1.00); they do not follow Semantic Versioning.

## [1.18] - 2026-11-14

### Added

- Pomera DM250: the stock electronic dictionaries (Kadokawa thesaurus, Meikyo Japanese, Genius English-Japanese and Japanese-English) in the novel editor. Ctrl+j looks up the selection or the word at the cursor, F3 a typed word; the result is a floating box with the four books (← → or Tab), the entries of the word (Shift+← →), pronunciations and the thesaurus's classes. The books are read from the Pomera's own internal memory, mounted read-only, by `fdtp-dict`, which comes with the armhf .deb only (binary only, not under the AGPL; it works only on a Pomera that has the dictionaries). `fdtp-pomera-setup` asks whether to mount them.
- The proper nouns of ATOK's sports, anime and entertainment dictionaries go into the user's Mozc user dictionary (`scripts/pomera/pomera_atok_mozc.py`; the personal names dictionary is added switched off). The user's own Mozc dictionaries are kept.
- Novel editor: the first save after opening a file keeps the previous file in `~/.fdtp/backup/` (3 versions).
- The user manual: 2.4, 6.6, 6.7, 10.9 and 10.10.

### Fixed

- Japanese input: on the second and later pages, the candidate list showed the first page's words.
- Shift_JIS text: ～ ∥ － ￠ ￡ ￢ ― (as Windows reads them) were shown as 〓.

## [1.12] - 2026-10-29

### Added

- Optional PD hub charging service for the Pomera DM250 with an RK818 charger: raises a low USB input limit to 2 A and optionally controls the charging LED. Configure and Pomera setup offer installation or removal.

## [1.11] - 2026-10-29

### Fixed

- The source on GitHub builds again: the public `.gitignore` left out `mk/config.mk` (it meant only the top-level `config.mk`). The 1.10 .deb files were not affected.
- The file list follows changes made elsewhere (another window, Finder, LocalSend, a shell): the shown directories are checked every 1.5 seconds and read again when they change, keeping the cursor and the marks of what is still there.
- Total, Used and Free on the top line use one unit.

### Changed

- Give by browser answers a too long file reference with 403, as receiving does.

## [1.10] - 2026-10-29

### Added

- LocalSend (Alt+l): send files to, and receive files from, the LocalSend app on an iPhone, iPad, Mac, Windows or Android on the same network. Marked files go together; directories go as zip. Receiving makes a new PIN each time and asks before each sender's files. The helper `fdtp-localsend` (C and OpenSSL) does the network part.
- Give by browser (Alt+l, B): a phone or PC without LocalSend downloads the files with its browser after the PIN.
- HEIC (iPhone photos) in the image viewer, with libheif loaded when a HEIC file is opened (`libheif1`, `libheif-plugin-libde265`; not needed to build).
- The internal memory of the Pomera DM250 can be mounted read-write: `./configure` asks read-only, read-write or none. Read-write checks the file system (read only) after a boot that followed a stop, mounts read-only when it finds damage, and tells at login and when FDTP starts. Nothing is repaired automatically.
- The user manual: LocalSend (4.7), updating (2.5), and the internal memory choices.

### Changed

- `./configure` and `fdtp-pomera-setup` remove an earlier internal memory mount when the new answer differs.
- Images open a little faster (the scaling was made faster).

### Fixed

- `tools/make_sysroot.sh` no longer stops without a word when no SYSROOT is given.

## [1.01] - 2026-10-25

### Fixed

- Running rmp-jukebox with `x` (execute) no longer ends at once: it is started without a terminal around it, as when a song is opened.

## [1.00] - 2026-10-24

The first public release.

### Added

- A file manager that follows the screens and keys of FD 3.13, on the DRM/KMS console and in an X11 window.
- English and Japanese screens, chosen on the first start, with `z`, or with `LANGUAGE` in `~/.fdtprc`.
- File operations, including deleting directories, and tar, zip and LHa archives.
- File split (Alt+n) and join (Alt+h).
- Viewers for text (with encodings), hex, PDF, docx, xlsx, csv, pptx and images (including MAG and Pi).
- PDF annotations: highlight, underline, strikethrough and sticky notes.
- The PDF menu (Alt+p): merge, split, extract, delete, reorder and rotate pages, passwords, compression, and conversion between PDF and JPEG/PNG.
- Editing of docx paragraphs, xlsx and csv cells, and pptx text, with undo and redo.
- pptx slide show (F5).
- A novel editor for Japanese manuscripts: ruby and emphasis dots, vertical and horizontal writing, publisher formatting, and export to a vertical Word document.
- System information (Alt+i).
- `AWAKE_MINUTES`, which keeps the screen responsive after a pause on the DRM/KMS console.
- Launching other DRM/KMS programs such as rmp-jukebox, with the screen handed over while they run.
- `./configure`, `make install` and `make uninstall`, with optional Pomera DM250 settings (Wi-Fi stabilization and a read-only mount of the internal memory).
