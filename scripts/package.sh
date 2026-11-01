#!/usr/bin/env bash
# Build the downloadable packages for bootcade.
#
#   ./scripts/package.sh              # everything that can be built here
#   ./scripts/package.sh deb tgz      # only the named targets
#
# Targets: deb tgz appimage flatpak
# Output:  dist/
#
# The same script backs the GitHub Actions release job, so what you get locally
# is what users download.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$ROOT/build-package"
DIST="$ROOT/dist"
APP_ID="io.github.battousai90.Bootcade"
ARCH="$(uname -m)"

VERSION="$(sed -n 's/^project(bootcade VERSION \([0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt")"
[ -n "$VERSION" ] || { echo "!! cannot read version from CMakeLists.txt" >&2; exit 1; }

TARGETS=("$@")
[ ${#TARGETS[@]} -eq 0 ] && TARGETS=(deb tgz appimage flatpak)

say()  { printf '\n\033[1;35m==>\033[0m \033[1m%s\033[0m\n' "$*"; }
warn() { printf '\033[1;33m!!\033[0m %s\n' "$*"; }
has()  { command -v "$1" >/dev/null 2>&1; }
wants() { for t in "${TARGETS[@]}"; do [ "$t" = "$1" ] && return 0; done; return 1; }

mkdir -p "$DIST"

# ── A fresh libzip, built once, used by every target ─────────────────────────
# Release builds deliberately run on the oldest supported Ubuntu (glibc
# compatibility : see release.yml), which is exactly why its *packaged* libzip
# is old too: Ubuntu 24.04 still ships 1.7.3, from 2020. That is harmless for the
# .deb/.tar.gz : they link dynamically, so apt/the loader resolves libzip.so.5
# against whatever the *end user's own system* has at install time, not the
# build machine's. It is not harmless for the AppImage: linuxdeploy bundles the
# literal .so the binary was linked against, so without this step the AppImage
# would carry a five-year-old libzip frozen inside it forever.
#
# Same source and checksum as packaging/*.yml's Flatpak module, so all three
# packaging paths agree on one libzip.
LIBZIP_VER=1.11.4
LIBZIP_SHA256=82e9f2f2421f9d7c2466bbc3173cd09595a88ea37db0d559a9d0a2dc60dc722e
DEPS="$ROOT/.deps"
if [ ! -f "$DEPS/lib/pkgconfig/libzip.pc" ] && [ ! -f "$DEPS/lib64/pkgconfig/libzip.pc" ]; then
  say "Building libzip $LIBZIP_VER from source (the system one is likely years old)"
  TMP="$(mktemp -d)"
  if curl -fsSL -o "$TMP/libzip.tar.gz" "https://github.com/nih-at/libzip/releases/download/v$LIBZIP_VER/libzip-$LIBZIP_VER.tar.gz" \
     && echo "$LIBZIP_SHA256  $TMP/libzip.tar.gz" | sha256sum -c - >/dev/null 2>&1
  then
    tar -C "$TMP" -xf "$TMP/libzip.tar.gz"
    cmake -S "$TMP/libzip-$LIBZIP_VER" -B "$TMP/build" \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$DEPS" \
        -DBUILD_EXAMPLES=OFF -DBUILD_DOC=OFF -DBUILD_REGRESS=OFF -DBUILD_TOOLS=OFF >/dev/null
    cmake --build "$TMP/build" -j"$(nproc)" >/dev/null
    cmake --install "$TMP/build" >/dev/null
  else
    warn "could not fetch/verify libzip $LIBZIP_VER : packages will use the system one"
  fi
  rm -rf "$TMP"
fi
export PKG_CONFIG_PATH="$DEPS/lib/pkgconfig:$DEPS/lib64/pkgconfig:${PKG_CONFIG_PATH:-}"
export LD_LIBRARY_PATH="$DEPS/lib:$DEPS/lib64:${LD_LIBRARY_PATH:-}"

# ── Configure & build once; every target consumes this tree ──────────────────
say "Building bootcade $VERSION ($ARCH)"
cmake -S "$ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr >/dev/null
cmake --build "$BUILD" -j"$(nproc)"

# ── .deb and .tar.gz come straight from CPack ────────────────────────────────
if wants deb || wants tgz; then
  say "Packaging with CPack"
  gens=()
  wants deb && gens+=(DEB)
  wants tgz && gens+=(TGZ)
  ( cd "$BUILD" && cpack -G "$(IFS=';'; echo "${gens[*]}")" )
  find "$BUILD" -maxdepth 1 \( -name '*.deb' -o -name '*.tar.gz' \) -exec cp {} "$DIST/" \;
fi

# ── AppImage ─────────────────────────────────────────────────────────────────
# linuxdeploy bundles the binary with every shared library it needs; the GTK
# plugin additionally pulls in the pixbuf loaders, GIO modules and the icon
# theme, without which the app starts but renders nothing.
if wants appimage; then
  say "Building AppImage"
  TOOLS="$ROOT/.cache/appimage-tools"
  mkdir -p "$TOOLS"
  fetch() { # url dest
    [ -x "$2" ] && return 0
    echo "  fetching $(basename "$2")"
    curl -fsSL -o "$2" "$1" && chmod +x "$2"
  }
  BASE="https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous"
  GTKB="https://raw.githubusercontent.com/linuxdeploy/linuxdeploy-plugin-gtk/master"
  if fetch "$BASE/linuxdeploy-$ARCH.AppImage" "$TOOLS/linuxdeploy" &&
     fetch "$GTKB/linuxdeploy-plugin-gtk.sh"  "$TOOLS/linuxdeploy-plugin-gtk.sh"
  then
    APPDIR="$BUILD/AppDir"
    rm -rf "$APPDIR"
    DESTDIR="$APPDIR" cmake --install "$BUILD" >/dev/null

    # Runtime data sits at usr/share/bootcade inside the AppDir, which is
    # exactly the <bin>/../share layout AppContext already probes for.
    PATH="$TOOLS:$PATH" "$TOOLS/linuxdeploy" \
        --appdir "$APPDIR" \
        --plugin gtk \
        --desktop-file "$APPDIR/usr/share/applications/$APP_ID.desktop" \
        --icon-file "$APPDIR/usr/share/icons/hicolor/scalable/apps/$APP_ID.svg" \
        --icon-file "$APPDIR/usr/share/icons/hicolor/256x256/apps/$APP_ID.png" \
        2>&1 | grep -viE 'warning|^\s*$' || true
    # appimagetool refuse une icone de tete qui n'est pas un PNG (.DirIcon) :
    # on la pose nous-memes plutot que de dependre du choix de linuxdeploy.
    rm -f "$APPDIR/$APP_ID.svg" "$APPDIR/.DirIcon"
    cp "$APPDIR/usr/share/icons/hicolor/256x256/apps/$APP_ID.png" "$APPDIR/$APP_ID.png"
    ln -s "$APP_ID.png" "$APPDIR/.DirIcon"
    PATH="$TOOLS:$PATH" "$TOOLS/linuxdeploy" --appdir "$APPDIR" \
        --output appimage 2>&1 | grep -viE 'warning|^\s*$' || true

    out="$(find "$ROOT" "$BUILD" -maxdepth 1 -name '*.AppImage' -newer "$BUILD/CMakeCache.txt" | head -1)"
    if [ -n "$out" ]; then
      mv "$out" "$DIST/bootcade-$VERSION-$ARCH.AppImage"
      chmod +x "$DIST/bootcade-$VERSION-$ARCH.AppImage"
    else
      warn "AppImage was not produced"
    fi
  else
    warn "could not download linuxdeploy : skipping AppImage"
  fi
fi

# ── Flatpak ──────────────────────────────────────────────────────────────────
# Built from the local checkout, so it needs no network for the app itself # only the GNOME runtime, which flatpak fetches once.
if wants flatpak; then
  say "Building Flatpak bundle"
  # Read the runtime out of the manifest so this can never drift from it.
  FP_VER="$(sed -n "s/^runtime-version: *'\([0-9]*\)'.*/\1/p" "$ROOT/packaging/$APP_ID.yml")"

  # flatpak-builder is also published as a Flatpak, which avoids needing root just
  # to produce a package.
  if has flatpak-builder; then
    FPB="flatpak-builder"
  elif flatpak info org.flatpak.Builder >/dev/null 2>&1; then
    FPB="flatpak run org.flatpak.Builder"
  else
    FPB=""
  fi

  if [ -n "$FPB" ]; then
    flatpak install -y --noninteractive flathub \
        "org.gnome.Platform//$FP_VER" "org.gnome.Sdk//$FP_VER" >/dev/null 2>&1 || true
    # State and target must share a filesystem, and rofiles-fuse is unavailable
    # when the builder itself runs inside a Flatpak sandbox.
    # The state dir must sit OUTSIDE the source tree: the manifest's source is the
    # repo itself, so a state dir inside it gets copied into its own build context
    # and flatpak-builder then trips over the directory it is standing in. It still
    # has to share a filesystem with the target, hence a sibling of the repo.
    FP_STATE="$(dirname "$ROOT")/.bootcade-flatpak-state"
    $FPB --force-clean --user --disable-rofiles-fuse --install-deps-from=flathub \
        --state-dir "$FP_STATE" \
        --repo="$ROOT/.fp-repo" "$ROOT/.fp-build" "$ROOT/packaging/$APP_ID.yml"
    flatpak build-bundle "$ROOT/.fp-repo" \
        "$DIST/bootcade-$VERSION-$ARCH.flatpak" "$APP_ID" --runtime-repo=https://flathub.org/repo/flathub.flatpakrepo
    rm -rf "$ROOT/.fp-repo" "$ROOT/.fp-build" "$ROOT/.flatpak-builder"
  else
    warn "no flatpak-builder : skipping Flatpak"
    warn "  flatpak install --user flathub org.flatpak.Builder"
  fi
fi

say "Done : dist/"
ls -lh "$DIST" | awk 'NR>1 {printf "    %-52s %s\n", $9, $5}'

# A checksum file so users can verify what they downloaded.
#
# Ecrit sous un nom temporaire puis renomme : sinon le fichier fait partie du
# motif qui le remplit, et une execution sur un dist/ deja peuple produisait un
# SHA256SUMS vide, publie tel quel. Les erreurs ne sont plus avalees, et un
# resultat vide arrete la construction : un fichier de sommes vide est pire
# qu'absent, il donne l'illusion qu'on peut verifier.
(
  cd "$DIST"
  rm -f SHA256SUMS SHA256SUMS.tmp
  files=$(ls -1 2>/dev/null | grep -v '^SHA256SUMS')
  [ -n "$files" ] || { echo "!! dist/ est vide, aucune somme a calculer" >&2; exit 1; }
  echo "$files" | xargs sha256sum > SHA256SUMS.tmp
  [ -s SHA256SUMS.tmp ] || { echo "!! SHA256SUMS vide" >&2; exit 1; }
  mv SHA256SUMS.tmp SHA256SUMS
)
