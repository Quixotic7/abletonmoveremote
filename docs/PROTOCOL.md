# Move Remote protocol, version 1

`include/moveremote/proto.h` is the same thing as C; keep the two in step.

## Transport

One byte stream, whichever way it travels:

| Link | Address | Round trip (measured 2026-11-17) |
|---|---|---|
| USB-C (the Move's NCM network gadget, `usb0`) | `172.16.254.1:7720` | 0.65 ms median, 0.74 ms max |
| Wi-Fi | `move.local:7720` (or its address) | 4.6 ms median, 8.6 ms max |
| WebSocket (browsers) | `ws://<move>:7720/ws` | as the link under it |

USB networking stays up while Move's own software is stopped, so "USB" and "Wi-Fi" are the same protocol on two
addresses. Plain TCP and WebSocket share the port: a connection that opens with `GET ` is HTTP (`/` serves the
built-in page, `/ws` upgrades), anything else is the plain stream. A WebSocket's binary frames carry the stream in
any chunking: a message may span frames, and a frame may hold several messages.

## Framing

```
u16 len (little-endian; counts type + payload; >= 1) | u8 type | payload[len - 1]
```

Every multi-byte field is little-endian. Types `0x01..0x7F` go device to host, `0x81..0xFF` host to device. A
message of length 0 is a protocol error; the device answers it with `BYE(5)` and closes.

## Session

1. The host connects and sends `HELLO` with `proto = 1` and its name.
2. The device answers `HELLO`. **The last host to say HELLO is in control**: an earlier one gets `BYE(2)` and is
   closed. Every new host starts from the defaults (below), with every LED off and the device's status screen up
   until it draws.
3. `STATUS` every 500 ms is the heartbeat. A connection that has not said HELLO after 10 s is closed.
4. When the host goes, the device turns every LED off, empties the audio queue and shows its status screen.

Only the host in control can drive the Move. Any connection may `PING`.

## Device to host

| Type | Name | Payload |
|---|---|---|
| `0x01` | HELLO | `"MOVR"` proto16 caps16 rate32 block16 w8 h8 via8 pad8 out_cap32 name[24] (46 bytes) |
| `0x02` | INPUT | n × { frame32, usb_midi[4] } |
| `0x03` | AUDIO | frame32 nframes16 out_fill16 flags8 [int16 stereo × nframes] |
| `0x04` | STATUS | 32 bytes, below |
| `0x05` | PONG | token32 host_time64 frame32 |
| `0x06` | BYE | reason8: 1 exit gesture, 2 replaced, 3 power off, 4 host asked, 5 protocol error |

**caps**: `0x1` audio out, `0x2` audio in, `0x4` RGB LEDs, `0x8` USB-A MIDI, `0x8000` simulator.
**via**: the link the host came in on: 0 Wi-Fi, 1 USB, 2 local.

### INPUT: the raw surface

Every event the surface sends, as the USB-MIDI packet it arrived in, stamped with the device's sample clock (the
frame of the 128-frame block it arrived in). Cable 0 is the surface, cable 2 is a device on USB-A. Nothing is
filtered except the XMOS heartbeat (CIN 0..3) and cables other than 0 and 2. `host/mr_client.c`'s `mr_decode()`
turns them into typed events.

| Control | Message (cable 0) |
|---|---|
| 32 pads (bottom-left to top-right, rows of 8) | `90 68..99 vel` / `80 68..99 0`; poly aftertouch `A0 68..99 p`; channel pressure `D0 p` |
| 16 step buttons | `90 16..31 127` / `80 16..31 0` (no velocity) |
| Touch: knobs 1..8, volume, jog wheel | `90 0..9 v>0` touched, `80` or `90 .. 0` released |
| Knobs 1..8, volume | `B0 71..79 d`: relative, 1..63 clockwise, 65..127 counter-clockwise (`d - 128`) |
| Jog wheel | turn `B0 14 d` (relative, same), click `B0 03 127/0` |
| Buttons | `B0 cc 127/0`: 49 Shift, 50 Menu, 51 Back, 52 Capture, 54 Down, 55 Up, 56 Undo, 58 Loop, 60 Copy, 62 Left, 63 Right, 85 Play, 86 Rec, 88 Mute, 118 Sample, 119 Delete, 40..43 Track 4..1 (reversed) |
| Jacks | `B0 114 0/127` line in, `B0 115 0/127` line out / headphones |
| Power button | SysEx `F0 00 21 1D 01 01 3A bits battery 00 F7` (bit 3 held). The device answers it itself. |

### AUDIO: the clock

