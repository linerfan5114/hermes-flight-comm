# Hermes Flight Comm

A host-based software simulation of concepts used in redundant,
secure spacecraft/EVA-suit intercom systems: adaptive noise
cancellation, authenticated encryption, triple-modular-redundancy
(TMR) fault tolerance, and a simulated multi-peer wireless mesh.

## What this actually is (please read)

This project runs as a normal process on your computer — it does
**not** run on flight hardware, has **not** been certified to any
aerospace software standard (e.g. MISRA-C, DO-178C), and has **not**
been tested on radiation-tolerant silicon. Those are real, expensive
engineering/certification processes that require specialized tools
and, in most cases, a company or agency behind them.

What it *does* do is implement the underlying algorithms for real and
correctly, as a learning project:

| Concept | How it's really implemented here |
|---|---|
| Noise-cancelling audio | A genuine adaptive LMS (least-mean-squares) filter that learns to subtract a reference noise signal from a primary mic signal, sample by sample. |
| Authenticated encryption | AES-128-CBC + HMAC-SHA256 (Encrypt-then-MAC), implemented from the public FIPS-197 / FIPS-180-4 specifications. |
| Triple Modular Redundancy | A real majority-vote implementation: a computation runs three times against independent copies of its input (with optional simulated bit-flip fault injection), and the voter picks the majority result. |
| Wireless mesh | Simulated using UDP sockets on localhost — each "node" is a process bound to its own port, sending to a list of known peer ports. This mirrors how ESP-NOW's peer-list model works closely enough that porting to real ESP32 hardware later would mean swapping this layer for an ESP-NOW driver, without changing the rest of the code. |
| Concurrent tasks | Not yet implemented with a real RTOS — the demo runs single-threaded for clarity. A future iteration could move to FreeRTOS on real ESP32 hardware. |

## Building

Requires a C99 compiler. No external dependencies.

```bash
mkdir build && cd build
cmake ..
make
./hermes_demo
ctest        # or: ./test_hermes
```

## Project layout

```
hermes-flight-comm/
├── CMakeLists.txt
├── include/
│   └── hermes.h        # all public declarations
├── third_party/
│   ├── aes.h / aes.c    # AES-128 (FIPS-197)
│   └── sha256.h / sha256.c  # SHA-256 + HMAC (FIPS-180-4 / RFC 2104)
├── src/
│   ├── audio.c          # audio pipeline + LMS noise filter
│   ├── crypto.c          # Encrypt-then-MAC authenticated encryption
│   ├── tmr_vote.c         # triple modular redundancy voter
│   ├── mesh_net.c         # UDP-simulated mesh networking
│   ├── node.c             # node state machine tying it all together
│   └── main.c             # demo: two nodes exchanging an encrypted frame
└── tests/
    └── test_hermes.c      # unit tests for each component
```

## What the demo shows

1. Node A generates a synthetic voice signal mixed with noise, and a
   copy of the noise as a "reference" signal (like a second mic aimed
   away from the wearer's mouth).
2. The LMS filter removes the noise, encrypts the result, and sends
   it to Node B over the simulated mesh.
3. Node B authenticates and decrypts the packet and recovers the
   audio.
4. Separately, a TMR demo shows a computation surviving a simulated
   single-event upset (bit flip) via majority voting.

## Known limitations / honest caveats

- The crypto uses a fixed demo key baked into `main.c` — a real
  system would need a proper key-exchange/provisioning step.
- The IV derivation is a simple incrementing counter; safe as long as
  a key is never reused across sessions, but a production system
  would mix in a random per-session salt too.
- This has not been audited by a cryptographer. It's built to
  demonstrate the *pattern* (Encrypt-then-MAC) correctly, not as a
  drop-in secure-communications library.
- No real RF layer, no real embedded target yet — see the table above.

## Possible next steps

- Port `mesh_net.c` to real ESP-NOW on an ESP32/ESP32-S3 dev board.
- Replace the single-threaded demo loop with FreeRTOS tasks.
- Add a proper key-exchange handshake instead of a hardcoded key.
