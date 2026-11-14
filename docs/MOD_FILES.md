# File mods (`files/Fnnnn.BIN`)

A mod can replace any `Fnnnn.BIN` file of the disc, for example an event bank rebuilt by `p2text insert` (see `TRANSLATION_TOOLS.md`). The replacement may be larger or smaller than the original. The guest code is not changed, and the disc image is never written.

## Layout

```
<mods dir>/<mod>/
  manifest.json          optional: {"priority": 10, "serial": ["SLPS-02785", "SLUS-01158"]}
  files/F0057.BIN        replaces disc file F0057 (name: F + 4 digits + .BIN, any case)
```

* `--mods <dir>` turns it on, both in `personarm --boot` and in `personarm_sdl` (the same folder also holds texture mods and gameplay options).
* `serial`: the mod is used only with these discs (punctuation is ignored). Without the key the mod applies to every disc.
* When two mods replace the same file, the higher `priority` wins. On a tie, the first folder name wins. A warning names both mods.
* A mod must contain only the modder's own work. A rebuilt game file is derived from the user's own disc: it is built locally and is never distributed (distribute the translated text or a diff instead).

## How it works

The game finds every file through `FILEPOS.DAT` (LBA 23), a table of `{u32 lba, u32 size}` per file id. At boot it reads the table into RAM, and every whole-file load takes the LBA and size from that RAM copy: `load_file_by_id`, and the field-event loader (`TEXT_SYSTEM.md` TS-013). The redirect has two parts:

1. **Virtual sectors.** At start-up the host reads each mod file and lays the files out one after another from LBA `0x60000` (MSF 87:22:66), past the end of any real CD (80 minutes = 360 000 sectors). `FileRedirect` sits between the CD-ROM controller and the disc: an LBA below `0x60000` reads the disc, and an LBA above it returns a synthesized Mode 2 Form 1 sector (sync, BCD MSF header, data submode, the 2048 bytes of the mod file, zero padding). Sectors of the virtual area outside every file read as zeros.
2. **FILEPOS entry.** A `GamePatch` (`engine/engine.h`) watches the size word of the file's entry in the RAM table. When the game writes it (the CD DMA of `FILEPOS.DAT`), the patch checks that the 8-byte entry still equals the disc's `FILEPOS.DAT` entry. If so, it rewrites the entry to `{virtual lba, mod file size}`. If the table is loaded again, the patch fires again.

Table addresses (from the same constant in `load_file_by_id`):

| Disc | Serial | `load_file_by_id` | FILEPOS in RAM |
|---|---|---|---|
| JP disc 1 (Deluxe Pack) | SLPS-02785 | `0x800272D0` | `0x80083948` |
| US | SLUS-01158 | `0x80027584` | `0x80085068` |

### Why the CD layer

* The guest code is untouched: no PC hooks and no code patches. This keeps the interpreter and the recompiled code identical, because the only new guest-visible effect is a RAM write fired by a store or DMA word, the same mechanism as the gameplay patches.
* The size can change, because the game itself reads `ceil(size / 2048)` sectors from the patched entry.
* Every reader of the entry follows it, including loaders that do not go through `load_file_by_id`: the field-event loader, and the partial loaders that read `FILEPOS.lba + offset` (these keep working as long as the mod file keeps the internal layout they index).
* Serving a whole file from host memory at the `cd_read_start` call would need a PC hook in resident code. That hook is a function boundary that the recompiler would have to exclude, and it would bypass the CD timing and DMA path.

### Guards

* **Serial.** Only discs with a known table address get file mods. On any other disc (for example JP disc 2), the log reads `FILEPOS table address unknown, N file mod(s) skipped` and nothing is installed.
* **Expected bytes.** The patch writes only when the RAM entry equals the disc's own `FILEPOS.DAT` entry. Otherwise it logs `patch 'file Fnnnn (...)' skipped: expected bytes differ` once, and the game reads the disc file.
* Ids beyond the table, ids with an empty entry, unreadable or empty files, and files that do not fit in the virtual area (about 110 MB) are skipped with a log line.

## Cost with no mods

Without `--mods`, or when no file mod applies to the disc, nothing is installed. The CD-ROM reads the disc directly, as before, and no RAM word is watched. Checked on JP disc 1 with `docs/examples/disc1_new_game_to_field.txt` to frame 7000, with a frame dump every 50 frames: the frames and the final RAM are byte-identical to the build before this change, both without `--mods` and with an empty mods folder. The same check on the US disc with `us_new_game_to_field.txt` also matched.

With a mod, the only extra work is one table lookup per CD sector read and one RAM-word check per write to a watched entry.

## Validation (local, real discs)

* JP disc 1, F0057 (shrine, the first dialogue on the new-game path; window at f6181, text from f6506): message 0 was edited to ASCII glyphs. The edited line renders at f6550–f6700, and every frame before the dialogue is identical to an unmodded run. A second edit grew the file by one sector (294 912 to 296 960 bytes, decompressed bank 3 516 to 5 832 bytes). Its text renders, and the end of the grown bank (bytes that are only in the added sector) is present in the bank in RAM at `0x800EC000`.
* The interpreter and `--recomp` (resident module live) give byte-identical frames and RAM for no mods, the same-size edit and the grown edit. `personarm_sdl --recomp --mods` shows the same frame.
* US disc, F0057: the speaker name and the first line were edited. The edit renders at f6700–f6750.

## Limits

* `cd_read_async` loads that compute their LBA without the FILEPOS table would still read the disc. None were seen on the tested path.
* Packs read through descriptors (offset and size inside a file, for example F0056) keep working only if the mod file keeps the same internal offsets.
* Mod files are held in memory (`shortcut:` there is no streaming; a 60 MB pack costs 60 MB of RAM).
* The size of an event bank that fits in RAM is unknown (`TEXT_SYSTEM.md` Q-TS4). The grown bank above ran to f8200 without visible problems.
* Seek timing to the virtual area uses the controller's fixed seek delay, like any other seek.
