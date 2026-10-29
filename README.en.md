[日本語](README.md) | English

# FDTP — File & Directory Tool for Pomera

<p align="center"><img src="docs/images/cover.jpg" alt="FDTP" width="640"></p>

<p align="center">
<b>✨ The stock Pomera software didn't have one, so I made a vertical &amp; horizontal novel editor with ruby! ✨</b><br>
(Narou and Kakuyomu notation, publisher-style formatting included)<br><br>
Oh, and while I was at it: a file manager that shows images, Excel, Word, PowerPoint and PDF too!<br>
And while I was at that: merge, split and mark up PDFs too!<br>
PowerPoint? Press F5 for a slide show!<br>
And English screens, for Pomera DM250US folks! 🎉
</p>

![FDTP on a Pomera DM250](docs/images/pomera-filer.jpg)

FDTP is a file manager for the King Jim Pomera DM250 running Debian 13. Its screens and keys follow FD 3.13, the file manager many people used on the NEC PC-98. It runs on the DRM/KMS console and in an X11 window.

The screens are in English or Japanese:

- On the first start, FDTP asks which language to use and saves the answer in `~/.fdtprc`.
- You can change it later in the customizer (`z`), or with `LANGUAGE=auto|ja|en` in `~/.fdtprc`.
- With `auto`, FDTP follows `LANG`: Japanese for `ja_*`, English for everything else.

## Features

**File manager**
- The internal commands and keys of FD 3.13: copy, move, delete (directories too), rename, attributes, make directory, tree, find, and more. Press `h` or `?` for the list of keys.
- The Pomera's Alt key works. The Alt commands can also be reached with Shift+F1 to F10 and Ctrl+letter.
- Archives: browse, create and extract tar (gz, bz2, xz), zip and LHa.
- Split a file into pieces (Alt+n, 10 MB each by default) and join them again (Alt+h).
- LocalSend (Alt+l): exchange files with an iPhone, Mac and other devices. The other device can also download them in a browser.
- System information (Alt+i): screen output, key devices, memory, disk, battery, the GPU power setting, and more.

**Viewers and editors** (`v` or Enter)
- Text, with a choice of encodings (Shift_JIS, EUC-JP, ISO-2022-JP, UTF-8, UTF-16) and line numbers. Hex view with `b`.
- PDF: view pages, and mark them up with highlight, underline, strikethrough and sticky notes. The marks are saved as standard PDF annotations, so other PDF readers show them too.
- The PDF menu (Alt+p): merge, split, extract, delete, reorder and rotate pages, add or remove a password, compress, PDF to JPEG, and JPEG/PNG to PDF. Everything runs on the Pomera; nothing is sent anywhere.
- docx: edit paragraphs. xlsx and csv: edit cells, with recalculation of common functions. Undo and redo with Ctrl+z and Ctrl+y (100 steps by default).
- pptx: edit the text on the slides, and show them full screen with F5 (Esc to return).
- Images: JPEG, PNG, GIF, BMP, WebP, HEIC (iPhone photos, with libheif), and the PC-98 formats MAG and Pi.

**Novel editor** (`e` or F9)
- A writing editor for Japanese manuscripts. It shows ruby (furigana) and emphasis dots from the notation used by Japanese web novel sites, and writes vertically or horizontally.
- F2 formats the text to a publisher's style, switches between manuscript and web layout, and exports a vertical Word document (.docx).
- Japanese input uses uim (Mozc).
- The editor works the same with English screens; only the screen text changes.

**Screen**
- On the Pomera's 1024x600 screen, FDTP shows 85 columns by 25 rows. On larger screens, use `--size auto` or `--size 160x45`.
- In an X11 window, Alt+Enter or F11 switches to full screen (or start with `--fullscreen`).

```
fdtp [--kms|--x11] [--fullscreen] [--size auto|COLSxROWS] [DIRECTORY]
fdtp -v      # print the version
```

## Screenshots

The novel editor (ruby and emphasis dots) and the F2 conversion menu.

| Horizontal | Vertical | F2 conversion |
|---|---|---|
| ![Horizontal editor](docs/images/novel-horizontal.png) | ![Vertical editor](docs/images/novel-vertical.png) | ![Conversion menu](docs/images/novel-convert.png) |

## Install

### From a release (.deb)

