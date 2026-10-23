# Cybiko radio over ESP-NOW

How the emulated Cybikos on Dragonfruit boards reach each other over
ESP-NOW, how another ESP32 project can join them, and how to test it all on
a PC. It is written for ESP32 projects that want to talk to the emulated
Cybikos, or to each other in the same format.

The protocol itself (packets, CRCs and parity, IDs, channels, pings,
delivery, messages, Chat, gates) is CyOS's own and is documented in
cybiko-docs' [radio-protocol.md](https://github.com/SovGVD/cybiko-docs/blob/main/radio-protocol.md), with the
gate and web browsing in [cywig.md](https://github.com/SovGVD/cybiko-docs/blob/main/cywig.md). This page covers
only the transport, the `cy_radio` library and the test tools.

## Contents

1. [What can talk to what](#1-what-can-talk-to-what)
2. [Carrying packets over ESP-NOW](#2-carrying-packets-over-esp-now)
3. [The cy_radio library](#3-the-cy_radio-library)
4. [Testing on a PC](#4-testing-on-a-pc)
5. [Open questions](#open-questions)

## 1. What can talk to what

A real Cybiko's radio is an RF2915 transceiver driven by an AT90S2313 AVR
(see [hardware.md](https://github.com/SovGVD/cybiko-docs/blob/main/hardware.md#11-radio)). The AVR's own
air protocol (modulation, preamble, how it frames the bytes) is not known,
and an ESP32 has no radio that could speak it. So:

| From | To | Works? |
| --- | --- | --- |
| an emulated Cybiko (Dragonfruit board) | another one | yes, over ESP-NOW |
| an ESP32 project using this document | an emulated Cybiko | yes: send and receive the same ESP-NOW frames |
| an ESP32 project | another ESP32 project | yes, the same way, with no Cybiko involved |
| anything here | a real Cybiko | no: it needs the Cybiko's RF hardware (a future bridge could put a real Cybiko's AVR and RF chip on an ESP32's serial port, see [Open questions](#open-questions)) |

Everything above the AVR, which is all of the protocol, is the same for
real and emulated Cybikos: the emulator passes CyOS's packets through
unchanged.

## 2. Carrying packets over ESP-NOW

Dragonfruit boards (`src/components/cybiko_app/cybiko_link.c`) send every
packet CyOS transmits as an ESP-NOW broadcast to `ff:ff:ff:ff:ff:ff`, with
no encryption:

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 2 | `'C' 'Y'` |
| 2 | 1 | version, 2 |
| 3 | 1 | the RF channel the packet was sent on ([channels](https://github.com/SovGVD/cybiko-docs/blob/main/radio-protocol.md#5-channels)) |
| 4 | 50 or 200 | the packet ([packets](https://github.com/SovGVD/cybiko-docs/blob/main/radio-protocol.md#3-packets)), parity included |

A board hands a frame to its Cybiko only if the Cybiko's radio is tuned to
that RF channel at the time, the way a real radio would only hear its own
channel. So a device that wants to hear Cybikos in Chat listens to frames
for RF channel 4, and one that wants to be heard sends on the channel the
listener is on ([channels](https://github.com/SovGVD/cybiko-docs/blob/main/radio-protocol.md#5-channels)).

All the devices must be on the same Wi-Fi channel. Boards use Wi-Fi channel
1 unless `rf.channel` in `cybiko.cfg` says otherwise; a board that has
joined a Wi-Fi network (for the web gate or the browser) is on that
network's channel instead. ESP-NOW frames carry no acknowledgement or retry
for broadcasts; CyOS's own acknowledgements ([delivery](https://github.com/SovGVD/cybiko-docs/blob/main/radio-protocol.md#7-delivery-acknowledgements-and-queries)) take care of that.
There is no hop count and no relaying ([mesh](https://github.com/SovGVD/cybiko-docs/blob/main/radio-protocol.md#10-mesh-gates-and-the-internet)): a frame reaches the
boards in range of the sender and no further.

Over ESP-NOW nothing is lost or garbled in practice, so the Reed-Solomon
parity is never needed to repair a packet, but it must still be there and
right: CyOS runs every packet through its decoder and drops it otherwise.

Every board needs its own Cybiko ID (CyOS drops packets that carry its own
ID as the sender). Boards keep the flash dump's first byte (`4c` for an
Xtreme) and take the other three from the Wi-Fi MAC (`rf.id = auto`); a new
device should pick an ID the same way.

## 3. The cy_radio library

[`src/components/cy_radio`](../src/components/cy_radio) is the protocol as
portable C (C99, no dependencies, no allocation; an ESP-IDF component, and
it builds on a PC too):

| Function | Does |
| --- | --- |
| `cyr_header`, `cyr_seal`, `cyr_check` | build a header; add CRCs and parity; check a received packet |
| `cyr_ping`, `cyr_ping_details` | make a ping with a nickname; read one |
| `cyr_ack`, `cyr_query` | answer a message packet; ask whether one was delivered |
| `cyr_msg_packets`, `cyr_msg_parse`, `cyr_rx_t` | turn a message into packets (parts included); read one; put parts together |
| `cyr_chat`, `cyr_chat_parse` | a chat line for a room; read one |
| `cyr_cyid_str` | an ID's 7-letter form |
| `cyr_espnow_wrap`, `cyr_espnow_unwrap` | the Dragonfruit ESP-NOW frame |

Sending a chat line to everyone from an ESP32 (Wi-Fi started in station
mode on channel 1, ESP-NOW initialised with a broadcast peer):

```c
#include "cy_radio.h"

static void send_frame(const uint8_t *pkt, size_t len, uint8_t rf)
{
    static const uint8_t all[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    uint8_t frame[CYR_ESPNOW_HEAD + CYR_LONG];
    esp_now_send(all, frame, cyr_espnow_wrap(frame, pkt, len, rf));
}

void say(uint32_t my_id, const char *text)
{
    cyr_msg_t m;
    uint8_t out[2][CYR_LONG];
    cyr_chat(&m, CYR_ROOM_GENERAL, esp_timer_get_time() / 1000, text, false);
    int n = cyr_msg_packets(&m, CYR_BROADCAST, my_id, CYR_RF_CHAT, false, 0, out, 2);
    for (int copy = 0; copy < 3; copy++)        /* better spread out over ~400 ms */
        for (int i = 0; i < n; i++) send_frame(out[i], CYR_LONG, CYR_RF_CHAT);
}
```

The `--bot` code in `src/host/cybiko_host.c` is a complete small client:
pings, receiving, acknowledging, private lines with retries and queries.

## 4. Testing on a PC

`cybiko_host` (see [cybiko.md](cybiko.md#pc-runner)) runs up to three
Cybikos on one radio, and a radio-only chat partner:

| Option | Does |
| --- | --- |
| `--pair DISK2`, `--third DISK3` | a second and third Cybiko (IDs +1, +2), keys with `--press b:KEY@MS` and `c:KEY@MS` |
| `--chain` | B hears A and C, A and C don't hear each other |
| `--rf-log` | print every packet sent: time, machine, RF channel, header and body |
| `--frames DIR --frame-log` | screens of all machines, with times |
| `--dump-ram FILE` | RAM of every machine at the end (`FILE.b`, `FILE.c`) |
| `--bot NICK` | a chat partner built on `cy_radio` (ID `4c0000b0`): pings, prints chat lines and other messages it hears on RF channel 4, answers messages to it |
| `--bot-room N`, `--bot-say MS:TEXT`, `--bot-tell MS:ID:TEXT` | ... says TEXT in room N, or privately to Cybiko ID |

A Cybiko needs its first-run questions answered and Chat opened before it
chats. With the original dumps this does it, typing the nickname "alice"
and opening the General room by about 112 s:

```
P=""; for t in 34000 40000 46000; do P="$P --press enter@$t"; done
t=55000; for k in a l i c e; do P="$P --press $k@$t"; t=$((t+500)); done
for t in 60000 66000 72000 78000 84000; do P="$P --press enter@$t"; done
P="$P --press right@95000 --press enter@96000 --press enter@101000"
P="$P --press enter@106000 --press enter@112000"
cybiko_host --rom cyrom150.bin --flash cyos_v1508.bin --disk-size 4096 \
    --seconds 140 $P --press enter@116000 \
    --bot ESP32 --bot-say "118000:Hello from an ESP32" \
    --bot-tell "125000:4c8051a3:psst, private hello"
```

Enter at 116 s posts Chat's ready-made "Hi, everybody!", which the bot
prints; the bot's two lines appear on the Cybiko's screen as
`ESP32▶ Hello from an ESP32` and `ESP32▶ psst, private hello`. Add `b:`
copies of the presses (with another nickname) and `--pair DISK2` for two
Cybikos.

## Open questions

- A bridge to real Cybikos: the AVR's air protocol is not known (see
  [the open questions in cybiko-docs](https://github.com/SovGVD/cybiko-docs/blob/main/radio-protocol.md#12-open-questions)),
  so a real Cybiko could only be reached by driving a real Cybiko's AVR and
  RF chip from an ESP32's serial port, with the serial protocol in
  [hardware.md](https://github.com/SovGVD/cybiko-docs/blob/main/hardware.md#11-radio).
