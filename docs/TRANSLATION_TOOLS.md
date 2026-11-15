# Translation tools (`p2text`)

`p2text` extracts the dialogue of Persona 2: Eternal Punishment (JP) from **your own** disc image into editable text files, and writes translated event files back. Format background: `docs/research/TEXT_SYSTEM.md`.

## Legal note

This repository contains no game text, glyph images or game files, and `p2text` never ships any. Everything it produces (TSV files, font sheets, patched files) is generated locally from your disc and must stay on your machine. A translation project should distribute **only the translator's own text** (the edited TSV column or a diff of it), never original or patched game files.

## Build

```
cmake -S . -B build -DBUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --target p2text unit_test_p2text
ctest --test-dir build -C Release -R p2text
```

## How the text is stored (short)

Dialogue is not Shift-JIS. It is a sequence of `u16` codes: values below `0x1000` are glyph indices into the game's own 12x12 font (in the game's own order; `0x20`-`0x7E` match ASCII), and `0x1Lxx` are control codes occupying `L` `u16` (`<br>` = `0x1101`, `<color=nn>` = `0x122E nn`, `<indent>` = `0x1131`, `<page>` = `0x1102`, `<end>` = `0x1103`). Messages live in the last section of an LZ-compressed `0x201`/`0x101` chunk (the "event bank") of field-event files and are referenced from the bank's bytecode (op 19) by offset. The tool compresses with the same LZ format (window 256, matches 3-130).

## Commands

```
p2text extract (--disc <img.cue|bin|iso> [--index N] | --file <F0057.bin>) --out <dir> [--map glyphs.txt] [--us|--jp]
p2text insert  --file <F0057.bin> --tsv <F0057.tsv> --out <patched.bin> [--map glyphs.txt] [--recompress] [--us|--jp] [--disc <img> | --exe <SLUS_011.58>] [--strict]
p2text verify  (--disc <img> [--index N] | --file <F.bin> [--exe <SLUS_011.58>])
p2text font-sheet (--disc <img> | --font <F0041.bin>) --out sheet.bmp [--first N] [--count N]
```

`--disc` scans every `Fnnnn` file up to 3 MB. Nothing is ever written to the disc image.

### extract

Writes `<dir>/<Fnnnn>.tsv`, one per file with banks. Columns (tab separated): `bank` (chunk offset in the file), `msg` (index), `offset` (inside the message section), `codes` (original `u16` codes in hex), `text`. Edit **only the `text` column**.

Text syntax:

| Tag | Meaning |
|---|---|
| `<br>`, `<indent>`, `<page>`, `<end>` | newline, indent, page wait, end of message |
| `<color=11>` | text color (decimal) |
| `<gNNNN>` | glyph by decimal index (used for glyphs without a mapping, and for `<` itself) |
| `<c:1331 0001 0002>` | any other control: hex words, the first word `0x1Lxx` with `L` words in total |

Every message must end with exactly one `<end>`.

### US release (SLUS-01158): packed ASCII text

The US build stores English text packed: control `0x1136`, then `u16` words that each hold two ASCII bytes (low byte first). The run ends at the first NUL byte: an odd-length string ends in a word whose high byte is 0, an even-length string is followed by a `0x0000` word. Names, `<color=N>`, `<br>`, `<page>`, `<end>` and the other controls stay separate `u16` codes between runs, so one message has several runs. No other control packs text. `p2text` detects the format per file (any `0x1136` in a bank), so `extract` writes plain English and `insert` packs it again; `--us` / `--jp` force the mode (needed only to add text to a US file that has no packed run yet). The TSV header says `us-packed`.

* `<` in text is written `<lt>`. Only printable ASCII fits; anything else is an error.
* A run that is not canonical (empty, a control byte inside, a NUL in the wrong half) is kept as raw `<c:1136><c:xxxx>...` tokens. In a packed file a bare glyph index below `0x1000` (some debug messages) renders as `<gNNNN>`, not as a character.
* The US font (`F0041`/`F0013` chunk 0) is 8x12, 1 bpp, ASCII order from `0x20`, proportional. See "Window limits and overflow warnings" below for the widths.
* Check on a real US disc: `verify --disc <us.cue>` gives 0 failures over 11 020 messages; the JP disc still gives 0 failures over 11 385. Edited F0057 message 0 as plain text, served it with `--mods` (`MOD_FILES.md`), and the line shows in the shrine dialogue (frame ~6750-6800).

### Glyph mapping

