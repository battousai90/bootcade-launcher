# Bootcade

🎮 A lightweight, native launcher and ROM manager for **FinalBurn Neo** and
**MAME**, on **Linux** and **Windows**.
Built with **C++17** and **GTKmm 3.0**, designed to be fast, simple, and desktop-friendly.

No more terminal commands. Scan your ROMs, fix what is broken, pick a game, and play.

**[Website](https://bootcade.netlify.app)** ·
**[Download](https://github.com/battousai90/bootcade-launcher/releases/latest)**

---

## 📦 Install

Grab a package from the [latest release](https://github.com/battousai90/bootcade-launcher/releases/latest):

| Format | For | Notes |
|---|---|---|
| **`.AppImage`** | any Linux distribution | Portable, nothing to install. `chmod +x` then run. **Recommended.** |
| **`.deb`** | Debian / Ubuntu | `sudo apt install ./bootcade_*.deb` |
| **`.flatpak`** | any Linux distribution | `flatpak install bootcade.flatpak` |
| **`.tar.gz`** | any Linux distribution | Extract anywhere; system libraries required |
| **`windows-x64.zip`** | Windows 11 | Portable folder: extract anywhere and run `bootcade.exe` |

Each release ships a `SHA256SUMS` file: `sha256sum -c SHA256SUMS`.

You also need an emulator. Bootcade can download it for you (see
[Emulators](#emulators)), so there is nothing else to install by hand.

### Staying up to date

Bootcade tells you when a new release exists. On Linux, **Update** does the rest in
one click: it downloads the right file for the way Bootcade was installed
(AppImage, Flatpak, `.deb` or archive), checks it against the release digests, installs
it and restarts. Emulators and DAT files have their own update buttons.

---

## ✨ Features

### Library
- 🕹️ Browse and launch your collection in a **list** or **cover grid** view
- 🔍 Instant search across titles, ROM names, manufacturers and years
- ⭐ Mark games as favorites and filter to them from the header
- 🎲 **Random play** : let Bootcade pick a game, from any emulator or from one only
- 🧩 **Stackable filters** : combine system, type, year, manufacturer, status…
  (e.g. *Arcade + Original*, *SNES + Homebrew*), with active filters shown as
  removable chips
- 🏷️ **Type filter** : tell real games apart from derivatives: Original, Clone,
  Hack, Homebrew, Bootleg, Prototype
- 📊 Sidebar filters by system, manufacturer, year, source, aspect ratio,
  orientation, ROM status, Highscore and Achievements
- 🧱 One tab per emulator, and a combined view that interleaves both catalogues
  alphabetically
- ⚡ Rows are built lazily as you scroll, and long lists are paged, so a 25k-game
  library opens instantly
- 🕒 Play time and last-played tracking, and a choice of which game is shown at
  startup (last played, most played, best score…)

### Game details
A details panel (movable, in four tabs) with the game's information, its ROM files
and what each one is for, the artwork, your records and, when available, its
achievements. Games that cannot start yet are shown as **incomplete** and the panel
names exactly what is missing: a BIOS, a parent set, a device.

### Emulators
- 🔥 **FinalBurn Neo** : the Bootcade build, or the official upstream release, side by side
- 🅼 **MAME** : the official monthly release, or **Sooner**, the in-development
  build rebuilt every day, for the machines and fixes added since the last release
- 📥 Download, install and update each emulator from Settings; the matching DAT
  files follow your choice
- 🧭 A guided first launch walks you through picking or downloading an emulator, so
  a fresh install starts cleanly with a ready game list
- 🖥️ FinalBurn Neo display options (fullscreen, integer scale, scaling…) from Settings

### ROM Manager
- 📀 **Audit with CRC verification** : every set is checked against its DAT and the
  window says which set is broken and which file is at fault (missing, wrong CRC,
  extra file, misnamed set, missing BIOS, parent or CHD)
- 🗂️ **DAT groups** : each DAT has its own folders, its own emulator and its own
  merge mode (split, merged, non-merged…), the way a dedicated ROM manager
  handles them. Bootcade checks a set against the layout of *its* DAT
- 🔧 **Fix** what can be fixed in place, and tick every row an action can act on
- 📤 **Inbox → Outbox rebuilder** : repairs incomplete sets from pieces found in your
  downloads and in your existing library, then **Move to library** when you are
  happy. The library itself is never written to until you say so, and nothing is
  ever deleted
- 📦 Reads zip, 7z, rar and tar archives; always writes clean ZIP sets
- 💿 **CHD** support for MAME: import, verify and move disc images, laid out the way
  the DAT says
- ⬇️ **Downloads tab** : fetch missing sets straight from the sources listed in the
  window, for FinalBurn Neo and MAME. A CRC database published by the Bootcade
  server lets Bootcade offer only the sets that really match your DAT, so no
  download quota is spent on the wrong file. Every download is verified against
  the DAT, and a summary stays on screen afterwards
- 🗃️ Local **SQLite** database : incremental rescans only re-read what changed
  on disk
- 🔄 **DAT updates** judge only what changed, for one emulator at a time; a MAME
  `-listxml` file can be read directly
- 📁 A **Bootcade folder** is created on first launch with sensible subfolders, and
  you can move your workspace anywhere
- 📤 Export your game list

> Bootcade does not host any ROM. It checks what you download against your DAT and
> files it for you; you are responsible for the use of what you download.

### Launching and controllers
- 🚀 Launch games straight into the emulator, with joystick support always enabled
- 🎮 **Controller configuration** with reusable profiles, up to 16 buttons, for
  FinalBurn Neo and MAME alike
- 🔌 Controllers are recognised by name and followed as you plug them in: each
  player gets the buttons of their own pad at launch, with no ghost entries, and
  player 1 gets the keyboard back when the saved pad is not connected
- 🕹️ **Analog controls**: sticks, paddles, dials and trackballs can be bound to a
  stick, or to two separate inputs (two triggers, two buttons or two keys), with a
  per-game **sensitivity** setting
- ⌨️ Keyboard bindings and per-game layouts

### Scores and achievements
- 🏆 **Highscores and leaderboards**: with a free Bootcade account, your records on
  supported games are read from the emulator and published; scores made while
  signed out are kept and sent when you sign in. The Records tab of the details
  panel shows where you stand, and an in-game report can be sent for games whose
  score is not yet understood
- 🎖️ **RetroAchievements**: link your account to see a game's achievements in the
  details panel, filter on them, and use them in Random play
- ✋ Everything score-related is opt-in, with one switch in Settings

### Artwork
- 🖼️ Preview and title images are fetched automatically, from the Bootcade
  artwork server first and the libretro thumbnails after it, for FinalBurn Neo and
  MAME
- 📸 Press **F6** in the emulator and Bootcade offers to use that capture as the
  game's title or preview image

### App
- 🌍 Available in English, French, Spanish, German, Portuguese, Japanese, Chinese
  and Thai
- 🌑 Dark and light themes, drawn by Bootcade's own style so it looks the same on
  every distribution and desktop
- 🔎 Windows and lists adapt to the screen size, from small laptops to 1080p and
  beyond, and never open smaller than their content
- 🐧 Native on Linux : no Electron, no web bloat
- 🪟 Also available on Windows as a portable build, same interface and same library
  format

Perfect for retro enthusiasts who want a no-nonsense frontend.

---

## 🚀 First run

1. Bootcade offers to create its **Bootcade folder** (or lets you choose another)
   and sets up default locations inside it
2. Pick an emulator: choose **Download** for the Bootcade build of FinalBurn Neo or
   for MAME, or point Bootcade at one you already have in **Settings**
3. Add your **ROM directories** if you already have a collection
4. Get the **game list**: Bootcade downloads the DAT files that describe every
   known set and their CRCs, which are what the scanner checks your ROMs against
   (you can also generate them from **Emulator → Generate DAT Files**)
5. Hit **Scan ROMs**, then open the **ROM Manager** to see what is complete, what
   is broken and what can be downloaded

Your library, settings and cache live in `~/.config/bootcade/` on Linux and in
`%APPDATA%\bootcade` on Windows. Set `BOOTCADE_CONFIG_DIR` to use another location,
for instance to try a build without touching your real library.

Rescans are incremental: only directories whose contents actually changed are
re-read, so a rebuilt ROM set is picked up without a full re-scan of the
collection.

---

## 🛠️ Build & Run

### 1. Install dependencies (Ubuntu/Debian)

```bash
sudo apt update
sudo apt install -y build-essential g++ make cmake pkg-config \
  libgtkmm-3.0-dev libpugixml-dev nlohmann-json3-dev \
  libcurl4-openssl-dev libssl-dev libzip-dev libarchive-dev zlib1g-dev libsqlite3-dev
```

### 2. Build

```bash
cmake -B build
cmake --build build -j$(nproc)
```

### 3. Run

```bash
./build/bootcade
```

The build copies `assets/` and `locale/` next to the binary, so run it from
`build/` (or ship that directory as a whole). An installed build finds them in
`<prefix>/share/bootcade` instead; `BOOTCADE_DATA_DIR` overrides both.

### 4. Build the packages (optional)

```bash
./scripts/package.sh                 # everything it can build here
./scripts/package.sh deb tgz         # or just the ones you want
```

Artifacts land in `dist/`. Targets are `deb`, `tgz`, `appimage` and `flatpak`;
the last two need `curl` and `flatpak-builder` respectively, and are skipped with
a warning when unavailable.

This is the same script the release workflow runs, so what you build locally is
what users download. A release is published by the release workflow, which bumps
the version, tags it and builds every package, including the Windows zip.

### Windows

The Windows build is made with MSYS2 (UCRT64) by the `windows.yml` and `release.yml`
workflows, which assemble the portable folder (the executable, its libraries, the
assets and the translations) and zip it. Building it by hand follows the same
steps: install the MSYS2 UCRT64 toolchain and the packages listed in the workflow,
then `cmake -B build -G Ninja && cmake --build build`.

### Testing the Flatpak locally

`flatpak-builder` is itself available as a Flatpak, so no root is needed:

```bash
flatpak install --user flathub org.flatpak.Builder
./scripts/package.sh flatpak          # produces dist/*.flatpak
flatpak install --user dist/bootcade-*.flatpak
flatpak run io.github.battousai90.Bootcade
```

The first build is long: `org.gnome.Platform` ships GTK 3 but not the gtkmm C++
bindings, so libsigc++, glibmm, cairomm, pangomm, atkmm and gtkmm are compiled from
source. Later builds reuse the cache in `.flatpak-builder/`.

To uninstall: `flatpak uninstall --user io.github.battousai90.Bootcade`.
