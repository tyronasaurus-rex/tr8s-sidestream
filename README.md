# TR-8S SideStream (spike)

Minimal **VST3 instrument**: Ableton keeps **Model 12 ASIO**; plugin captures **TR-8S** via **WASAPI** as **12 mono outs**.

**Version:** 0.3.0 — aggregate stems + deeper FIFO / drift slip-pad / UI depth control

## Build / install

```bat
cd /d C:\Users\tbkea\OneDrive\Documents\Development\VSTS\tr8s-sidestream
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --target TR8SSideStream_VST3
```

Close Ableton, then install to:

`C:\Program Files\Common Files\VST3\TR-8S SideStream.vst3`

## Ableton vs plugin buffers (0.3.0 defaults)

| Setting | Recommended start |
| --- | --- |
| Live sample rate | **48 kHz** (match **Start → Roland → TR-8S**) |
| Live buffer / device | **512 samples** (256 OK if CPU allows; 1024 if still crunchy) |
| Plugin **WASAPI buf** | **512** (match Live; reopen after change) |
| Plugin **Depth ms** | **100** (PDC ≈ depth + WASAPI period; raise to 150–200 if underruns climb) |

**How to open:** MIDI track → TR-8S SideStream → **`TR-8S (all stems)`** → set depth/WASAPI → **Open**. Wait for status `primed yes` before judging audio.

### What the depth control does

- Pre-roll: no audio until every open pair has ~depth ms buffered.
- Drift: if a pair runs ahead, samples are **slipped** (dropped) back to the waterline; if behind, that pair soft-underruns (silence pad) without blocking `process()`.
- Overrun path discards oldest audio so the ring keeps recent signal.

Underrun/overrun/slip counters reset when priming completes; slow growth can still happen (six independent WASAPI clocks). Sustained climb → increase Depth or Live buffer.

## Routing

1. TR-8S USB Audio = **INDIV**; rates matched.
2. Aggregate opens `IN 1-2`…`IN 11-12` (shared WASAPI); **MIX** off.
3. Audio tracks → Audio From → plugin buses BD…EXT.

## Changelog (spike)

| Ver | Fix |
| --- | --- |
| 0.1.1 | Ableton Instrument event (MIDI) bus |
| 0.2.0 | Multi-endpoint aggregate |
| 0.2.1 | `AudioDeviceManager::initialise` before open |
| 0.3.0 | Larger rings, pre-roll, aligned WASAPI period, slip/pad drift, UI depth |

## License

Public WASAPI + Roland driver only. Not affiliated with Roland/Elektron.
