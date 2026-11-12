# PRMTRACE v1 - execution trace format

Status: v1. Any incompatible change bumps `v`. Implementation: `src/trace/trace.{h,cpp}`
(`TraceWriter`, `ReadTrace`, `CompareTraces`). Tests: `tests/unit/test_trace.cpp`.

Purpose: differential testing of the PersonaRM runtime against a reference emulator (PCSX-Redux).
Redux exposes no per-instruction hook, so traces are aligned by **events**, never by instruction
count or cycles. Both sides emit the same format; `CompareTraces` finds the first divergence.

## 1. File layout

UTF-8, JSON Lines (`\n`, `\r\n` tolerated on read). Line 1 is the header; every other line is one event.
Compact JSON, no padding. The C++ writer emits keys in sorted order, so output is byte-deterministic
(DET-001); other producers (Redux Lua) need not match byte-for-byte, only semantically.

Value encoding: addresses and register values are strings `"0x%08x"` (lower case); hashes are
16 lower-case hex digits, no prefix; counters (`seq`, `frame`, `id`, `lba`, `fn`, ...) are JSON integers.

## 2. Header

```
{"v":1,"src":"prm|redux","build":"<git sha|redux version>","disc":"<serial>","exe_sha256":"<hex>",
 "mode":"events|verbose","ram_masks":[["0x00000000","0x00010000"]]}
```

`ram_masks` are half-open physical ranges excluded from `ram_hash` (section 4). Default: the kernel/BIOS
area `[0, 0x10000)`, because the runtime uses an HLE BIOS and Redux the real one. Extra ranges (timing
buffers, dead stack) come from the versioned mask file `tests/diff/ram_masks.json` (ranges only).

## 3. Events

Every event has `seq` (logical, strictly increasing per file, assigned by the writer; gaps allowed) and `ev`.

| `ev` | Fields | Emitted when |
|---|---|---|
| `call` | `kind:"bios"`, `vec:"0xa0/0xb0/0xc0"`, `fn` (t1), `a0..a3`, `ra` | entry of a BIOS A0/B0/C0 call |
| `load_file` | `id`, `dest`, `ra` | entry of `load_file_by_id(id,dest)` |
| `cd_read` | `lba`, `sectors` (=ceil(bytes/2048)), `dest` | every CD read start, including `cd_read_start`/`cd_read_async` (overlays load this way too, OV-001) |
| `vsync` | `frame` (1-based), optional `gp0_hash`, `gp0_words` | end of each vertical blank |
| `ckpt` | `tag`, `pc`, optional `gpr_hash`, `ram_hash`, `scratch_hash`, `vram_hash` | checkpoints (tags: `overlay_loaded`, `vsync_N`, `fn_entry`) |
| `irq` | `src` (`vblank|cd|dma|timer`), `pc` | interrupt delivery (not compared by default) |
| `mmio` | `addr`, `w` (1/2/4), `dir` (`r|w`), `val` | verbose mode only (not compared by default) |
| `end` | `reason` (`quit|limit`), `frames` | last event |

Ordering rule when several events happen at one vsync: `vsync` first, then any `ckpt` taken at it.
Unknown `ev` values are accepted by the reader and compared field-by-field.
Optional fields are omitted when the producer cannot supply them (Redux: `gp0_*`, `vram_hash`).

### 3.1 Collapsed calls (v1 extension)

A polling loop (`TestEvent`, `CdSync`-style waits) would emit one `call` per poll. The writer therefore
collapses a run of **consecutive identical** `call` events (all fields equal except `seq`) into one event
with an extra integer field `n` = number of calls in the run (`n` is omitted when the run has one call).
Any other event ends the run, so ordering is preserved; the collapsed event gets the `seq` of the moment it
is flushed (still strictly increasing). Readers that do not know `n` see a valid v1 event, so `v` stays 1.

## 4. Hashing

