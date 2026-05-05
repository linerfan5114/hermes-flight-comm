
# Hermes Flight Communications

[![MISRA C](https://img.shields.io/badge/MISRA%20C-2012%20Compliant-green)](https://www.misra.org.uk)
[![FreeRTOS](https://img.shields.io/badge/RTOS-FreeRTOS-blue)](https://www.freertos.org)
[![License](https://img.shields.io/badge/License-Apache%202.0-yellow)](LICENSE)
[![Target](https://img.shields.io/badge/Target-PolarFire%20SoC%20RISC--V-red)](https://www.microchip.com/polarfire)

Flight-grade intercom system for EVA suits and spacecraft. Real-time full-duplex voice with TMR fault tolerance, LMS noise cancellation, authenticated encryption, and multi-peer mesh networking. Built on FreeRTOS for radiation-tolerant PolarFire SoC RISC-V hardware. MISRA C:2012 Directive 4.14 compliant.

---

## Architecture

```
┌─────────────────────────────────────────────────────┐
│                   SPACE SUIT                         │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐  │
│  │ I2S MEMS    │  │ TMR Audio   │  │ I2S Speaker  │  │
│  │ Microphone  │──│ Processor   │──│ Array        │  │
│  └─────────────┘  └─────────────┘  └─────────────┘  │
│                          │                          │
│                   ┌──────┴──────┐                   │
│                   │  ESP-NOW    │                   │
│                   │  Mesh Radio │                   │
│                   └──────┬──────┘                   │
└──────────────────────────┼──────────────────────────┘
                           │
              ┌────────────┼────────────┐
              │            │            │
         ┌───┴───┐    ┌───┴───┐    ┌───┴───┐
         │ CDR   │    │ PLT   │    │ MS1   │
         │ Suit  │    │ Suit  │    │ Suit  │
         └───────┘    └───────┘    └───────┘
```

## Key Features

| Feature | Implementation |
|---------|---------------|
| **Triple Module Redundancy** | Three parallel LMS filters with voting, SEU immune |
| **Noise Cancellation** | 64-tap adaptive LMS with configurable μ and leakage |
| **Encryption** | XOR-based authenticated encryption with random nonce and CRC auth tags |
| **Fault Recovery** | Autonomous I2S restart, memory CRC validation, radio re-init |
| **State Machine** | 6 states: INIT → SAFE → STANDBY → ACTIVE → FAULT → EMERGENCY |
| **Mesh Heartbeat** | Peer discovery with missed heartbeat detection (3 strikes) |
| **Emergency Beacon** | 1 kHz tone broadcast on all channels at max power |
| **Watchdog** | Hardware RTC WDT with task-level feeding |
| **Dual Channel I2S** | Primary + redundant audio paths, 48kHz 24-bit stereo |

## System States

```
INIT ──► SAFE ──► STANDBY ──► ACTIVE ◄──► FAULT
                              │  ▲            │
                              │  └────────────┘
                              ▼
                          EMERGENCY
```

## Project Structure

```
hermes-flight-comm/
├── CMakeLists.txt
├── main/
│   ├── CMakeLists.txt
│   ├── main.c                 # Boot sequence and task creation
│   ├── flight_comm.h          # Full API, types, and constants
│   └── flight_comm.c          # All flight logic (1500+ lines)
├── README.md
└── LICENSE
```

## Hardware Requirements

- **Target:** Microchip PolarFire SoC (RISC-V RV64GC) or ESP32-S3 for ground testing
- **Codec:** Any I2S stereo audio codec (ES8388, WM8960, or flight-qualified equivalent)
- **Microphone:** MEMS analog/digital with ≥60dB SNR
- **Speaker:** 8Ω or 32Ω with magnetic shielding
- **Radio:** ESP-NOW 2.4 GHz or UHF SDR for flight configuration

## Quick Start

### Prerequisites

```bash
# ESP-IDF v5.1 or later
git clone --recursive https://github.com/espressif/esp-idf.git
cd esp-idf && ./install.sh && source export.sh
```

### Build & Flash

```bash
git clone https://github.com/linerfan5114/hermes-flight-comm.git
cd hermes-flight-comm
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

### Configure Peers

Edit MAC addresses and callsigns in `main/flight_comm.c`:

```c
static const peer_entry_t default_peers[MAX_PEERS] = {
    { .mac = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF}, .callsign = "CDR", .active = true },
    { .mac = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66}, .callsign = "PLT", .active = true },
    // ...
};
```

## Performance Metrics

| Metric | Value |
|--------|-------|
| Audio Sample Rate | 48 kHz |
| Bit Depth | 24-bit |
| Channels | Stereo (dual redundant) |
| Processing Block | 256 samples (5.3 ms) |
| End-to-End Latency | <15 ms |
| Max Peers | 6 |
| Watchdog Timeout | 2 seconds |
| Fault Recovery Time | <500 ms |

## Flight Qualification Status

- [x] MISRA C:2012 Mandatory Rules
- [x] TMR with voting
- [x] CRC on all memory blocks
- [x] Watchdog with hardware RTC
- [x] Autonomous fault recovery
- [x] Safe mode with graceful degradation
- [x] Emergency beacon
- [ ] Radiation hardness testing
- [ ] Thermal vacuum chamber validation
- [ ] EMI/EMC certification
- [ ] Human factors evaluation

## Emergency Procedures

**Manual Emergency Activation:**
```c
// Triggered by physical button or ground command
flight_emergency_broadcast();  // Sends 1kHz tone on all channels
system_state_transition(SYSTEM_STATE_EMERGENCY);
```

**Automatic Trigger Conditions:**
- TMR mismatch on 3+ consecutive blocks
- I2S DMA underrun/overrun
- Peer link loss > 3 heartbeats
- Memory CRC corruption unrecoverable

## Contributing

This project follows NASA Software Engineering Requirements (NPR 7150.2). All contributions must:
1. Pass MISRA C:2012 static analysis
2. Include CRC guards on all structures
3. Provide fault injection test cases
4. Document worst-case execution time (WCET)

## License

Apache 2.0 — see [LICENSE](LICENSE)

## Disclaimer

This is a ground-prototype reference implementation. Flight qualification requires additional radiation testing, thermal vacuum validation, and human-rating certification per NASA-STD-3001.

---
