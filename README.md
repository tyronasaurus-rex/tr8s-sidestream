# TR-8S SideStream (spike)

Minimal **VST3 instrument** that keeps Ableton on **Tascam Model 12 ASIO** while capturing **Roland TR-8S** USB audio via **WASAPI**, exposing **up to 12 mono output buses** (INDIV stems) into Live.

This is an MVP / spike — not Overbridge. No MIDI editor, no total recall, no Roland proprietary protocol reverse-engineering.

## Requirements

- Windows 10/11
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

Outputs:

| Artifact | Path |
| --- | --- |
| Build tree | `build\TR8SSideStream_artefacts\Release\VST3\TR-8S SideStream.vst3` |
| Auto-copy | `%LOCALAPPDATA%\Programs\Common\VST3\TR-8S SideStream.vst3` |

Also builds a **Standalone** target (`TR8SSideStream_Standalone`) useful for device/meter dry-runs without Live.

## Install for Ableton

1. Confirm the `.vst3` bundle exists under `%LOCALAPPDATA%\Programs\Common\VST3\` (or copy the build-tree bundle into `C:\Program Files\Common Files\VST3\`).
2. Ableton → Preferences → Plug-Ins → ensure **Use VST3 Plug-In System Folders** is on (and/or add the LocalAppData VST3 folder as a custom path).
3. Rescan plug-ins. Find **TR-8S SideStream** under Instruments / Tyron.

## TR-8S + Model 12 prep

1. TR-8S: `UTILITY → GENERAL → USB Audio → **INDIV**`.
2. Windows: **Start → Roland → TR-8S** → set sample rate to match Live (e.g. **48 kHz**).
3. Ableton Audio: ASIO driver = **Tascam Model 12**, same sample rate.
4. Prefer separate USB ports/controllers for Model 12 and TR-8S.

## Ableton routing (12 outs)

1. Keep Live’s audio device on **Model 12**.
2. On a MIDI track, load **TR-8S SideStream**.
3. Open the plugin UI → **Refresh** → pick a TR-8S WASAPI device:
   - Prefer names containing **MULTI** (Audacity’s 12-channel path).
   - `IN 1-2` … `IN 11-12` are **stereo pair** endpoints (2 channels each) — useful for testing, not full stems in one instance.
4. Click **Open**. Status line should show capture channel count, rate, and PDC latency.
5. Create up to **11 additional Audio tracks**.
6. On each Audio track: **Audio From** → `TR-8S SideStream` → choose stem bus (`BD`, `SD`, … `EXT`).
   - Ableton may show these as Instrument multi-outs; enable “Instrument’s multi-out” / “Ext. Out” style routing as with Overbridge/Kontakt multi-out.
7. Arm/monitor the Audio tracks as needed. Main MIDI track carries bus 1 (`BD`); other buses are tapped from sibling Audio tracks.

If Live widens mono buses to stereo, the plugin duplicates the mono stem to both L/R.

## What works / known limits (spike)

| Item | Status |
| --- | --- |
| Loadable VST3 + Standalone | Goal of this spike |
| WASAPI open while Live holds Model 12 ASIO | Expected (per-endpoint exclusivity) |
| Device picker + last-device recall (plugin state) | Yes |
| Ring buffer capture → `processBlock` | Yes |
| 12 mono out buses + meters | Yes |
| PDC latency reported to host | Approx. (device + ring pad) |
| Drift / SRC compensation | **Stubbed** — match sample rates |
| Aggregating six stereo endpoints into one 12-ch stream | **Not in spike** |
| MIDI / Overbridge editor | Out of scope |

**Channel distinctness:** Confirm by playing one TR-8S instrument at a time and watching meters / Live tracks. If MULTI only carries a mix, or stereo pairs don’t map 1:1 to INDIV, note that for the next engineering pass.

## Dry-run without TR-8S

1. Build Standalone or VST3.
2. Launch Standalone → Refresh → you should still see other WASAPI inputs.
3. Open any input; meters / ring stats should move with signal.
4. In Live (even without TR-8S): load the instrument, confirm 12 out buses appear in routing.

## Next engineering priorities

1. **Prove MULTI channel map** — record distinct BD/SD/… while Live holds Model 12; document exact WASAPI endpoint that yields 12 discrete stems.
2. **Multi-endpoint aggregator** — if only stereo pairs expose stems, open `IN 1-2`…`IN 11-12` together and interleave into 12 mono outs.
3. **Drift compensator** — adaptive SRC between TR-8S capture clock and Model 12/host clock for long jams.
4. **Safer Engine process** — optional out-of-process capture (Overbridge-like) for crash isolation.
5. **Ableton rack preset** — pre-wired 12-track template.
6. **Exclusive vs shared WASAPI** latency / glitch matrix on this machine.

## License / legal

Uses public Windows WASAPI + installed Roland driver only. Not affiliated with Roland or Elektron. Check Roland driver EULA before redistributing commercially.
