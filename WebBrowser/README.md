# simplebrowser

A minimal web browser for Ubuntu, built in C with GTK3 + WebKitGTK.

v1 is a single window, single tab, with:
- Address bar (type a URL or search term, press Enter)
- Back / Forward / Reload / Stop buttons
- Window title and address bar that track the current page
- Ctrl+L to focus the address bar

## Install dependencies (Ubuntu)

```bash
sudo apt update
sudo apt install build-essential pkg-config libgtk-3-dev libwebkit2gtk-4.1-dev
```

If `libwebkit2gtk-4.1-dev` isn't found on your Ubuntu version, try:

```bash
sudo apt install libwebkit2gtk-4.0-dev
```

and change `webkit2gtk-4.1` to `webkit2gtk-4.0` in the Makefile's `PKGS` line.

## Build and run

```bash
make
make run
```

or just:

```bash
make
./simplebrowser
```

## Building a portable AppImage

An AppImage bundles the app plus its GTK/WebKit libraries into a single
file that runs on most Linux distros without an install step.

```bash
chmod +x build-appimage.sh
./build-appimage.sh
```

This downloads `linuxdeploy` and its GTK plugin (needs internet access,
first run only — they're cached in `appimage-tools/`), builds the app,
and produces `simplebrowser-x86_64.AppImage`.

Run it anywhere:

```bash
chmod +x simplebrowser-x86_64.AppImage
./simplebrowser-x86_64.AppImage
```

**Notes on portability:**
- The AppImage bundles GTK3 and WebKitGTK, so the *target* machine
  doesn't need those installed — but it still needs a roughly modern
  glibc and a working X11/Wayland session (true for any current
  desktop Linux).
- WebKitGTK pulls in a lot of shared libraries (media codecs, graphics
  drivers). `linuxdeploy` bundles what your build machine's binary
  actually links against, so build on a fairly standard Ubuntu install
  to get broad compatibility, and test the resulting AppImage on a
  different machine/distro before relying on it.
- If you'd rather distribute via Flatpak instead (sandboxed,
  auto-updatable, needs Flatpak installed on the target), let me know
  and we can set up a Flatpak manifest instead — it's a different
  packaging model, not a drop-in replacement for this script.

## Project layout

```
simplebrowser/
├── main.c               # all application logic (v1: one file)
├── Makefile
├── simplebrowser.desktop  # app launcher metadata (used by the AppImage)
├── simplebrowser.png      # app icon (used by the AppImage)
├── build-appimage.sh       # packages everything into a portable AppImage
└── README.md
```

## Planned next steps

1. **Tabs** — wrap the single `WebKitWebView` in a `GtkNotebook`, add
   "New Tab" (Ctrl+T) and "Close Tab" (Ctrl+W)
2. **Bookmarks** — save/load a simple JSON file of `{title, url}`
3. **History** — log visited URLs to a local SQLite database
   (`libsqlite3-dev`)
4. **Settings** — homepage and default search engine, stored in a
   small config file (e.g. `~/.config/simplebrowser/config.ini`)
5. **Find in page** — hook up `webkit_find_controller`
6. **Downloads** — handle WebKit's `download-started` signal, show
   progress

We'll build these one at a time on top of this base.
