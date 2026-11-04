# TR-8S SideStream (spike)

Minimal **VST3 instrument** that keeps Ableton on **Tascam Model 12 ASIO** while capturing **Roland TR-8S** USB audio via **WASAPI**, exposing **up to 12 mono output buses** (INDIV stems) into Live.

This is an MVP / spike — not Overbridge. No MIDI editor, no total recall, no Roland proprietary protocol reverse-engineering.

**Version:** 0.1.1

## Requirements

- Windows 10/11 (x64)
- JUCE 8 (this machine: `C:\Program Files\JUCE` 8.0.9)
- CMake ≥ 3.22 + Visual Studio 2022 Build Tools (or VS 2026 with VS17 generator)
- Roland TR-8S Windows driver
- Ableton Live (any recent version with VST3)

## Build (Release VST3)

```bat
cd /d C:\Users\tbkea\OneDrive\Documents\Development\VSTS\tr8s-sidestream
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --target TR8SSideStream_VST3
```

Close Ableton before overwriting `C:\Program Files\Common Files\VST3\…` (Live locks the DLL).

Outputs:

| Artifact | Path |
| --- | --- |
| Build tree | `build\TR8SSideStream_artefacts\Release\VST3\TR-8S SideStream.vst3` |
| Auto-copy | `%LOCALAPPDATA%\Programs\Common\VST3\TR-8S SideStream.vst3` |
| **Ableton install (preferred)** | `C:\Program Files\Common Files\VST3\TR-8S SideStream.vst3` |

Bundle layout (required):

```text
TR-8S SideStream.vst3\
  Contents\
    Resources\moduleinfo.json
    x86_64-win\TR-8S SideStream.vst3   ← x64 PE DLL
```

Also builds a **Standalone** target (`TR8SSideStream_Standalone`) for device/meter dry-runs without Live.

## Install for Ableton

1. Copy the Release bundle to **`C:\Program Files\Common Files\VST3\TR-8S SideStream.vst3`** (admin / elevated Explorer or PowerShell). Close Live first if replacing an existing install.
2. Ableton → Preferences → Plug-Ins → **Use VST3 Plug-In System Folders** = on.
3. Rescan plug-ins (or restart Live). Find **TR-8S SideStream** under **Instruments** (vendor Tyron).

### Track type (important)

This plugin is a **VST3 Instrument** (synth category) with a MIDI/event input bus (MIDI is ignored; audio comes from WASAPI).

| Do | Don’t |
| --- | --- |
| Drag onto a **MIDI track** | Put it only in Audio Effect slots |
| Or create MIDI track → Instruments → TR-8S SideStream | Expect it to behave as an insert FX |

Ableton Log error if MIDI event bus is missing (fixed in 0.1.1):

`Vst3: plugin has instrument category, but no valid event input bus`

## TR-8S + Model 12 prep

1. TR-8S: `UTILITY → GENERAL → USB Audio → **INDIV**`.
2. Windows: **Start → Roland → TR-8S** → set sample rate to match Live (e.g. **48 kHz**).
3. Ableton Audio: ASIO driver = **Tascam Model 12**, same sample rate.
4. Prefer separate USB ports/controllers for Model 12 and TR-8S.

## Ableton routing (12 outs)

1. Keep Live’s audio device on **Model 12**.
2. On a **MIDI track**, load **TR-8S SideStream**.
3. Open the plugin UI → **Refresh** → pick a TR-8S WASAPI device:
   - Prefer names containing **MULTI** (Audacity’s 12-channel path).
   - `IN 1-2` … `IN 11-12` are **stereo pair** endpoints (2 channels each) — useful for testing, not full stems in one instance.
4. Click **Open**. Status line should show capture channel count, rate, and PDC latency.
5. Create up to **11 additional Audio tracks**.
6. On each Audio track: **Audio From** → `TR-8S SideStream` → choose stem bus (`BD`, `SD`, … `EXT`).
   - Same multi-out pattern as Overbridge / Kontakt.
7. Arm/monitor the Audio tracks as needed. Main MIDI track carries bus 1 (`BD`); other buses are tapped from sibling Audio tracks.

If Live widens mono buses to stereo, the plugin duplicates the mono stem to both L/R.

## Troubleshooting load failures

1. Check `C:\Users\tbkea\AppData\Roaming\Ableton\Live 12.3\Preferences\Log.txt` for `VST3: Failed` / `event input bus`.
2. Confirm install path is the **bundle** under `Common Files\VST3`, not a lone DLL at the folder root.
3. Confirm binary is **x64** (`Contents\x86_64-win\…`).
4. Close Live before replacing the bundle; then rescan.
5. Version in `Contents\Resources\moduleinfo.json` should be **0.1.1** or newer.

## What works / known limits (spike)

| Item | Status |
| --- | --- |
| Loadable VST3 Instrument in Ableton MIDI track | 0.1.1 (event bus fix) |
| WASAPI open while Live holds Model 12 ASIO | Expected (per-endpoint exclusivity) |
| Device picker + last-device recall (plugin state) | Yes |
| Ring buffer capture → `processBlock` | Yes |
| 12 mono out buses + meters | Yes |
| PDC latency reported to host | Approx. (device + ring pad) |
| Drift / SRC compensation | **Stubbed** — match sample rates |
| Aggregating six stereo endpoints into one 12-ch stream | **Not in spike** |
| MIDI processing / Overbridge editor | Out of scope (MIDI bus present but ignored) |

**Channel distinctness:** Confirm by playing one TR-8S instrument at a time and watching meters / Live tracks.

## Dry-run without TR-8S

1. Build Standalone or VST3.
2. Launch Standalone → Refresh → you should still see other WASAPI inputs.
3. Open any input; meters / ring stats should move with signal.
4. In Live: load the instrument on a MIDI track, confirm 12 out buses appear in routing.

## Next engineering priorities

1. **Prove MULTI channel map** — distinct BD/SD/… while Live holds Model 12.
2. **Multi-endpoint aggregator** — if only stereo pairs expose stems.
3. **Drift compensator** — adaptive SRC for long jams.
4. **Safer Engine process** — optional out-of-process capture.
5. **Ableton rack preset** — pre-wired 12-track template.

## License / legal

Uses public Windows WASAPI + installed Roland driver only. Not affiliated with Roland or Elektron. Check Roland driver EULA before redistributing commercially.