The font order is the game's own, and kanji/kana cannot be derived automatically. The tool seeds only the identity range `0x20`-`0x7E`; every other glyph renders as `<gNNNN>`. To read and translate the text you provide a mapping file: one line per glyph, `id<TAB>character` (id decimal or `0xHEX`, exactly one UTF-8 character, `#` comments allowed). Later lines override earlier ones; if two glyphs map to the same character the lowest id is used when inserting.

`font-sheet` writes a BMP of the glyphs `first..first+count` (32 per row, id = `first + row*32 + column`) from your own font data, to help you build that mapping by eye. Keep it local.

### insert

Re-reads the original file, applies the rows whose `text` changed (unchanged rows keep the original bytes, so unmapped glyphs survive), rebuilds the message section, updates every op-19 operand, recompresses the chunk and writes a patched file. Any character without a glyph rejects the whole run with a list of `file:line: msg N: U+XXXX 'c' sin glifo` and writes nothing. It prints the size change of the messages, the decompressed bank, the chunk and the file.

* If nothing changed, the output is byte-identical to the input. `--recompress` forces recompression (our compressor is not bit-identical to the game's, but decompresses to the same data and is usually ~0.5 % larger).
* The chunk grows or shrinks and the padding to the next `0x800` boundary is recomputed; the file may therefore change size. To load a patched file in the game, put it in a mod as `files/Fnnnn.BIN` and run with `--mods` (see `MOD_FILES.md`): the game then reads it with its new size.
* Messages not referenced by any op 19 may shift when earlier messages change length; the tool warns about it because other opcodes might point at them.
* The size of the bank that fits in RAM is unknown (`TEXT_SYSTEM.md` Q-TS4). Keep translations reasonably close to the original size.

### verify

Read-only self-check on real data: for every bank, LZ compress/decompress round trip, text render/encode round trip, unchanged rebuild equals the original, a growth rebuild keeps every op-19 reference consistent, and a whole-file patch with forced recompression re-reads to identical banks. Exit 1 on any failure.

## Window limits and overflow warnings

The dialogue window is the same box in JP and US: lines start at x=16 and the inside of the box ends at x=303, so a line has **288 px** (JP: 24 glyphs of 12 px). The box shows the speaker-name line plus 3 text lines, so **4 lines per page** (`<br>`-separated, name line included). The game does not wrap or clip: a line that is too wide is drawn over the border and out of the box (up to the screen edge); a 5th line is drawn below the box on the map. A control-only line (for example the trailing `<c:1107><c:1106>`) does not count.

`insert` measures every message it changes and prints `aviso: F0057 msg 0 pag 1 linea 3: 300 px > 288 px de ventana` (file, message, page, physical line counted from the speaker-name line, measured width, limit) and `... pag 1: 5 lineas > 4 por pagina`. Warnings never block the write unless `--strict` is given (then nothing is written and the exit code is 1). `verify` prints the same measurement for the original text (US: widest line 280 px; JP: 288 px, exactly the limit; pages with more than 4 lines are lists and menus, not dialogue).

US widths: the engine's width routine (resident `0x8001BF2C`) adds, per ASCII char, `pair[idx[c - 0x20]].width + 1` px, and draws the 8-px bitmap shifted left by `pair.left`. `idx` (96 B at `0x80010464`) and the 16 `{left, width}` pairs (`0x80010444`) are in the boot EXE. `p2text` reads them at run time from `--disc` (its boot executable) or `--exe <SLUS_011.58>`, after checking the two `addiu` words that load those addresses; they are never copied into the repository. Without `--disc`/`--exe` only the lines per page are checked. The widths cannot be derived from the bitmaps alone (a few glyphs, for example `Q S Z \ 0 1`, differ from ink width + 1), which is why the game's table is read. Space is 5 px. Confidence: HIGH. A test mod drawing all 94 printable ASCII chars matched the model pixel for pixel (442/442 and 625/625 glyph pixels; the third line differs only in the descender rows of `g j p q y`, whose vertical placement is not modelled), and natural lines of F0057 messages 0 and 2 to 5 matched too.

Limits of the check: the 288 px / 4 line values were measured on the F0057 shrine window only (US frames of `us_new_game_to_field.txt` around f7000; JP frame f6800). Windows without a speaker name, menus, other event windows and the scroll arrow at the bottom right of line 3 (about x=296..303) were not measured.

## Limits

* Only field-event banks are covered (menus, battle text and the other font chunk of `F0013` are not).
* No glyph-to-character table is included. JP: the engine advances a fixed 12 px. US: widths come from the disc's own EXE (below); controls with a runtime width (`<indent>`, speaker-name placeholders) count as 0 px.
* `0x1106`/`0x1107` and other controls are preserved verbatim but their meaning is unknown.