Sent every `AUDIO_BATCH` blocks (default 2: 256 frames, 5.8 ms) while `AUDIO_CLOCK` is on. It says how many frames
the speaker just took from the output queue (`nframes`) and how many are still queued (`out_fill`). It carries the
input pcm (mic or line in) when `flags & 0x01`; `flags & 0x02` means the output queue ran dry in this batch.

A host that sends sound answers each tick with about `nframes` frames; `mr_pacer_t` keeps the queue near a target.
Measured on 2026-11-17 with no underruns: 512 frames (12 ms) over USB, 4096 frames (93 ms) over Wi-Fi.

### STATUS

```
 0 frame32        device sample clock          16 late32         SPI transfers > 400 us late
 4 battery8                                    20 dropped32      AUDIO not sent (link backed up)
 5 power8         bit 3: power button held     24 led_pending16  LED writes waiting
 6 jacks8         bit 0 line in, bit 1 out     26 out_fill16     frames queued for the speaker
 7 flags8         bit 0 real-time, bit 1 test  28 test_vol8      percent of output in test mode
 8 underruns32                                 29 pad[3]
12 overruns32
```

The counters start from zero for each host.

## Host to device

| Type | Name | Payload |
|---|---|---|
| `0x81` | HELLO | proto16 name[≤32] |
| `0x82` | LED | n × { status8, number8, value8 } |
| `0x83` | LED_RGB | n × { kind8 (0 note, 1 CC), number8, r8, g8, b8 } |
| `0x84` | DISPLAY | 1024 bytes |
| `0x85` | AUDIO | int16 stereo interleaved, (len − 1) / 4 frames |
| `0x86` | MIDI | complete MIDI messages to USB-A (SysEx, real time and system common included) |
| `0x87` | CONFIG | n × { key8, value32 (signed) } |
| `0x88` | PING | token32 host_time64 |
| `0x89` | QUIT | (none): leave Move Remote; Move's own software starts again |
| `0x8A` | RAW | n × usb_midi[4] to the surface: cable 0, CIN 4..E only. An escape hatch |

### LEDs

`LED` writes the surface's own palette (`status` `0x90|anim` for a note LED, `0xB0|anim` for a CC LED). Note LEDs
are the pads (68..99) and steps (16..31), which take a palette index (0 off, 120 white, 125 blue, 126 green, 127 red;
the 128 colours are in Schwung's `src/shared/constants.mjs`). CC LEDs:

| LEDs | CC | Value |
|---|---|---|
| RGB: Play, Rec, Sample, Track 1..4, knob rings 1..8 | 85, 86, 118, 43..40, 71..78 | palette index |
| White: Shift .. Right, Mute, Delete | 49..52, 54..56, 58, 60, 62, 63, 88, 119 | brightness 0..127 |
| White: under the steps | 16..31 | brightness 0..127 |

`anim` (the status' low nibble) is the surface's own tempo-synced animation: 0 steady, 1..5 fade, 6..10 pulse,
11..15 blink. `LED_RGB` sets a true colour (8 bits each) on any RGB LED, sent as Move's `3B` SysEx. The latest
write to an LED wins, palette or RGB.

The surface takes 20 USB-MIDI packets per 2.9 ms transfer, shared with USB-A MIDI and the power button's reply. The
device keeps one wanted value per LED and sends only the newest, so a host may write as often as it likes. A full
repaint of 32 pads in RGB (192 packets) takes about 30 ms; in palette (32 packets), about 6 ms.

### DISPLAY

128 × 64, one bit a pixel: 8 pages of 128 columns, one byte a column per page, bit 0 the top pixel of the page.
`mr_gfx_pack()` makes it from a canvas. The panel pulls the frame in six slices, so the newest whole frame is shown
up to about 57 times a second; a host need not pace its writes.

### CONFIG keys

| Key | Name | Values | Default |
|---|---|---|---|
| 1 | AUDIO_CLOCK | 0/1: send AUDIO ticks | 1 |
| 2 | AUDIO_IN | 0/1: put the input pcm in them | 0 |
| 3 | AUDIO_BATCH | 1..8 blocks a tick | 2 |
| 4 | OUT_GAIN | 0..256 (256 = unity) | 256 |
| 5 | LEDS_OFF | any: every LED off | |

## On the device, whatever the host does

- **Exit:** Shift held + volume knob touched + jog wheel clicked leaves Move Remote (`BYE(1)`), and Schwung's
  launcher starts Move again. This is decided on the audio thread from the raw events, so it works with no host or a
  wedged one.
- **Power:** the power button held for 2 s powers the Move off (`BYE(3)`).
- **Speaker:** with Move's software stopped there is no speaker EQ (Schwung's `speaker_eq.c` is not applied yet).
