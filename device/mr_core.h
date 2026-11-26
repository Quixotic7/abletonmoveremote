/* SPDX-License-Identifier: MPL-2.0 */
/* The device side of Move Remote, apart from the hardware: a platform (plat_move.c on the Move, plat_sim.c on a
 * Mac) owns the surface link and calls in from two threads:
 *
 *   the real-time thread   mr_rt_block() once per 128-frame block, mr_rt_display() when the panel asks for slice 1
 *   the main thread        mr_init(), then mr_step() in a loop until it says to stop, then mr_shutdown()
 *
 * Between the two there are only single-producer/single-consumer rings and atomics. */
#ifndef MR_CORE_H
#define MR_CORE_H

#include <stddef.h>
#include <stdint.h>

#define MR_VERSION "0.1.1"
#define MR_OUT_MAX 20                         /* USB-MIDI packets the surface takes per transfer */

typedef struct {
    const char *test_dir;  /* where the test hook's files live (/dev/shm on the Move) */
    int simulator;         /* 1: the Mac simulator (MR_CAP_SIMULATOR, no USB-A) */
    int port;              /* MR_PORT unless a test says otherwise */
} mr_cfg_t;

enum { MR_RUN = 0, MR_STOP_EXIT = 1, MR_STOP_POWER = 2, MR_STOP_SIGNAL = 3 };

int mr_init(const mr_cfg_t *cfg);

/* the real-time thread: in[] are the transfer's 8-byte events (zeros already skipped or not, either is fine);
 * ain may be NULL; aout gets MR_BLOCK stereo frames; out[] gets up to MR_OUT_MAX packets for the next transfer */
void mr_rt_block(const uint8_t *in, int nin, const int16_t *ain, int16_t *aout, uint8_t out[][4], int *nout);
/* copies the newest whole frame into serving (call when the panel asks for slice 1) */
void mr_rt_display(uint8_t *serving);
void mr_rt_late(void);                         /* the platform saw a late transfer */

/* the main thread: waits up to max_wait_ms for something to do. Returns MR_RUN or why to stop */
int mr_step(int max_wait_ms);
void mr_request_stop(void);                    /* async-signal-safe: mr_step() returns MR_STOP_SIGNAL */
/* any thread (an embedder's "Shut Down?" confirmed): mr_step() says BYE(3) and returns MR_STOP_POWER, as the power
 * button held 2 s does; the platform then powers the Move off (plat_move.c: Move's power service over D-Bus; the
 * simulator only logs it) */
void mr_request_power_off(void);
void mr_set_realtime(int rt);                  /* reported in MR_D_STATUS */
int mr_test_vol(void);                         /* percent of output (100 unless the test hook is on) */
void mr_shutdown(void);

/* the platform's hook: a frame was published (the Move copies it to Schwung's display mirror) */
void mr_plat_frame(const uint8_t px[1024]);

/* ---- embedding (a program that runs Move Remote's device code in-process with its own host; Move Remote itself
 * calls none of these, so its behaviour is unchanged)
 *
 * mr_set_port: before mr_init, the port to listen on instead of the platform's (0: the platform's; < 0: no listening
 * socket at all, the embedder's own connections only).
 * mr_set_audio_source: before the rt thread starts; the rt thread then takes each block's output from fn (frames
 * stereo int16 into out; it returns how many it gave, the rest is silence and counts as an underrun) instead of the
 * MR_H_AUDIO queue. fn runs on the rt thread: it must not block, allocate or lock. NULL: the queue again.
 * mr_adopt: any thread; a connected stream socket (a socketpair end) taken as a host connection at the next mr_step,
 * as if it had come in on the listening socket with via = MR_VIA_LOCAL and spoken the plain stream. The descriptor
 * is the core's from then on. 0 queued, -1 the queue is full.
 * mr_set_banner: before mr_init; the status screen (shown while no host is active) names the embedder instead:
 * title on the top bar, then "OPEN IN A BROWSER:", name (e.g. "remotefm1.local:7730") and the Wi-Fi address with
 * web_port. The strings must outlive the core. */
typedef int (*mr_audio_source_fn)(void *ctx, int16_t *out, int frames);
void mr_set_banner(const char *title, const char *name, int web_port);
void mr_set_port(int port);
void mr_set_audio_source(mr_audio_source_fn fn, void *ctx);
int mr_adopt(int fd);