All hashes are **FNV-1a 64** (offset `0xCBF29CE484222325`, prime `0x100000001B3`), not cryptographic.
- `ram_hash`: the 2 MiB RAM image with every byte inside `ram_masks` replaced by `0`, then hashed.
- `gpr_hash`: r0 (forced 0), r1..r31, hi, lo as 34 little-endian u32 words.
- `scratch_hash`, `vram_hash`: raw bytes (scratchpad 1 KiB, VRAM 1 MiB).
- `gp0_hash`: reserved: hash of the GP0/GP1 word stream since the previous vsync (little-endian words).
`exe_sha256` and `disc` in the header are informational identity; SHA-256 is computed by the tooling, not here.

## 5. Comparison rules (`CompareTraces`)

1. Header: `v`, `disc`, `exe_sha256`, `mode`, `ram_masks` must be equal when present on both sides;
   `src` and `build` are ignored. Mismatch => divergence at index 0, reason `header ...`.
2. Both event streams are filtered: `irq` and `mmio` are dropped (delivery timing differs between emulators,
   RC-015); configurable via `CompareOptions::ignore_events`.
   After filtering, consecutive identical `call` events (ignoring `seq` and `n`) are merged on each side and
   their repeat counts summed, so a collapsing producer (section 3.1) aligns with a non-collapsing one.
   The count itself is not compared by default (poll counts depend on timing); `CompareOptions::
   compare_repeat_counts` turns that on (reason `repeat count: A vs B`).
3. The remaining events are aligned **by position** (the n-th compared event of A with the n-th of B);
   `seq` is never compared. Equivalent to per-type ordinals because both streams hold the same event kinds.
4. Two events match when `ev` and `kind` are equal and every field present on **both** sides is equal
   (compared as JSON values). A field present on one side only is an accepted omission.
5. Divergence = first position where (a) event type differs, (b) a shared field differs, or (c) one stream
   is shorter. The result carries the index, a reason naming the field, and +-`context` (default 5) raw events
   from each side.
Accepted divergences (HLE BIOS vs real, kernel RAM, IRQ timing) are handled by masks and ignored events, and
must be recorded with evidence in `tests/diff/accepted_divergences.md` (VALIDATION_STRATEGY 10).

## 6. Reader / malformed input

`ReadTrace` fails with `line N: <reason>` on: invalid JSON, a non-object record, a missing/unsupported
header (`v` must be 1), a missing string `ev`, a missing non-negative integer `seq`, or non-increasing `seq`.
Empty lines are skipped.

## 7. Where the engine is wired

Done: `--trace-out`, header at boot, `call` (HLE vector dispatch), `vsync` + `ckpt` (`vsync_N`) per frame,
`irq` at the exception vector, `end` at shutdown, collapsed `call` runs (3.1). The headless boot harness
(`personarm --boot <disc.cue> [--max-frames N] [--trace-out f]`) also emits `load_file` (PC hook on
`load_file_by_id`) and `cd_read` (PC hook on `cd_read_start`). Still open: `mmio`, `gp0_hash`.

`ITraceSink` (in `trace.h`) is the only interface the runtime needs; a null pointer means tracing off.
`TraceWriter` implements it. Planned call sites (owned by other work in progress, hence not edited here):
- `src/engine/engine.cpp`: own a `TraceWriter` (flag `--trace-out <file>`), write the header at boot, call
  `OnVsync` at each emulated vblank followed by `Checkpoint(...)` when due, `OnEnd` at shutdown.
- `src/loader/ps1_bios_hle.cpp` (`HandleBiosHook`/`ExecuteCall`): `OnBiosCall(vec, t1, {a0..a3}, ra)` before dispatch.
- `load_file_by_id` entry (PC `0x800272D0` disc 1 / `0x800254E0` disc 2, intercepted at the CPU hook layer): `OnLoadFile(a0, a1, ra)`.
- CD read start (`cd_read_start` `0x8002724C` / `0x8002541C`, or the CD controller command handler): `OnCdRead`.
- Optional: interrupt delivery in `cpu_context`/bus -> `OnIrq`; MMIO bus accessors -> `OnMmio` (verbose only).
