# Move Remote

The whole Ableton Move as a control surface for a computer, phone or tablet, over USB or Wi-Fi. Start it from
Schwung's Tools menu: Move's own software stops, and a host on the other end gets everything raw.

- **From the Move:** every pad (velocity, polyphonic aftertouch), step button, button, knob turn and knob touch,
  jog wheel turn, touch and click, the volume knob, the jacks, and the power button. Also the audio input and MIDI
  from a device on the USB-A port.
- **To the Move:** every LED (palette, brightness and true RGB, with the surface's own animations), the 128 × 64
  screen, audio to the speaker and outputs, and MIDI to the USB-A device.

**Leave it with Shift + touch the volume knob + click the jog wheel.** Move's own software starts again. Holding
the power button for 2 s turns the Move off.

It is a [Schwung](https://github.com/charlesvestal/schwung) standalone tool, built the way
[evilgeniusmove](https://github.com/Quixotic7/evilgeniusmove) is. It is not approved, endorsed or supported by
Ableton.

## Install

You need a Move with [Schwung](https://schwung.dev) **1.6.0 or later** installed.

**From Schwung Manager** (on a computer on the same Wi-Fi as the Move):

1. Open [http://move.local:7700](http://move.local:7700) and go to **Modules**.
2. Find **Move Remote** under Tools and press **Install**.

**If it is not in the list yet:**

1. On the same **Modules** page, open **Install Custom Module**.
2. Under **From GitHub URL**, enter `Quixotic7/abletonmoveremote` and press **Install from GitHub**.

Or download `moveremote-module.tar.gz` from the
[latest release](https://github.com/Quixotic7/abletonmoveremote/releases/latest) and use **From Tarball**.

To update later, use the same page: Move Remote shows an update when there is a new release.

## Use

1. **Start it:** on the Move, open Schwung's Tools menu with **Shift + Step 13** (or **Shift + touch Volume +
   Step 13**, depending on your Schwung shortcut setting), choose **Move Remote**, and press the jog wheel. Move's
   own software stops, and the screen shows the addresses to connect to.
2. **Connect a computer, phone or tablet:**
   - **USB:** plug the Move's USB-C port into the computer and connect to `172.16.254.1`. This is the fastest link.
   - **Wi-Fi:** connect to `move.local`, or the Wi-Fi address on the Move's screen.
   - **Try it in a browser:** open `http://move.local:7720/` (or `http://172.16.254.1:7720/` over USB). The page
     shows every press, lights the pads you press, and lets you draw on the Move's screen. It works on an iPhone
     or iPad too.
3. **Leave it:** hold **Shift**, touch the **volume knob** and **click the jog wheel**. Move's own software starts
   again. Holding the power button for 2 seconds turns the Move off.

While Move Remote runs, the Move only does what the connected app tells it, so it makes no sound of its own.

**On a Mac, if an app cannot connect:** macOS may be blocking it from your local network. Go to **System Settings
→ Privacy & Security → Local Network** and turn the app on.

## Connecting

| Link | Address |
|---|---|
| USB-C cable to the computer | `172.16.254.1:7720`: the Move's own USB network, 0.65 ms round trip |
| Wi-Fi | `move.local:7720`: about 5 ms round trip |
| A browser | `http://move.local:7720/`: the built-in page shows every input, lights what you press, draws on the screen |

The protocol is one small binary stream over TCP, or over a WebSocket at `/ws`. It is described in
**[docs/PROTOCOL.md](docs/PROTOCOL.md)**. Hosts in C use `libmoveremote` (`include/moveremote/client.h`):

```c
mr_client_t *c = mr_connect(NULL, 0, "my host", &callbacks, 2000);   /* USB first, then move.local */
while (mr_poll(c, 10) >= 0) {
    /* callbacks.on_input gets mr_event_t: MR_EV_PAD, MR_EV_KNOB, MR_EV_TOUCH, MR_EV_BUTTON ... */
    mr_pad_rgb(c, 0, 255, 64, 0);
    mr_display(c, packed);   /* mr_gfx_*() draws and packs */
}
```

`mrctl` is a host from the terminal, and a test bench:

```bash
build/host/mrctl info              # what the device says about itself
build/host/mrctl monitor           # every input event, decoded
build/host/mrctl demo              # pads, steps, buttons and knobs light and draw
build/host/mrctl ping 50           # round trips
build/host/mrctl tone 3 2          # a 440 Hz tone at 3% for 2 s (the speaker is loud)
build/host/mrctl quit              # back to Move
```

## Build

```bash
make                      # build/sim/moveremote-sim (the device on this Mac) and build/host/{libmoveremote.a,mrctl}
make test                 # the simulator and mrctl end to end, the page, the WebSocket handshake
make package              # dist/moveremote-module.tar.gz for the Move (aarch64, in Docker)
scripts/deploy.sh --test  # onto ableton@move.local and started, SILENT (--launch to start it normally)
```

The simulator is the device's own code with a fake surface. It takes presses from its test FIFO (`mr-cmd`: `note N
VEL`, `cc N VAL`, `at N PRESSURE`, `shot`, `quit`) and writes its state to `mr-state`. On the Move, the same hook
is on when `/dev/shm/mr-test` exists at launch, which `deploy.sh --test` arranges.

## Layout

| | |
|---|---|
| `device/` | the Move side: `mr_core.c` (rings, LEDs, network, status screen, test hook), `plat_move.c` (SPI, real-time audio thread), `plat_sim.c` (the simulator), `heal.c` (the real-time helper) |
| `host/`, `include/moveremote/` | `libmoveremote`: the client, decoding, a 1-bit canvas |
| `tools/mrctl.c` | the terminal host |
| `web/index.html`, `web/bridge.html` | the page the Move serves, and the bridge window for https pages (RemoteFM1) |
| `module/` | what Schwung installs: `module.json`, `standalone` |
| `docs/` | [PROTOCOL.md](docs/PROTOCOL.md), [REMOTEFM1.md](docs/REMOTEFM1.md) (the Move as a RemoteFM1 surface) |

## Status

0.1.0. Checked on a Move on 2026-11-17:
- **Links:** USB and Wi-Fi, a real-time audio thread, every message type.
- **Input, by hand:** every pad (velocity 1..127, aftertouch), step, button, knob, knob touch, the volume knob, and
  the jog wheel's turn, touch and click.
- **LEDs and screen, by eye:** pads in RGB, the step LEDs, the step icons (CC 16..31), the button LEDs, the knob
  rings, and the screen drawn by the host.
- **Exit:** the exit gesture, by hand, hands the Move back to its own software.
- **Audio:** with output silenced, no underruns at a 12 ms queue over USB, or a 93 ms queue over Wi-Fi.

Not checked yet:
- audio actually heard;
- USB-A MIDI with a device plugged in;
- the power button;
- the browser page on the unit.

Not done:
- speaker EQ (Move's is off while its software is stopped);
- a host app for iOS (the browser page works there now);
- the RemoteFM1 integration ([docs/REMOTEFM1.md](docs/REMOTEFM1.md)).

## License

MPL-2.0 ([LICENSE](LICENSE)); `device/heal.c` is MIT ([THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md)).