/* ---- audio input for an embedder (RemoteFM14Move: a Fantasy CFW's ain1). Move Remote itself calls neither, so
 * without them its input goes to the host connection as before (MR_H_CONFIG's input flag), and only there.
 *
 * mr_set_audio_in_sink: any time; fn non-NULL: the rt thread calls fn(ctx, in, MR_BLOCK) once per block with that
 * transfer's input pcm (stereo int16 interleaved, 44.1 kHz: the line in, else the mic; silence when the platform
 * gives none), before the block's output is taken from the audio source. fn runs on the rt thread: it must not block,
 * allocate or lock. NULL: off.
 * mr_sim_set_audio_in (plat_sim.c only; tests): the simulator's input pcm, frames stereo int16 played looped in place
 * of its quiet 440 Hz tone; NULL: the tone again. The buffer must stay until it is replaced. */
typedef void (*mr_audio_in_fn)(void *ctx, const int16_t *in, int frames);
void mr_set_audio_in_sink(mr_audio_in_fn fn, void *ctx);
void mr_sim_set_audio_in(const int16_t *pcm, uint32_t frames);

/* ---- USB-A for an embedder (an FM-1 on the Move's USB-A port, RemoteFM14Move): Move Remote itself calls none of
 * these, so without them its USB-A goes to and from the host connection as before (MR_D_INPUT, MR_H_MIDI).
 *
 * mr_set_usba_in: before mr_init. fn non-NULL: USB-A input (cable 2) no longer goes to the host connection; the rt
 * thread queues it, and the embedder's own thread gets it with mr_usba_poll(), which calls fn (on that thread) with
 * the packets waiting, as they came (USB-MIDI packets: cable 2, CIN, 3 bytes). mr_usba_send then works too.
 * mr_usba_send: one embedder thread; complete MIDI messages (SysEx allowed) to the device on USB-A through the same
 * packetizer as MR_H_MIDI, queued whole or not at all: the packets queued, 0 nothing to send, -1 no room now (4096
 * packets queue) or no mr_set_usba_in. mr_usba_queued: the packets still waiting.
 * mr_set_usba_cap: USB-A packets per SPI transfer at most, 1..MR_OUT_MAX - 6 (the default: MR_OUT_MAX - 6), so the
 * Move's LED writes keep their share of each transfer (a transfer takes MR_OUT_MAX packets; USB-A goes first).
 * About 3 SysEx bytes a packet: 8 a transfer is about 8.3 KB/s. Any time.
 * mr_usba_stats: packets in (to the embedder), in dropped (its queue full), out (from mr_usba_send) so far. */
typedef void (*mr_usba_in_fn)(void *ctx, const uint8_t (*pkts)[4], int n);
void mr_set_usba_in(mr_usba_in_fn fn, void *ctx);
int mr_usba_poll(void);
int mr_usba_send(const uint8_t *msgs, size_t n);
int mr_usba_queued(void);
void mr_set_usba_cap(int pkts);
void mr_usba_stats(uint32_t *in, uint32_t *in_dropped, uint32_t *out);

/* ---- the simulator's USB-A (plat_sim.c only; tests): a fake device behind a model of the Move's surface
 * controller, which bridges USB-A MIDI into the SPI transfers. Device -> Move: mr_sim_usba_dev_send() puts packets
 * into the bridge's buffer (MR_SIM_BRIDGE_PKTS); what does not fit is lost, as on the Move (a burst of more than
 * ~200 bytes); each transfer takes at most MR_SIM_BRIDGE_IN of them. Move -> device: each transfer's cable-2
 * packets, for mr_sim_usba_dev_recv(). Unplugged (mr_sim_usba_plug(0), the start): nothing goes either way.
 * mr_sim_counts: LED (cable 0) packets and USB-A packets sent to the surface, USB-A packets lost in the bridge. */
#define MR_SIM_BRIDGE_PKTS 72
#define MR_SIM_BRIDGE_IN 31
void mr_sim_usba_plug(int on);
int mr_sim_usba_dev_send(const uint8_t (*pkts)[4], int n);   /* the packets taken (the rest lost) */
int mr_sim_usba_dev_recv(uint8_t (*pkts)[4], int max);
void mr_sim_counts(uint32_t *led_pkts, uint32_t *usba_out_pkts, uint32_t *bridge_lost);

#endif
