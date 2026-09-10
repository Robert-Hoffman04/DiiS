# tools/harness-control

Desktop side of the DeSmuMEWii unified test harness
(`desmumewii-harness-and-network-plan.md`). This is **live session control**:
launching a build, talking to it over the network transport, receiving
logs / frames / profiles / crashes, driving input and control commands.

Offline analysis of already-collected results stays in `tools/benchmark/`
(`analyze.py`, `perfzones.py`, `scenes.conf`) - a separate concern, kept
separate on purpose.

| File | What it is |
|---|---|
| `common.sh` | The **one** copy of `dolphin_launch` / `dolphin_kill` / `gecko_*` / `shot` / `ae_diff`. `tools/vsd-testrom/harness/common.sh` and `tools/benchmark/benchmark.sh` both source this now instead of keeping their own copies. |
| `wire.py` | Single source of truth for the §2 framed wire protocol (`magic \| version \| type \| len \| payload`, big-endian). Packet types, encoder, incremental `Decoder`, FRAME payload helpers. The device side (`source/src/harness/`) must stay byte-compatible with this. |
| `wii_control.py` | The transport **server**. Listens on one TCP port (default 4300), same server whether the peer is Dolphin (network passthrough) or a real Wii on the LAN. Prints LOG, saves FRAME (PNG if Pillow is present, else raw `.bin`), records PROFILE, symbolicates CRASH, tallies ASSERT into the process exit code. Built-in watchdog (`--watchdog SECONDS`) sends `CTRL next_rom` on device silence. stdin line commands send INPUT / CTRL. |
| `symbolicate.py` | Turns raw `0x8xxxxxxx` addresses from a CRASH into `<func+0xNN>` using the build's `-Wl,-Map` linker `.map`. Usable standalone. |
| `launch.sh` | One entry point: `launch.sh dolphin <dol>` (headless Dolphin, network passthrough) or `launch.sh wiiload <dol>` (real `wiiload` transfer to `WIILOAD=tcp:<host>`). Sending/launching on a **physical** Wii still needs explicit per-run permission (plan §6.3). |

## Quick start (Dolphin, once network passthrough is confirmed enabled)

```sh
# terminal 1 - start the server
tools/harness-control/wii_control.py --host 127.0.0.1 --port 4300 \
    --out harness-out --map desmumewii.elf.map

# terminal 2 - launch a DESMUME_HARNESS build in headless Dolphin
tools/harness-control/launch.sh dolphin build/desmumewii.dol
```

A single `LOG` round-trip is enough to prove the path before anything is built
on top of it (plan §7.2).

## Wire protocol

```
offset 0  : magic         "DSMH"   (4 bytes)
offset 4  : version        1        (u8)
offset 5  : packet_type             (u8)   LOG=1 FRAME=2 INPUT=3 CTRL=4
offset 6  : payload_length          (u32, big-endian)
offset 10 : payload                        PROFILE=5 CRASH=6 ASSERT=7
```

Framing is explicit because payloads range from UTF-8 log lines to raw
framebuffer bytes; never assume one `send()` == one `recv()`.

- `INPUT` payload: raw bytes in the `geckoinput.cpp` alphabet `a b x y s u d l r z L R`.
- `FRAME` payload: `seq(u32) tick(u64) w(u16) h(u16) fmt(u8) label_len(u8) | label | image`.
- `ASSERT` payload: `pass(u8, 1=pass) | message` (sent on failure only).
- `CRASH` payload: best-effort UTF-8 register/backtrace text with `0x…` addresses.
- `CTRL` payload: short text (`next_rom`, `capture_frame [label]`, `heartbeat rom=<name>`, `ping`).
