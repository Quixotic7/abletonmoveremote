# The Move as a RemoteFM1 control surface

Status: design. Nothing in RemoteFM1 is changed yet.

## Where it plugs in

RemoteFM1's hosts run an FM-1 firmware and use a panel: the FM-1's keys, buttons, encoders and MASTER in, its 240 ×
240 screen and LEDs out. Today the panel is the real FM-1, over `libremotefm1` (`RemoteFM1/host/`). For the Move,
the panel is a Move running Move Remote.

There are two ways to do it:

- **(A) The Move pretends to be an FM-1.** A Move-side build answers RemoteFM1's HELLO and PANEL_R, and draws the
  TILES itself. No host learns anything new, but Move Remote stops being a general remote and every layout decision
  ships in the Move's firmware.
- **(B) The host maps a Move onto the panel. This is the plan.** Move Remote stays the raw remote it is. The mapping
  is one portable C module, so every host compiles the same code, as they all compile `libremotefm1` already.

### (B) in detail

```
 Move (Move Remote)                          a RemoteFM1 host
 raw input  ──INPUT──►  libmoveremote ──► rfm_move: Move events → panel events ──► the firmware
 LEDs, screen ◄───────  libmoveremote ◄── rfm_move: FM-1 LEDs, 240×240 → Move    ◄── the firmware
 speaker     ◄─AUDIO─   (optional: the Move's audio clock drives the firmware's audio)
```

- `rfm_move` is a new portable C99 module in `RemoteFM1/host/src/move/`, with no OS calls, like the rest of
  `libremotefm1`. It does three things:
  - **In:** it takes `mr_event_t`s and gives `rfm_event_t`s (KEY / BUTTON / ENC / MASTER, with velocity and
    pressure).
  - **LEDs:** it takes the panel's `led[11][4]` and `led_play_green` and gives Move LED writes.
  - **Screen:** it takes the 240 × 240 RGB565 screen and gives a 1024-byte Move frame.
- `libmoveremote` (`host/` and `include/` from this repository) comes in as a submodule or a vendored copy. It
  supplies the socket, the protocol and the decoding.
- **Each host only adds a "Move" choice to its device picker** and a thread that runs `mr_poll`:
  - **FM1VST (Mac):** a `MoveLink` beside `RemoteLink.cpp`.
  - **iOS:** RFMKit compiles the C as it does `libremotefm1`.
  - **Android:** the NDK, likewise.
- **The web player is the hard one.** remotefm1.com is https, and a browser will not open `ws://` from an https page
  to the Move. Options:
  1. A wss endpoint on the Move. Needs a certificate the browser trusts.
  2. A player page served by the Move itself over http.
  3. A small local bridge.
  4. **A bridge window (Move Remote 0.1.1):** the https page opens `http://<move>:7720/bridge` with `window.open()`
     from a click; that plain-http window opens the WebSocket to its own host and relays it over `postMessage`.
     RemoteFM1's `MoveLink` speaks to it. Protocol: [PROTOCOL.md, "The bridge"](PROTOCOL.md#the-bridge-https-pages).

## The mapping (a proposal)

| FM-1 | Move | Notes |
|---|---|---|
| 27 keys (16 white, 11 black) | pads, as two piano rows of two octaves: white keys on pad rows 1 and 3, black keys above them on rows 2 and 4 | velocity → `key_vel`, poly aftertouch → `key_lean` (0..100 %) |
| OCT− / OCT+ | Left / Right | |
| FX SEL ENV LFO EDIT GLO HOME SAVE ARP SEQ | step buttons 1..10, their LEDs showing the buttons' LEDs | the screen shows the labels while Shift is held |
| PLAY / REC | Play / Rec | PLAY's green LED is the Play LED's colour |
| 7 encoders (SELECT, ALGO, K1..K4, PRESETS) | knobs 1..7 (knob 8 spare); the jog wheel turns SELECT as well | touching a knob shows its firmware label (ABI 6 `fm1core_label`) |
| MASTER (0..1023) | the volume knob, 16 per detent | |
| TRS MIDI in | USB-A MIDI in | firmware MIDI out → USB-A out |
| 41 single-colour LEDs at 5 levels | pad brightness in the key's colour; white LED brightness on buttons | |

The pads leave 5 spare pads, and knob 8, Shift, the track buttons and Menu, Back and Capture are left over. These
could be host functions (switch firmware, swap the screen view) instead of firmware controls.

### The screen

The FM-1's screen is 240 × 240 in colour; the Move's is 128 × 64, one bit a pixel. A host shows the full screen on
its own display anyway, so the Move's screen is a companion. Three views, switched with Shift + jog:

1. **Thumbnail + labels (the default).** The whole screen at 64 × 64 (a 3.75× downscale of the luminance, ordered
   dither) on the right. On the left 64 pixels, the knob being touched, or the last thing changed, with its
   firmware label and value.
2. **Zoom.** A 128 × 64 window of the screen at 1.875× reduction, which is enough to read most firmwares' text. It
   pans with the jog wheel.
3. **Labels only.** The 8 knob labels and the 10 button labels, as a cheat sheet.

### Sound

The firmware's audio comes out of the host's audio device, as it does with an FM-1, by default. A "Sound on the
Move" switch sends it to the Move's speaker or outputs instead, clocked by the Move's AUDIO ticks: the firmware's
`audio()` runs once per tick for exactly the frames the Move took. Over USB the queue is 12 ms; over Wi-Fi it is
about 93 ms, which is too slow to play live, so the switch says so.

## Order of work

1. **`rfm_move` and its tests in `RemoteFM1/host`.** The mapping table, the LED and screen reductions, and golden
   frames for the screen views. Testable headless, against `moveremote-sim`.
2. **FM1VST (Mac).** The first host: Move in the device picker, the panel driven by it, sound on the Mac or the Move.
3. **iOS** (RFMKit).
4. **Android** and **the web** (the web through the bridge window).

## Open questions

- The pad layout: piano rows (above) or a chromatic run of 27 pads?
- Should the step buttons carry the FM-1's 10 function buttons, or should those go to Move's named buttons?
- Sound on the Move by default when the link is USB?
