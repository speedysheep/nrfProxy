# Device-control command (phone → nrfProxy over BLE)

The phone can control the nrfProxy device itself — drive a relay GPIO
(motor-enable) and change the UART baud rate — by writing a small command packet
to the **Nordic UART Service (NUS) RX characteristic**, the same characteristic
used for phone→serial data. These commands target the nrfProxy device; they are
**not** forwarded down UART1.

These are control commands for *this* device, not a motor-controller wire
protocol, which is why they live here and not in `ebike_protocols`.

## Transport

- Write the packet to the **NUS RX characteristic**
  (`6e400002-b5a3-f393-e0a9-e50e24dcca9e`) as a single GATT write.
- The link **must be encrypted first** (the pairing lock). Until the bond is in
  place and the link is encrypted, all NUS writes — including these commands —
  are dropped by the firmware. In normal app use this is already true.
- The device replies on the **NUS TX characteristic**
  (`6e400003-b5a3-f393-e0a9-e50e24dcca9e`) via a notification — so subscribe to
  TX notifications to receive the acknowledgement.

## Packet format (fixed 16 bytes)

Every command is the same fixed 16-byte frame. The width is fixed so the format
never has to grow again: a new control just takes a new **opcode** and reuses the
spare payload bytes.

| Offset  | Name     | Value                                             |
|---------|----------|---------------------------------------------------|
| `[0]`   | start    | `0x22` (fixed)                                    |
| `[1]`   | length   | `0x10` (16) — declared length, must equal the frame |
| `[2]`   | opcode   | which control (see below)                         |
| `[3..14]` | payload | opcode-specific; **unused bytes must be `0x00`** |
| `[15]`  | checksum | `sum(bytes[0..14]) & 0xFF`, 8-bit additive sum    |

The checksum only guards against corruption / accidental framing — the security
boundary is the encrypted BLE link, not this byte. Because `length` and the
checksum both cover the whole frame, a stray 16-byte chunk of ordinary serial
data is astronomically unlikely to be mistaken for a command.

### Opcodes

| Opcode | Name  | Payload                                                  |
|--------|-------|----------------------------------------------------------|
| `0x01` | RELAY | `payload[0]` = `0x00` disable / `0x01` enable the pin     |
| `0x02` | BAUD  | `payload[0..3]` = new UART baud, **uint32 little-endian** |

#### RELAY (`0x02` byte → `0x01`)

Drives the motor-enable relay GPIO. `payload[0]` is `0x00` (disable) or `0x01`
(enable); any other value is refused.

**The enable is tied to the BLE connection.** See
[Relay lifetime](#relay-lifetime-the-enable-is-tied-to-the-link-with-a-60-second-grace)
below — the firmware releases the relay 60 s after the link drops, unless the
phone reconnects and re-encrypts inside that window.

#### BAUD (`0x02`)

Reconfigures UART1's baud rate on the fly, for switching between motor
controllers that talk at different rates. `payload[0..3]` is the target baud as a
little-endian `uint32`. Only an **allow-listed** rate is accepted:

| Baud   | little-endian payload bytes (`[3] [4] [5] [6]`) |
|--------|--------------------------------------------------|
| 1200   | `B0 04 00 00`  (Bafang, slow but real)           |
| 9600   | `80 25 00 00`                                     |
| 19200  | `00 4B 00 00`                                     |
| 38400  | `00 96 00 00`                                     |
| 57600  | `00 E1 00 00`                                     |
| 115200 | `00 C2 01 00`                                     |

Any baud not on this list is refused (no pin/UART change, no ack). The set lives
in `proxy_baud_supported()` in `proxy_core.c` — extend it there (and this table)
if a controller needs another rate the nRF UARTE can realise.

The change quiesces RX/TX, reconfigures, and restarts reception; **any in-flight
or buffered UART bytes are dropped**, which is fine because a baud change means
the peer on the other end just changed too. The ack is sent **only after the new
rate is live**.

## Validation and dispatch

A write is treated as a command only if the framing validates: length is exactly
16, `[0]` is `0x22`, `[1]` is `0x10`, and `[15]` equals the checksum. Given that:

- **Framing invalid** → not ours: the write **falls through and is forwarded to
  UART1** as ordinary data (this is how a genuine 16-byte serial chunk that isn't
  a command still gets through).
- **Framing valid but opcode unknown or payload out of range** → the packet is
  **consumed and dropped** (a framed control packet is never injected into the
  motor UART), but **no action is taken and no ack is sent**.
- **Framing valid and actionable** → the action is performed and the packet is
  echoed back (see below).

The framing/validation is pure logic in `proxy_core.c` (`proxy_cmd_parse`,
host-tested); the GPIO drive, baud reconfigure, and ack live in `main.c`.

## Acknowledgement

On an actionable command the firmware performs the action and then **echoes the
exact same 16 bytes back** as a NUS TX notification. The phone confirms the
command took effect by matching the echo against what it sent. **No echo = not
applied** (link dropped, refused opcode/payload, or — for a baud change —
reconfiguration failed and the old rate is still live); re-send.

⚠️ The echo means *the firmware acted on the command*, not that hardware moved.
A RELAY command on a board whose overlay defines no `relay-control` alias is
logged and ignored by `relay_set()` — and still acked, because `cmd_ack_send()`
follows it unconditionally (`main.c`). Nothing downstream of the pin is verified
either. If a caller needs "there is a relay here", that has to become its own
opcode; it cannot be inferred from the ack.

## Relay lifetime: the enable is tied to the link, with a 60-second grace

The relay is held while the phone that authorised it is connected — **plus a
60-second grace window after the link drops**, so that ordinary radio
interference doesn't cut assist under a rider.

| Event | Relay |
|---|---|
| Boot / reset / power loss | **off** — always, unconditionally |
| RELAY command, `payload[0] = 0x01`, link encrypted | **on** |
| RELAY command, `payload[0] = 0x00` | **off**, immediately |
| BLE disconnect (any cause) | **stays on**, 60 s countdown starts |
| Reconnect + link encrypted, inside those 60 s | **stays on**, countdown cancelled |
| 60 s elapse with no encrypted link | **off** |

"Any cause" is literal: a clean disconnect from the app, the phone going out of
range or its battery dying (supervision timeout), or the security watchdog
dropping a link that never encrypted. They all arrive at `on_disconnected()` in
`main.c`, which arms `relay_grace_work`. The countdown is cancelled from
`on_security_changed()` — note that it takes an **encrypted** link to save the
relay, not merely a connection, so a link that reconnects but fails to pair still
loses it when the window expires.

The grace is a plain timer, not a persisted state: a reset inside the window
still comes up disabled, because the enable is never written to flash.

Three consequences worth designing the app around:

- **The app should re-send the enable after a reconnect** rather than assume the
  relay survived. It's harmless if it did (the command is idempotent, and it
  cancels any pending grace release), and necessary if the window had expired.
  The ack echo is what confirms the actual state.
