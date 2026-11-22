# HALTZ GPU protocol v1 (provisional)

Status: draft, rev 2, 2026-11-22. Goes with architecture-v1.md (GPU section) and wiring-v1.md (stage 3).
Rev 2: strip rendering decided; GPU firmware written (section 6).
Covers how the S3 talks to the Pico GPU over the SPI link: transport rules, boot handshake, the picture model, and every message.
The single source of truth for IDs and sizes is `shared/haltz_proto/include/haltz/msg_ids.def`; constants and bit layouts are in `haltz/gpu.h`. This document explains them.
Two points differ from architecture-v1: the GPU does not keep a double-buffered RGB565 frame (decided), and drawing commands get no ACK (open question 2).

## Design goals
1. Commands, not pixels. The S3 changes a picture that the GPU keeps (text, tiles, sprites, bitmap). Moving a sprite costs 11 bytes on the wire.
2. Same packet as every link: `SYNC 0xA5 | TYPE | LEN | PAYLOAD | CRC8`. Unchanged.
3. Fire and forget. Drawing commands are not acknowledged. The GPU reports only errors (NAK) and counters (STATUS), so the link is one long stream, not request/wait/reply.
4. Frame-exact. The panel changes only at PRESENT, so a program never shows a half-drawn frame.
5. The command stream is the API. If the TFT ever moves to the S3 (fallback in architecture-v1), a local renderer reads the same frames and programs don't notice.
6. Fits the Pico's RAM with room to spare, and the speed the Pico can do as an SPI slave.

## 1. Transport (the SPI link)
| Setting | Value | Why |
|---|---|---|
| SPI mode | 3 (CPOL=1, CPHA=1), MSB first, 8-bit | Proven by the wiring test. In mode 3 the Pico's SPI slave keeps working while CS stays low for many bytes |
| Clock | Start at 4 MHz, target 10 MHz | The Pico's SPI slave needs clk_peri >= 12 x SCK: 150 MHz / 12 = 12.5 MHz max. 10 MHz = 80 MHz / 8 on the S3 |
| Transaction | CS low, 1 to 4096 bytes, CS high | `HALTZ_GPU_MAX_XFER`. One S3 DMA buffer |
| Filler | 0x00 | Never SYNC. Also what a dead or reset line reads |

**Two byte streams.** Each direction is one continuous stream of frames. CS only means "the bus is in use"; transaction boundaries carry no meaning.
- MOSI (S3 -> GPU): frames back to back, filler allowed between frames. The S3 never splits a frame across two transactions, so the GPU flushes its decoder when CS is high and everything received is consumed (this drops a half frame caused by a glitch).
- MISO (GPU -> S3): the GPU's queued frames come out whenever the S3 clocks, followed by filler. A frame can be split across transactions, so the S3 runs one decoder over the MISO bytes of every transaction and never flushes it at CS.
- Every transaction is full duplex: replies ride along with drawing traffic for free. When the S3 has nothing to send but expects a reply, it clocks a poll: 64 filler bytes (`HALTZ_GPU_POLL_XFER`).
- Reply delay: the Pico keeps up to 8 bytes in its SPI TX FIFO, so a reply starts at most 8 filler bytes after the GPU queued it.

Why streams: at 10 MHz the Pico can't react between two bytes, so "send a request and get the reply in the same transaction" would need exact timing. With streams the GPU's TX DMA just runs: queued frames, then a filler source that never ends.

**BUSY** (Pico -> S3): high = don't start a transaction. The GPU raises BUSY when its receive ring has less than 2 x 4096 bytes free (`HALTZ_GPU_BUSY_FREE`). The S3 checks BUSY before every transaction, never during one. Because the threshold is two maximum transactions, a transaction that starts just as BUSY rises still fits. BUSY is only valid after the GPU's HELLO (during reset and boot the pin floats).

**FRAME** (Pico -> S3): a high pulse of at least 2 us. Rising edge = the frame of one PRESENT is completely on the panel. Exactly one pulse per PRESENT, in order; nothing else pulses it. The S3 uses it as vsync.

**RESET** (S3 -> Pico RUN, open drain): hard reset, held low for at least 1 ms. Used at boot, during standby (held), and when the GPU stops answering.