The [Releases](https://github.com/yasoteien/fdtp/releases) page has `.deb` packages for the Pomera (armhf) and for PCs (amd64).

```sh
sudo apt install ./fdtp_1.01-1_armhf.deb   # on a PC: ./fdtp_1.01-1_amd64.deb
sudo fdtp-pomera-setup      # optional: the Pomera settings described below
```

### From the source

```sh
./configure
make -j4
sudo make install           # sudo make uninstall removes it
```

`./configure` checks for the libraries the build needs. If something is missing, it stops and prints the `sudo apt install` line with the Debian package names. It then asks a few questions. Each question comes after a short explanation of what it does and how to undo it.

1. **Language** of the screens: Japanese, English, or auto (follow `LANG`). You can change it later with `z`.
2. **Only on a Pomera DM250**: stabilize Wi-Fi. On Debian 13 for the DM250, the SD card, the internal memory and Wi-Fi share one DMA controller, and a fault there can freeze all three at once. This step turns off Wi-Fi power saving, enables the hardware watchdog, and switches Wi-Fi from DMA to PIO.
   - The PIO change rewrites 1 MiB on the SD card. It works only with the ekesete.net Debian 13 image for the D250. If the data on your card is different, it does nothing.
   - Before writing, it saves the original 1 MiB. To undo: `sudo sh scripts/pomera/pomera_dma_pio.sh restore`.
   - `sudo make install` asks once more before it writes to the card.
   - Battery life gets slightly shorter, and Wi-Fi may be slower. A reboot is needed.
3. **Only on a Pomera DM250**: mount the internal memory at `/mnt/pomera-internal`. Choose read-only (default), read-write, or no mount. If the location is not found, the question is skipped and the previous setting stays. To stop mounting it, run `sudo sh scripts/pomera/pomera_mount_internal.sh unpersist`.
   - If you choose read-write, setup explains the risks and asks again. A file being written when the Pomera freezes may not be recoverable.
   - On boot, if the dirty flag is set, `fsck.fat -n` from `dosfstools` checks the memory without writing. It does not repair it automatically. If a problem is found, the memory is mounted read-only and you are notified at login and when FDTP starts.

Options: `--yes` (take the defaults), `--lang=ja|en|auto`, `--prefix=DIR` (default `/usr/local`), `--no-pomera` (skip the Pomera questions). `sudo make uninstall` does not undo the Pomera settings; it prints how to undo them.

#### Packages for the build

C11 compiler (gcc or clang), GNU make, Python 3 (standard library only, for generated tables and tests), and pkg-config. Libraries (Debian package names):

| For | Packages |
|---|---|
| Screen (DRM/KMS, EGL, GLES2) | libdrm-dev libgbm-dev libegl-dev libgles-dev |
| X11 window (optional; without it, DRM/KMS only) | libxcb1-dev |
| PDF and pptx | libmupdf-dev |
| Images | libjpeg-dev libpng-dev libgif-dev libwebp-dev |
| docx, xlsx, zip | zlib1g-dev libexpat1-dev |
| Japanese input (optional) | libuim-dev |
| LocalSend (optional) | libssl-dev |

`libheif1` and `libheif-plugin-libde265`, for HEIC, are not needed to build (FDTP loads them when it opens a HEIC file).

At run time, archives use the `zip`, `unzip`, `lhasa` and `tar` commands.

#### Cross-building for the Pomera

Building on the Pomera works but is slow. On an amd64 Debian 13 machine:

1. Install `gcc-arm-linux-gnueabihf` and run `dpkg --add-architecture armhf` (needs root).
2. Run `tools/make_sysroot.sh` to collect the armhf libraries into `~/work/sysroot-armhf` (no root needed).
3. `make CROSS=arm-linux-gnueabihf-` builds `build-arm-linux-gnueabihf/fdtp`.

#### Tests

`make check` runs the tests without a screen (`--headless`). It also works on macOS, where `make preview DIR=~ OUT=preview.ppm` writes a screen to an image (Homebrew: `jpeg-turbo libpng giflib webp`). `tests/check_*.py --build build` are the tests for each feature. `check_novel_format.py` compares against manuscripts that are not in this repository and is skipped without them. `check_pdf.py` runs on a real Pomera over ssh.

## Updating

Install the new version over the old one; there is no need to remove it first. Check the current version with `fdtp -v`.

If you installed a .deb, install the new one with `sudo apt install ./fdtp_<new-version>-1_armhf.deb`. If you installed from source, get the new source and run `./configure`, `make -j4` and `sudo make install`.

The program, helper, menu and manual are replaced. Your `~/.fdtprc`, input history and LocalSend certificate stay. Only the language line in `/etc/fdtprc` changes. A Pomera setting is replaced if you choose a different answer, and kept if the question is skipped.

## Battery

FDTP draws with the GPU, so it uses more battery than King Jim's own editor (the Pomera mode). For long writing sessions, consider switching between the two.

The Debian kernel lets the GPU sleep after 0.2 seconds of idle time, which makes the screen respond late after a pause. FDTP handles this in two ways:

- **AWAKE_MINUTES** (default 15, 0 to turn it off): for this many minutes after the last key, FDTP keeps the GPU and the screen awake on the DRM/KMS console. Set it in `~/.fdtprc` or with `z`.
- **The GPU sleep delay** (optional, needs root): to change the 0.2 seconds to 15 minutes, put this line in `/etc/udev/rules.d/91-pomera-gpu.rules` and reboot:
  ```
  ACTION=="add", SUBSYSTEM=="platform", KERNEL=="10090000.gpu", ATTR{power/autosuspend_delay_ms}="900000"
  ```

Alt+i shows the current values of both.

## Launching other programs

FDTP can also serve as a launcher. When it starts another program that draws on DRM/KMS, such as rmp-jukebox (Retro MIDI Player), FDTP gives up the screen and takes it back when the program exits, so the two do not get in each other's way.

- Enter or `v` on `.mid`, `.rcp`, `.r36` and `.g36` files starts rmp-jukebox. Ctrl+p or Alt+r opens any file with it.
- Add your own associations with `launch` lines in `~/.fdtprc`, for example `launch .ext "command %C"` (`%C` is the file).

![rmp-jukebox started from FDTP](docs/images/rmp-jukebox.jpg)

The iOS version is on the App Store as [Retro MIDI Player Mobile](https://apps.apple.com/us/app/retro-midi-player-mobile/id6802171148).

## What has been tested

FDTP has been tested only on the Japanese Pomera DM250 and on PCs.

- **Pomera DM250US** (US model): not tested. Its keyboard is a US layout and the arrow keys are arranged differently. We have not checked whether Alt and the right Shift come from the same device (gpio-keys), or whether the internal memory is in the same place.
- **Pomera DM200**: not tested. It has the same SoC (Rockchip RK3128) and a 1024x600 screen, but we do not know whether a Debian 13 image for it gives DRM/KMS, and it has only 512 MB of memory. `./configure` asks the Pomera questions only on a DM250.
- **Raspberry Pi and other machines** with Debian 13 and DRM/KMS: expected to work, but not tested on real hardware.
- The Wi-Fi PIO change is limited to an SD card made from the ekesete.net Debian 13 image for the D250.

FDTP comes with no warranty, as stated in sections 15 and 16 of the AGPL. If something does not work on your machine, you are welcome to fix it in your own copy.

## Reporting problems

Bug reports in Issues are welcome. Please include:

- The machine (Japanese DM250, DM250US, PC, Raspberry Pi, ...) and the Debian version
- The output of `fdtp -v`
- The screen language, and whether you use DRM/KMS or X11
- The steps to reproduce, what you expected, and what happened
- A screenshot or photo, if you can

**Pull requests are not accepted.** If you want to change FDTP, please fork it and make your own version under the AGPL. The same goes for the English text: if you would like it to read differently, your own version is welcome.

## License

FDTP is licensed under the **GNU Affero General Public License v3.0 or later (AGPL-3.0-or-later)**. See [LICENSE](LICENSE) for the full text. FDTP uses MuPDF and jbig2dec, which are AGPL-3.0-or-later, so FDTP uses a license that can be combined with them.

Third-party components and their licenses are listed in [NOTICE.md](NOTICE.md).

## Name and look

FDTP is a separate work by a different author. It follows the keys and screens of FD (by A. Idei), the file manager for MS-DOS and the PC-98. It is not related to FD or FDclone. No text or messages are copied from FD; all text in FDTP is new.

"Pomera" is a trademark of King Jim Co., Ltd. FDTP is unofficial and is not affiliated with King Jim.

## Acknowledgments

- FD, by A. Idei, which FDTP follows. The FDclone manual page helped with the mapping of internal commands to keys.
- The authors of the Shinonome fonts. Every character on the screen is drawn with them.
- The authors of MuPDF (Artifex), expat, zlib, libjpeg-turbo, libpng, giflib, libwebp, uim, lhasa and Info-ZIP.
- The people who opened the way to running Debian on the Pomera DM250.

## Author

yasoteien
