# Mac setup (local mods)

Files that live outside the source tree, kept here so they are not lost:

- `build.sh` - safe build: one build at a time, copies the binary to `game/` with rm + cp + codesign. Copy to the project root.
- `config.ini` - goes in `game/` (pgxpZbuffer=1, viewCells=41, ...).
- `gunshot.wav` - goes in `game/mod/`.
- `app/` - contents of `Driver 2.app` (launcher script, Info.plist, icon).
- `extract_levels.py` - the far city (draw distance beyond the loaded regions) reads its own copy of the level files from `game/mod/DRIVER2/LEVELS` and `NLEVELS`. Recreate them from the CD image (see the comment at the top of the script). Without them the game runs as before, only without the far city.
- `CLAUDE.md` - instructions for Claude sessions in this folder; copy to the project root (`~/Projects/Games/Driver 2/`).