## 2. Boot handshake and health
1. The S3 pulls RESET low for at least 1 ms, then releases it.
2. The GPU boots: clears the panel to black, turns the backlight on (255), writes `HALTZ GPU vN git XXXXXXXX - waiting for CPU` on the screen (so a GPU-only bring-up shows something), starts the SPI DMA and queues its HELLO.
3. The S3 sends its HELLO every 50 ms, each in a 64-byte transaction (HELLO plus filler), and watches MISO. The GPU answers every HELLO it receives with its own HELLO. No answer within 2 s: reset again. After 3 tries: GPU missing (console message, red blink on the LED).
4. The S3 compares `proto_version`. If it differs, the S3 shows a warning (on the TFT if it works, otherwise the console) and carries on (repo-layout rule 5).
5. The S3 sends GPU_INIT, draws its screen, and sends PRESENT.

Health:
- The S3 sends GPU_STATUS_REQ at least every 500 ms (cheap: 4 bytes, so sending one with every PRESENT is fine). The GPU answers with GPU_STATUS.
- The S3 hard-resets the GPU and repeats the handshake when: no STATUS for 1 s, BUSY high for more than 200 ms, or no FRAME pulse 500 ms after a PRESENT.
- An unrequested HELLO from the GPU, or `uptime_ms` going backwards in STATUS, means the GPU rebooted by itself and its picture is lost.
- After any GPU reset the S3 must send the whole picture again. So the S3 kernel keeps a shadow copy of the GPU state (about 150 KB, in PSRAM) and replays it. The S3 fallback renderer would draw from the same shadow.

