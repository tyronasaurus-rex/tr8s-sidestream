# TR-8S SideStream (spike)

Minimal **VST3 instrument** that keeps Ableton on **Tascam Model 12 ASIO** while capturing **Roland TR-8S** USB audio via **WASAPI**, exposing **12 mono output buses** (INDIV stems) into Live.

This is an MVP / spike — not Overbridge. No MIDI editor, no total recall, no Roland proprietary protocol reverse-engineering.

**Version:** 0.2.0 — multi-endpoint aggregation (`TR-8S (all stems)`)

## Requirements

- Windows 10/11 (x64)
- JUCE 8 (this machine: `C:\Program Files\JUCE` 8.0.9)
- CMake ≥ 3.22 + Visual Studio 2022 Build Tools
- Roland TR-8S Windows driver
- Ableton Live (VST3)

## Build (Release VST3)

```bat
cd /d C:\Users\tbkea\OneDrive\Documents\Development\VSTS\tr8s-sidestream
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --target TR8SSideStream_VST3
```

**Close Ableton** before overwriting `C:\Program Files\Common Files\VST3\…` (Live locks the DLL).

| Artifact | Path |
| --- | --- |
| Build tree | `build\TR8SSideStream_artefacts\Release\VST3\TR-8S SideStream.vst3` |
| Auto-copy | `%LOCALAPPDATA%\Programs\Common\VST3\TR-8S SideStream.vst3` |
| **Ableton install** | `C:\Program Files\Common Files\VST3\TR-8S SideStream.vst3` |

Bundle layout:

```text
TR-8S SideStream.vst3\
  Contents\
    Resources\moduleinfo.json
    x86_64-win\TR-8S SideStream.vst3
```

## Install for Ableton

1. Copy Release bundle → `C:\Program Files\Common Files\VST3\TR-8S SideStream.vst3` (elevated; close Live first).
2. Preferences → Plug-Ins → **Use VST3 Plug-In System Folders** on → rescan.
3. Load from **Instruments** onto a **MIDI track** (plugin exposes a MIDI/event bus; MIDI is ignored).

## How capture works on this machine

Roland exposes TR-8S as **six stereo WASAPI endpoints**, not one 12-ch device:

| Endpoint | Mapped stems |
| --- | --- |
| IN 1-2 (TR-8S) | BD, SD |
| IN 3-4 (TR-8S) | LT, HT |
| IN 5-6 (TR-8S) | CH, OH |
| IN 7-8 (TR-8S) | CY, RC |
| IN 9-10 (TR-8S) | CC, IN10 |
| IN 11-12 (TR-8S) | IN11, EXT |
| IN MIX (TR-8S) | **Not** opened by aggregate (optional single-device test only) |

**`TR-8S (all stems)`** (top of device list) opens all six pairs via **shared** WASAPI and feeds the 12 mono outs. Exclusive-mode duplicates are ignored for the aggregate (still listed for manual tests).

## Ableton routing (12 outs)

1. Live ASIO = **Model 12**; TR-8S USB Audio = **INDIV**; sample rates matched (Roland panel = Live).
2. MIDI track → **TR-8S SideStream**.
3. UI → select **`TR-8S (all stems)`** → **Open**. Status should show 6 pairs / 12 capture channels.
4. Create Audio tracks → **Audio From** → `TR-8S SideStream` → buses BD…EXT.
5. Confirm distinct instruments by playing one TR-8S part at a time and watching meters.

## Troubleshooting

| Symptom | Check |
| --- | --- |
| “could not be opened” | Need v0.1.1+ (MIDI event bus). See Log.txt for `event input bus`. |
| Aggregate missing from list | Refresh; confirm all six `IN n-(n+1) (TR-8S)` exist under shared WASAPI. |
| Only 2 meters move | You opened a single stereo endpoint — use **all stems**. |
| Partial pairs in status (`IN 7-8!`) | That endpoint failed to open; close other apps using it; retry. |

Log: `%APPDATA%\Ableton\Live 12.3\Preferences\Log.txt`

## Limits (spike)

| Item | Status |
| --- | --- |
| 12 stems via aggregated stereo WASAPI | **0.2.0** |
| MIX in aggregate | Off by default |
| Drift / SRC between pairs & host | Stubbed — match sample rates |
| Inter-pair sample sync | Best-effort (independent WASAPI clients) |
| MIDI / Overbridge editor | Out of scope |

## Next priorities

1. Confirm distinct INDIV stems in Live with Model 12 ASIO held.
2. Adaptive SRC / drift across the six capture clocks + host.
3. Optional Engine process; Ableton 12-track template.

## License / legal

Public WASAPI + installed Roland driver only. Not affiliated with Roland or Elektron.
