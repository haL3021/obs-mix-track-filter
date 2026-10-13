[ [日本語](README.md) | English ]

# [ Mix Track Filter (OBS Plugin) ]

An OBS Studio plugin that allows applying standard OBS audio filters and VST effects to **final mixed audio outputs (Tracks 1-6)**.

Apply mastering limiters, compressors, equalizers, and maximizers to the final mixed stream or recording track after all sources have been summed.

## | Key Use Case

> Balancing multiple audio sources with sufficient headroom while ensuring the final mixed stream never clips or distorts, and boosting the overall volume to commercial levels.

Each individual audio source can be mixed conservatively to prevent distortion, and then a maximizer / limiter can be applied to the master track to achieve high loudness and consistent dynamics.

---

## | System Requirements

- **OS:** Windows 10 / 11 (64-bit)
- **OBS Studio:** 32.x / 33.x series (64-bit)

## | Installation

1. Place `mix-track-filter.dll` in the following directory (create the directory if it does not exist):

```text
C:\ProgramData\obs-studio\plugins\mix-track-filter\bin\64bit\
```

2. Start OBS Studio.

---

## | Usage

### 1. Adding and Managing Filters
After launching OBS Studio, open the dock from the menu bar via **Docks > Mix Track Filter**.

- **Add & Edit Filters:** Click `+ Add` in the dock to add filters, or click `Open Filter Window...` to open the native filter properties and adjust parameters.
- **Bypass All:** Check `Bypass All Filters` to temporarily mute all processing and compare with the dry/unfiltered signal.
- **Auto-Rehook:** Automatically ensures filters remain at the head of the audio output chain during streaming and recording starts. (Keep enabled under normal operation).

> **Note:** If the Status is anything other than **Optimal**, the filter might not be applied properly in the output pipeline. Click `Rehook Order` to manually re-apply the filter hook.

### 2. Configuring Applied Tracks
By default, filters are applied only to **Track 1**. If you use other tracks for recording and wish to apply master processing to them as well, configure them in the settings dialog:

1. Open **Tools > Mix Track Filter Settings** from the menu bar.
2. Under **Applied Tracks**, check the desired track numbers (Tracks 1-6) and click **OK**.

> **Caution:** Applying filters across multiple tracks increases CPU overhead and may cause audio stuttering under heavy load.

---

## | Building from Source (For Developers)

### Prerequisites
- CMake 3.16 or higher
- Visual Studio 2022 (Desktop development with C++)
- Qt 6 (Widgets, Core components)
- OBS Studio source code / headers and build tree

### Build Steps

```powershell
cmake -B build -G "Visual Studio 17 2022" -A x64 `
  -DOBS_PATH="<path to OBS build directory>" `
  -DQT_DIR="<path to Qt6 cmake directory>"

cmake --build build --config Release
```

Once built, the compiled binary `mix-track-filter.dll` will be located in `build/Release/`.

---

## | License

This project is licensed under the [GNU General Public License v2.0](LICENSE).