## 3. The picture model
Screen: 320 x 240 landscape, (0,0) at the top left as mounted (the GPU's MADCTL does the 180 degree turn), x to the right, y down.

Layers, back to front:
| # | Layer | Size | Pixels |
|---|---|---|---|
| 0 | Backdrop | Whole screen | palette[0] |
| 1 | Bitmap | 320 x 240 | 8-bit palette index per pixel |
| 2 | Tiles | 32 x 32 map of 16 x 16 tiles (512 x 512 px), scrolls and wraps | 4-bit patterns + palette bank |
| 3 | Sprites | 64 sprites, 16 x 16 each | 4-bit patterns + palette bank |
| 4 | Text | 40 x 30 cells, 8 x 8 font | Foreground/background palette index per cell |
| 5 | Sprites marked ABOVE_TEXT | | For example the pointer, drawn over the monitor |

Architecture-v1's three modes (TEXT, TILES + SPRITES, BITMAP) become layers that can be on at the same time (GPU_LAYERS). This lets the halt monitor put its text over a running game's picture.

**Colours**
- One palette of 256 RGB565 entries.
- Colour index 0 is transparent in every layer. Where nothing opaque is drawn, the backdrop shows, which is palette[0]. Changing palette[0] changes the backdrop colour.
- Tiles and sprites use 4-bit pixels p with a palette bank b (0-15): colour index = b x 16 + p. p = 0 is transparent.
- Default palette (set by GPU_INIT): 0 black (the backdrop), 1-15 the system colours, 16-231 a 6 x 6 x 6 colour cube, 232-255 greys. Index 16 is an opaque black.
- System colours: 1 white, 2 red, 3 green, 4 blue, 5 yellow, 6 cyan, 7 magenta, 8 dark grey, 9 light grey, 10 orange, 11 brown, 12 pink, 13 light green, 14 light blue, 15 dark blue (names in `gpu.h`). Colour cube levels: 0, 51, 102, 153, 204, 255 (index 16 + 36r + 6g + b). Greys: 8 + 10i.
- The palette table is `haltz_gpu_default_palette` in shared/, generated by `tools/gen_gpu_tables.py`, so the GPU, the S3 shadow and the fallback all use the same values.

**Patterns**: 256 patterns of 16 x 16 4-bit pixels (128 bytes each), shared by tiles and sprites. Byte order: 16 rows top to bottom, 8 bytes per row, high nibble = left pixel. GPU_INIT clears them to 0 (fully transparent), so tile entry 0 is an empty tile.

**Tile map entry** (u16): bits 0-7 pattern, bits 8-11 palette bank, bit 12 horizontal flip, bit 13 vertical flip, bits 14-15 reserved (0).

**Sprite attribute** (u8): bits 0-3 palette bank, bit 4 horizontal flip, bit 5 vertical flip, bit 6 above text, bit 7 visible. A higher sprite number is drawn on top. By kernel convention sprite 63 is the pointer, with ABOVE_TEXT set.

**Text cell**: character u8 (a font index: no control codes, newline handling is the kernel's job), foreground u8, background u8 (0 = transparent). The font has 256 glyphs of 8 x 8 pixels, 1 bit per pixel, row by row, bit 7 = left pixel. The default font (`haltz_gpu_default_font`, drawn as ASCII art in `shared/haltz_proto/assets/font8x8.txt`) has ASCII 0x20-0x7E, box drawing lines and corners at 0x80-0x8A, blocks at 0x8B-0x8F and shades at 0x90-0x92 (names in `gpu.h`); everything else is blank. GPU_FONT_LOAD replaces glyphs. There is one text cursor: off, underline or block, with optional blinking. The GPU blinks it by itself at 2 Hz and redraws only that cell (no FRAME pulse for a blink).

**Frames**
- Commands are applied in order. They change the GPU's picture state, not the panel.
- GPU_PRESENT(id): the GPU draws every area changed since the last PRESENT onto the panel, then pulses FRAME, and STATUS reports `present_id = id`. Commands after a PRESENT wait in the receive ring until that frame is on the panel, so the panel always shows exactly the state at a PRESENT.
- Exceptions: the cursor blink (above), and GPU_BACKLIGHT, which acts immediately.
- Pacing: at most one PRESENT in flight. After a PRESENT the S3 may keep sending commands for the next frame, but it sends the next PRESENT only after the FRAME pulse. The VM's vsync syscall waits for FRAME.
- Frame rate is limited by the panel link. A full screen is 153,600 bytes: about 33 ms of transfer at a TFT SPI clock of 37.5 MHz (150 MHz / 4); measured 36.5 ms per full frame, 27 fps. Small changes (a text line, a few sprites) take well under 1 ms, because only changed areas are sent.

**Clipping and errors**
- Drawing outside the screen or map is clipped silently. It is not an error.
- Out-of-range numbers (sprite >= 64, text column >= 40, ...), reserved bits set, or unknown layer bits: NAK BAD_VALUE, and the whole command is ignored.
- Wrong length: NAK BAD_LEN. Unknown TYPE: NAK UNKNOWN_TYPE.
- Frames with a bad CRC are dropped without a NAK (their TYPE can't be trusted) and counted in STATUS.
- NAKs are best effort. If the GPU's send queue is full the NAK is dropped, but `cmd_errors` still counts it.
- ACK is not used on the GPU link. Architecture-v1 says command links answer ACK/NAK, but for the GPU that would mean thousands of ACKs per second. Errors come back as NAK; success shows as FRAME plus STATUS.

## 4. Messages
ID ranges: 0x01-0x0F every link, 0x10-0x3F GPU link, 0x40-0x6F reserved for the I/O link, the rest free.
All multi-byte fields are little-endian; i16 is two's complement. "n" is worked out from LEN.
Common messages on this link: HELLO 0x01 (both directions), NAK 0x03 (GPU -> S3). ACK 0x02 is not used here.

### Summary
| ID | Name | Dir | LEN | Payload |
|---|---|---|---|---|
| 0x10 | GPU_INIT | S3 -> GPU | 0 | - |
| 0x11 | GPU_PRESENT | S3 -> GPU | 2 | id u16 |
| 0x12 | GPU_STATUS_REQ | S3 -> GPU | 0 | - |
| 0x13 | GPU_BACKLIGHT | S3 -> GPU | 1 | level u8 |
| 0x14 | GPU_LAYERS | S3 -> GPU | 1 | mask u8 |
| 0x15 | GPU_PALETTE | S3 -> GPU | 3-255, odd | first u8, n x rgb565 u16 |
| 0x18 | GPU_TEXT_PUT | S3 -> GPU | 5-255 | col u8, row u8, fg u8, bg u8, n x char u8 |
| 0x19 | GPU_TEXT_FILL | S3 -> GPU | 7 | col, row, w, h, char, fg, bg (all u8) |
| 0x1A | GPU_TEXT_SCROLL | S3 -> GPU | 6 | top u8, height u8, rows i8, char u8, fg u8, bg u8 |
| 0x1B | GPU_TEXT_CURSOR | S3 -> GPU | 3 | col u8, row u8, style u8 |
| 0x1C | GPU_FONT_LOAD | S3 -> GPU | 9-249, 1 + 8n | first u8, n x 8 bytes |
| 0x20 | GPU_PATTERN_LOAD | S3 -> GPU | 129 | index u8, 128 bytes |
| 0x21 | GPU_TILE_PUT | S3 -> GPU | 4-254, even | x u8, y u8, n x entry u16 |
| 0x22 | GPU_TILE_FILL | S3 -> GPU | 6 | x u8, y u8, w u8, h u8, entry u16 |
| 0x23 | GPU_TILE_SCROLL | S3 -> GPU | 4 | sx u16, sy u16 |
| 0x28 | GPU_SPRITE_SET | S3 -> GPU | 7 | id u8, x i16, y i16, pattern u8, attr u8 |
| 0x30 | GPU_BMP_PIXELS | S3 -> GPU | 5-255 | x u16, y u16, n x colour u8 |
| 0x31 | GPU_BMP_FILL | S3 -> GPU | 9 | x u16, y u16, w u16, h u16, colour u8 |
| 0x32 | GPU_BMP_LINE | S3 -> GPU | 9 | x0 i16, y0 i16, x1 i16, y1 i16, colour u8 |
| 0x33 | GPU_BMP_COPY | S3 -> GPU | 12 | sx u16, sy u16, w u16, h u16, dx i16, dy i16 |
| 0x3F | GPU_STATUS | GPU -> S3 | 18 | see below |

### Control
- **GPU_INIT**: resets the picture state to the defaults. Layers TEXT + SPRITES; default palette and font; text all spaces with foreground 1 (white) and background 0; cursor off; tile map all 0, scroll 0; patterns all 0; sprites hidden; bitmap all 0. The next PRESENT redraws the whole screen. Does not change the backlight, the counters or `present_id`.
- **GPU_PRESENT**: `id` is chosen by the S3, normally a counter. Shows the frame (see Frames).
- **GPU_STATUS_REQ**: the GPU answers with GPU_STATUS.
- **GPU_BACKLIGHT**: 0 = off ... 255 = full, PWM on GP22. Acts immediately.
- **GPU_LAYERS**: bit 0 BITMAP, bit 1 TILES, bit 2 SPRITES, bit 3 TEXT, bits 4-7 must be 0. A layer that is off is skipped (and costs no render time). Sprites marked ABOVE_TEXT belong to the SPRITES layer.
- **GPU_PALETTE**: writes n = (LEN - 1) / 2 entries (1-127) starting at `first`. first + n must be <= 256. The whole palette takes 3 messages.

### Text
- **GPU_TEXT_PUT**: writes n characters (1-251) from (col, row), left to right with the given colours. Wraps to the start of the next row, stops after row 29 (no scrolling). col < 40 and row < 30.
- **GPU_TEXT_FILL**: fills a w x h block of cells with one character and colours. Clipped to the screen. Clearing the screen is one TEXT_FILL.
- **GPU_TEXT_SCROLL**: rows `top` to `top + height - 1` move up by `rows` (negative: down). The rows that become free are filled with `char`, `fg`, `bg`. `|rows| >= height` just clears the region. The region is clipped to the screen. Lets the console scroll while a status line stays put.
- **GPU_TEXT_CURSOR**: style 0 off, 1 underline, 2 block, plus 0x80 to blink. Drawn in the cell's foreground colour. A position outside the screen hides it.
- **GPU_FONT_LOAD**: replaces n = (LEN - 1) / 8 glyphs (1-31) starting at `first`. LEN - 1 must be a multiple of 8, first + n <= 256.

### Patterns and tiles
- **GPU_PATTERN_LOAD**: replaces one pattern (layout in section 3).
- **GPU_TILE_PUT**: writes n = (LEN - 2) / 2 map entries (1-126) from (x, y) along the row, wrapping to the next row, stopping at the end of the map. x < 32 and y < 32.
- **GPU_TILE_FILL**: fills a w x h block of the map with one entry. Clipped to the map.
- **GPU_TILE_SCROLL**: the map pixel (sx, sy) is shown at screen (0, 0). Both are taken mod 512, so the map wraps around.

### Sprites
- **GPU_SPRITE_SET**: sets everything about one sprite. id 0-63. (x, y) is the top-left corner on the screen and may be off screen (negative, or past the edge), which is how sprites slide in. A hidden sprite is one without the visible bit.

### Bitmap
- **GPU_BMP_PIXELS**: n colour indices (1-251) from (x, y) to the right, one row only, clipped at the right edge. An image row of 320 pixels takes 2 messages.
- **GPU_BMP_FILL**: fills a rectangle. Clipped.
- **GPU_BMP_LINE**: a straight line, both end points included. Clipped.
- **GPU_BMP_COPY**: copies the w x h rectangle at (sx, sy) to (dx, dy). Overlapping areas are handled (like memmove), the result is clipped. Used to scroll the bitmap.

### GPU_STATUS (GPU -> S3, 18 bytes)
| Field | Type | Meaning |
|---|---|---|
| present_id | u16 | id of the last PRESENT that is on the panel |
| frames | u16 | frames shown since boot (wraps) |
| render_us | u16 | time the last frame took, saturates at 65535 |
| rx_free_kb | u8 | free space in the receive ring, KB |
| flags | u8 | bit 0 BUSY is high, bit 1 a frame is being drawn, bits 2-7 0 |
| crc_errors | u16 | frames dropped for a bad CRC (wraps) |
| cmd_errors | u16 | commands refused with a NAK, sent or not (wraps) |
| rx_overruns | u16 | times the receive ring overflowed, data lost (wraps). Must stay 0: it means the S3 ignored BUSY |
| uptime_ms | u32 | ms since the GPU booted. Going backwards = the GPU rebooted |

## 5. Sizes and speed
At 10 MHz the link carries 1.25 MB/s. Each frame adds 4 bytes.
| Job | Bytes on the wire | Time |
|---|---|---|
| Move one sprite | 11 | 9 us |
| Write a 40-character text line | 48 | 38 us |
| Redraw all 1200 text cells (5 TEXT_PUTs) | about 1.2 KB | 1 ms |
| Move all 64 sprites | 704 | 0.6 ms |
| Whole tile map (32 x 32) | about 2.1 KB | 1.7 ms |
| Whole palette (3 messages) | 527 | 0.4 ms |
| All 256 patterns | about 34 KB | 27 ms |
| A full-screen bitmap image | about 81 KB | 65 ms |
| GPU_STATUS_REQ + its reply | 4 + 22 | 21 us |

A 60 fps game has 20 KB of link time per frame, far more than a frame of sprite moves, scrolling and score text needs. Only loading graphics (patterns, images) takes noticeable time, which is normal at program start.

## 6. GPU side (how it fits on the Pico)
Decided 2026-11-22: strips with dirty rectangles, no frame buffer (was open question 1). Implemented in `firmware/gpu-pico`.
- Core 0: SPI0 slave. RX DMA runs endlessly into a 32 KB ring and counts every byte (so an overrun is detected, not guessed). TX DMA sends queued frames, then chains to a channel that reads one 0x00 byte forever. Core 0 parses frames from the ring, applies them to the picture state, marks the changed 8 x 8 cells, and drives BUSY. When CS is high and every byte is consumed, a half frame left in the decoder is flushed.
- Core 0 at PRESENT: merges the changed cells into rectangles (at most 48; more, or more than 3/4 of the screen, becomes one full-screen rectangle) and hands them to core 1. Until core 1 is done it applies no further commands; they wait in the ring.
- Core 1: draws each rectangle in strips of up to 320 x 16 pixels into two RGB565 buffers in turn: one is being rendered while DMA sends the other to the TFT on SPI1 (16-bit frames). Then the FRAME pulse. Typing one character sends one 8 x 8 cell (128 bytes), not a whole strip.
- Cursor blink: when the phase changes, core 0 sends a job for the cursor cell alone (no FRAME pulse). A PRESENT job always includes the cursor cell (unless a rectangle already covers it), so the drawn phase stays in step.
- Renderer speed: it runs from RAM (not the flash cache shared with core 0) and works per run (one tile entry per 16-pixel run, one glyph row per text cell, sprites filtered once per strip, empty pattern rows skipped). Measured on the board, 2026-11-22: composing a full screen with every layer takes about 10 ms (about 20 cycles per pixel), text only about 7.6 ms; a full-screen frame takes 36.5 ms in total, so the demo (all layers, full redraw every frame) runs at 27 fps and the panel link is the limit. The first version needed 55 ms per frame (18 fps) because drawing was slower than sending. The PC tests compare the fast renderer with a simple reference renderer on 300 random pictures.
- RAM:

| What | Bytes |
|---|---|
| Bitmap layer, 320 x 240 x 8 bit | 76,800 |
| Patterns, 256 x 128 | 32,768 |
| Receive ring | 32,768 |
| Strip buffers, 2 x 320 x 16 x 2 | 20,480 |
| Text cells, 40 x 30 x 3 | 3,600 |
| Tile map, 32 x 32 x 2 | 2,048 |
| Font, 256 x 8 | 2,048 |
| Palette, 256 x 2 | 512 |
| Sprites, 64 x 7 | 448 |
| Total | about 171 KB of 520 KB (the real build: 184 KB of RAM with stacks and buffers) |

- Change from architecture-v1: no double-buffered RGB565 frame. The ST7789's own memory already holds the picture on screen, and the frame rate is set by the panel link (about 33 ms per full screen at 37.5 MHz), which a second frame buffer can't make faster. If the strip renderer turns out too slow, one full RGB565 frame (153.6 KB) still fits.
- The picture state, renderer and scenes (`gpu_state.c`, `gpu_render.c`, `gpu_scenes.c`) use no Pico SDK calls: the S3 fallback can reuse them, and they have PC tests that also write PNG pictures (`firmware/gpu-pico/test`).
- Console: Pico #1's own USB port (USB serial; also lets picotool restart it for flashing, so no debug probe is needed), and the same output on UART0 (GP0/GP1) for a probe. `d` runs a demo using every layer (only before the CPU has said HELLO), `s` prints the counters, `v` the banner. NAKs, HELLOs and overruns are logged there. A terminal that stops reading can delay the GPU by at most 2 ms per line (then output is dropped), so it can't stall the link. This changes repo-layout-v1 ("stdio over USB off").

## 7. S3 side
- A `gpu` driver task on core 0. The kernel console and the VM syscalls call `gpu_*()` functions that append frames to a 4 KB DMA buffer in internal RAM (two buffers, used in turn). A buffer is sent when it's full, at PRESENT, or 2 ms after its first frame (keeps typing snappy). Before each send, the task waits for BUSY low.
- The MISO bytes of every transaction go into one decoder. HELLO, STATUS and NAK are handled there; NAKs are logged on the console.
- The same `gpu_*()` calls update the shadow state in PSRAM, which is replayed after a GPU reset.
- The FRAME pin interrupt counts frames and wakes whoever waits for vsync.

## 8. Shared code
- `msg_ids.def` lines are now `X(NAME, ID, MIN_LEN, MAX_LEN)`. Fixed-size messages have MIN = MAX. `haltz_msg_check()` checks the range; each message's unpack function checks the rest (odd length, multiple of 8, ...).
- `haltz/gpu.h`: screen and layer sizes, layer bits, attribute bits, colour names, link limits and timings.
- `haltz/gpu_msgs.h`: one struct per message, `haltz_gpu_<name>_pack()` / `_unpack()` for each, and `haltz_gpu_cmd_unpack()`, which unpacks any S3 -> GPU command into one union (what the GPU's command loop calls). Pack and unpack apply the same value rules, so the S3 finds its own bugs before they reach the wire.
- Default palette and font: `src/gpu_tables.c`, generated by `tools/gen_gpu_tables.py` from the palette definition in the script and `assets/font8x8.txt`. The generated file is committed, so building firmware needs no Python. `--check` tells whether it is up to date.
- `HALTZ_PROTO_VERSION` goes from 1 to 2 (new message set). All three boards need re-flashing.
- Next: the S3 `gpu` driver (section 7).

## 9. Bring-up order
1. GPU alone: the boot screen drawn by the real renderer (no link yet), then the demo (`d` on the probe console).
2. Link at 4 MHz: HELLO handshake and STATUS polling.
3. Text: the S3 console on the TFT (architecture-v1 bring-up step 4).
4. Raise to 10 MHz. Stress test: a stream of random valid commands with a PRESENT every 16 ms; `crc_errors` and `rx_overruns` must stay 0 and `frames` must match the PRESENTs sent.
5. Tiles, sprites, bitmap.

## Open questions
1. Decided: render in strips with dirty rectangles, no frame buffer (section 6).
2. Is it OK that drawing commands get no ACK on the GPU link (architecture-v1 says command links answer ACK/NAK)?
3. Text cursor: blink on the GPU (as designed here), or let the S3 do it?
4. TFT SPI clock: 37.5 MHz is assumed. Try 75 MHz (150 / 2, above the ST7789's rated 62.5 MHz, but many panels take it) later?
5. Backlight while the Pico is held in reset (standby): many modules pull BL up, which would light the screen. Check at bring-up. If it lights, a pull-down resistor on GP22 keeps the screen dark in standby.
