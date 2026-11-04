# TR-8S SideStream (spike)

Ableton keeps **Model 12 ASIO**; plugin captures **TR-8S** WASAPI stems as **12 mono outs**.

**Version:** 0.4.0 — adaptive fractional SRC toward FIFO midpoint (multi-clock)

## Open / buffers

| Setting | Start here |
| --- | --- |
| Live + Roland panel | **48 kHz** |
| Ableton buffer | **512** (1024 if CPU/WASAPI still rough) |
| Plugin **WASAPI buf** | **512** (re-Open after change) |
| Plugin **Depth ms** | **120** (100–150 typical; raises midpoint waterline) |

1. MIDI track → **TR-8S SideStream** → **`TR-8S (all stems)`** → Open  
2. Wait for **`primed yes`**  
3. Status `SRC ≈ 1.00x` and pair line like `IN 1-2=1.001*` (`*` = master). `x` = dead callback.

Install: `C:\Program Files\Common Files\VST3\TR-8S SideStream.vst3` (close Live first).

## What 0.4.0 changed

- **Adaptive SRC**: each pair linear-resamples with a PI controller that holds FIFO near **~60% of Depth** (not just slip when empty).
- **Master pair** (prefer IN 1-2): hosts the reference ratio; slaves blend 50% toward master so all track one clock vs Live.
- **Dead-callback watchdog**: if a pair stops writing (~150 ms), it is muted (`x`) and no longer ratchets underruns.
- Pre-roll still fills to midpoint before audio; coarse slip only for emergency overflow.

## Build

```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --target TR8SSideStream_VST3
```
