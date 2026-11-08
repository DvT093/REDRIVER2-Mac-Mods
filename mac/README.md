# Mac setup (local mods)

Files that live outside the source tree, kept here so they are not lost:

- `build.sh` - safe build: one build at a time, copies the binary to `game/` with rm + cp + codesign. Copy to the project root.
- `config.ini` - goes in `game/` (pgxpZbuffer=1, viewCells=41, ...).
- `gunshot.wav` - goes in `game/mod/`.
- `app/` - contents of `Driver 2.app` (launcher script, Info.plist, icon).
