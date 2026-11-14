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
p2text extract (--disc <img.cue|bin|iso> [--index N] | --file <F0057.bin>) --out <dir> [--map glyphs.txt]
p2text insert  --file <F0057.bin> --tsv <F0057.tsv> --out <patched.bin> [--map glyphs.txt] [--recompress]
p2text verify  (--disc <img> [--index N] | --file <F.bin>)
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

### Glyph mapping

The font order is the game's own, and kanji/kana cannot be derived automatically. The tool seeds only the identity range `0x20`-`0x7E`; every other glyph renders as `<gNNNN>`. To read and translate the text you provide a mapping file: one line per glyph, `id<TAB>character` (id decimal or `0xHEX`, exactly one UTF-8 character, `#` comments allowed). Later lines override earlier ones; if two glyphs map to the same character the lowest id is used when inserting.

`font-sheet` writes a BMP of the glyphs `first..first+count` (32 per row, id = `first + row*32 + column`) from your own font data, to help you build that mapping by eye. Keep it local.

### insert

Re-reads the original file, applies the rows whose `text` changed (unchanged rows keep the original bytes, so unmapped glyphs survive), rebuilds the message section, updates every op-19 operand, recompresses the chunk and writes a patched file. Any character without a glyph rejects the whole run with a list of `file:line: msg N: U+XXXX 'c' sin glifo` and writes nothing. It prints the size change of the messages, the decompressed bank, the chunk and the file.

* If nothing changed, the output is byte-identical to the input. `--recompress` forces recompression (our compressor is not bit-identical to the game's, but decompresses to the same data and is usually ~0.5 % larger).
* The chunk grows or shrinks and the padding to the next `0x800` boundary is recomputed; the file may therefore change size. A patched file is meant to be served through a `FILEPOS` redirect that also carries the new size (not implemented here).
* Messages not referenced by any op 19 may shift when earlier messages change length; the tool warns about it because other opcodes might point at them.
* The size of the bank that fits in RAM is unknown (`TEXT_SYSTEM.md` Q-TS4). Keep translations reasonably close to the original size.

### verify

Read-only self-check on real data: for every bank, LZ compress/decompress round trip, text render/encode round trip, unchanged rebuild equals the original, a growth rebuild keeps every op-19 reference consistent, and a whole-file patch with forced recompression re-reads to identical banks. Exit 1 on any failure.

## Limits

* Only field-event banks are covered (menus, battle text and the other font chunk of `F0013` are not).
* No glyph-to-character table is included; no width/VWF handling (the engine advances a fixed 12 px).
* `0x1106`/`0x1107` and other controls are preserved verbatim but their meaning is unknown.