- **A reconnect that stalls on pairing does not save the relay.** If the app has
  to re-bond, that has to complete inside the 60 s.
- 60 s is a deliberate choice, not the figure in `LOCK_PLAN.md` — that document
  specifies a 10-minute grace with a 30-minute backstop, for a different scenario
  (the owner walking away with the phone). See the Deviation note there for why
  this is shorter, and what it would take to implement the full version.

To change the window, edit `RELAY_GRACE_MS` in `main.c`; nothing else depends on
the value.

## Pin per board (RELAY opcode)

Boot state is always **disabled** (pin inactive); the motor is never enabled on
its own. The pin is driven with *logical* levels, so the electrical polarity is
set by the `GPIO_ACTIVE_HIGH`/`GPIO_ACTIVE_LOW` flag in the board overlay's
`relay-control` node — change it to `GPIO_ACTIVE_LOW` for an active-low
(opto-isolated) relay module.

| Board                       | Relay pin | Notes                                  |
|-----------------------------|-----------|----------------------------------------|
| nRF52840 DK                 | **P1.04** | free header pin                        |
| Seeed XIAO BLE              | **P0.29** | D3 pad (only remaining free pad)       |
| Pro Micro / nice!nano       | **P0.20** | free broken-out pad                    |
| nRF52840 Dongle             | **P1.10** | free castellated pad                   |

All default to active-high: pin high = relay energised = motor enabled. A board
overlay may omit the `relay-control` alias, in which case that board has no relay:
a RELAY command is logged, ignored, **and still acknowledged** (see the warning
under [Acknowledgement](#acknowledgement)). A BAUD command works on every board —
it needs no board GPIO.

## Quick test with nRF Connect for Mobile

1. Connect and pair with the device (the app/central drives pairing).
2. Subscribe to the NUS **TX** characteristic (enable notifications).
3. **Enable relay:** write (HEX)
   `22 10 01 01 00 00 00 00 00 00 00 00 00 00 00 34` to the NUS **RX**
   characteristic → the relay pin goes high and the same 16 bytes come back on
   TX. (`0x34` = `0x22 + 0x10 + 0x01 + 0x01`.)
4. **Disable relay:** write
   `22 10 01 00 00 00 00 00 00 00 00 00 00 00 00 33` → pin goes low, echoed back.
5. **Set 9600 baud:** write
   `22 10 02 80 25 00 00 00 00 00 00 00 00 00 00 D9` → UART1 switches to 9600 and
   the frame is echoed. (`0xD9` = `0x22 + 0x10 + 0x02 + 0x80 + 0x25`.)
6. **Check the grace window:** enable the relay again (step 3), then
   **disconnect** in nRF Connect. The pin must **stay high**, with
   `Link lost; holding relay 60 s for a reconnect` in the log — then go low about
   a minute later with `Relay grace expired; controller disabled`.
7. **Check the reconnect saves it:** repeat step 6, but reconnect and re-pair
   within the minute. The pin never drops, and the log shows no expiry line.
   (Cancelling is bound to *encryption*, not connection — reconnecting without
   completing pairing will still lose the relay at the deadline.)
