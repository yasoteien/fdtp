# Changelog

All notable changes to FDTP are recorded in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/). Versions have two decimal places (0.01, 0.02, ... 1.00); they do not follow Semantic Versioning.

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
