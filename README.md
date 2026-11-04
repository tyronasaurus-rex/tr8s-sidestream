# TR-8S SideStream (spike)

**Version:** 0.4.1

## Honest architecture note

Opening **six independent WASAPI capture clients** inside a Live-hosted VST is fragile (callback jitter, shared-mode quantum, dual clocks). 0.4.1 fixes real bugs that caused **immediate** underrun ratchets; long-term stability may still need exclusive mode, larger periods, or an out-of-process engine / different I/O strategy.

## Critical fixes in 0.4.1

1. **SPSC bug**: capture thread was `discard()`-ing (moving `readIndex`) on overrun → raced `process()` and corrupted FIFO.
2. **SRC bug**: `readResampled` used all-or-nothing “need N samples or underrun+read zero”; one late pair underran **every block**. Now ratio is clamped to available so reads always progress when `fifo>2`.
3. Diagnostics: per-pair `fifo=`, `cb/s=`, `wr=` (samples written/sec).
4. Aggregate option: **`TR-8S (all stems, exclusive)`**.
5. Defaults: Depth **150 ms**, WASAPI **1024**.

## What Tyron should set / look for

| | |
|---|---|
| Live + Roland | 48 kHz |
| Ableton buffer | **1024** (or 512 if CPU OK) |
| Plugin WASAPI | **1024** |
| Plugin Depth | **150** |
| Device | try shared aggregate first; if `cb/s` uneven, use **exclusive** |

After Open, status should show all six pairs with **`cb/s` roughly similar and `>0`**, `wr≈48000`, `fifo` near mid — not stuck at 0 and not `DEAD`.

If any pair shows `cb/s=0` / `DEAD` while others run → that endpoint isn’t calling back (driver/scheduling); architecture rethink.

## Build / install

Close Live, then:

```bat
cmake --build build --config Release --target TR8SSideStream_VST3
```

→ `C:\Program Files\Common Files\VST3\TR-8S SideStream.vst3`
