# Soundbanks

A browsable, deduplicated database of individual **Tones** (the actual sounds - "AC PIANO",
"Metal Harp", not Patches, which only combine/arrange existing Tones across parts), built by
scanning a folder of Roland SysEx dumps. A tab in the desktop extended editor (**SOUNDBANKS**);
a full-screen view on Android, reached from the hamburger menu.

The database itself lives in a fixed location and is never rescanned automatically - only an
explicit **RESCAN** touches the source files, so opening the tab is always instant regardless of
how large the underlying library is.

## Building the database

Point it at your own personal library of `.syx`/`.mid`/`.smf` files (or a `.zip` of them - see
below), then hit **RESCAN**. A folder is scanned recursively, so a library organised into
subfolders (by device, pack, or artist) works as-is. Rescanning is incremental: files already
in the database are skipped, so re-running it after adding new files to the source is cheap.

Two ways to pick a source, usable together:

- **A folder** - scanned in place, in full, every RESCAN. The obvious choice for an existing,
  organised library.
- **Individual files, or a single `.zip`** - copied into a small internal staging area that
  RESCAN always scans in addition to the folder above, so picking a loose file or a `.zip`
  never disturbs an already-configured folder-based library.

**Desktop**: **CHOOSE FILES/FOLDER...** in the Soundbanks tab handles both cases in one dialog -
pick a folder to scan it in place, or pick one or more files/a `.zip` to add them to the staging
area. (The Utility tab's own **SOUNDBANKS FOLDER** picker still exists too, folder-only, for
setting the same source folder.)

**Android**: hamburger menu -> **Choose soundbank files...** - multi-select individual files or
a single `.zip` (Android can't reliably list a picked folder's contents, so there's no direct
folder picker there - a `.zip` is the practical stand-in for "a whole folder").

Same-name duplicates are merged when their bytes are identical; when they differ, later ones are
disambiguated as `Name (1)`, `Name (2)`, etc.

## Browsing

An alphabetical letter/digit strip on the left (with a count per letter), a multi-column list on
the right that flows into as many columns as the window is wide. Scroll with the mouse wheel, a
real scrollbar (drag the thumb or click the track to page), touch-drag on the list itself
(Android), or the arrow keys (Up/Down move by one row of the grid, Left/Right by one entry,
Page Up/Down by a screenful) once a tone is selected.

The search box searches every letter at once, not just the one currently selected, and is
click-to-activate so it never steals keyboard focus from the on-screen keyboard by accident.

## Using a tone

- **Double-click** (or, on a chosen tone, the on-screen **PART** row above the list) - plays it
  immediately on the selected Part's live/working tone. Free to audition as much as you like;
  no Tone Memory slot is spent.
- **Right-click** (desktop) / **long-press** (Android) a tone for:
  - **Add/Remove Favorites** - a separate, hand-picked list (see below).
  - **Send to** - audition on any of the 8 Parts, not just the currently selected one.
  - **Inject to slot...** - write it permanently into one of the 64 internal Tone Memory slots
    (with a confirmation naming whatever it would overwrite).
  - **Send to real D-110** - the same two choices (a Part, for instant audition, or a Tone
    Memory slot, permanent), but sent out a real MIDI Out port to actual connected hardware
    instead of this emulator. Needs a MIDI Out device selected first (hamburger/Options ->
    **MIDI Output**). Slot names can't be shown for a real remote unit the way they can for this
    emulator's own RAM.

## Favorites

A separate, hand-picked list, independent of the scanned database and never touched by RESCAN -
its own **FAVORITES** entry in the letter strip. **EXPORT FAVORITES...** writes them out as
standalone Roland SysEx bank file(s), sorted alphabetically: everything in one file if there are
64 or fewer; beyond that, split into one file per 64 (a complete internal Tone Memory bank each),
named `<chosen name>_01.syx`, `_02.syx`, and so on.
