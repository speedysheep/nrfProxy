# Relay-control command (phone → nrfProxy over BLE)

The phone can toggle a GPIO on the nrfProxy board — intended to drive a relay
that enables/disables the motor controller — by writing a small command packet
to the **Nordic UART Service (NUS) RX characteristic**, the same characteristic
used for phone→serial data. The command targets the nrfProxy device itself; it
is **not** forwarded down UART1.

This is a control command for *this* device, not a motor-controller wire
protocol, which is why it lives here and not in `ebike_protocols`.

## Transport

- Write the packet to the **NUS RX characteristic**
  (`6e400002-b5a3-f393-e0a9-e50e24dcca9e`) as a single GATT write.
- The link **must be encrypted first** (the pairing lock). Until the bond is in
  place and the link is encrypted, all NUS writes — including this command — are
  dropped by the firmware. In normal app use this is already true.
- The device replies on the **NUS TX characteristic**
  (`6e400003-b5a3-f393-e0a9-e50e24dcca9e`) via a notification — so subscribe to
  TX notifications to receive the acknowledgement.

## Packet format (3 bytes)

| Offset | Name     | Value                                   |
|--------|----------|-----------------------------------------|
| `[0]`  | start    | `0x22` (fixed)                          |
| `[1]`  | state    | `0x00` = disable, `0x01` = enable       |
| `[2]`  | checksum | `(start + state) & 0xFF`, 8-bit sum     |

The checksum only guards against corruption / accidental framing — the security
boundary is the encrypted BLE link, not this byte.

### The only two valid packets

| Action          | Bytes (hex)   |
|-----------------|---------------|
| **Enable** pin  | `22 01 23`    |
| **Disable** pin | `22 00 22`    |

A packet is acted on only if **all** of these hold: length is exactly 3, byte 0
is `0x22`, byte 1 is `0x00` or `0x01`, and byte 2 equals the checksum. Anything
else (wrong length, wrong start byte, bad checksum, out-of-range state) is
**not** treated as a relay command — it falls through and is forwarded to UART1
as ordinary data.

## Acknowledgement

On a valid command the firmware drives the pin and then **echoes the exact same
3 bytes back** as a NUS TX notification. The phone confirms the command took
effect by matching the echo against what it sent:

- Sent `22 01 23` (enable) → receive `22 01 23` back once the pin is high.
- Sent `22 00 22` (disable) → receive `22 00 22` back once the pin is low.

If no acknowledgement arrives, the command was not applied (e.g. link dropped,
or no relay GPIO on that board) — re-send.

## Pin per board

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
overlay may omit the `relay-control` alias, in which case that board has no relay
and a relay command is acknowledged only after being logged/ignored (no pin to
drive → the command is refused and no ack is sent).

## Quick test with nRF Connect for Mobile

1. Connect and pair with the device (the app/central drives pairing).
2. Subscribe to the NUS **TX** characteristic (enable notifications).
3. Write `22 01 23` (HEX) to the NUS **RX** characteristic → the relay pin goes
   high and `22 01 23` comes back on TX.
4. Write `22 00 22` → pin goes low, `22 00 22` comes back.
