[ [日本語](AGENTS.md) | English ]

# AGENTS.md - Project Rules & Guidelines

This document outlines the core principles, coding conventions, and architectural constraints for developing the OBS Studio plugin "**`mix-track-filter`**".

---

## 1. Core Principles & Technology Stack

- **Project Overview**:
  - A C++/Qt plugin for OBS Studio that applies standard audio filters and VST effects to final mixed audio outputs (Tracks 1–6).
- **Naming Conventions**:
  - Project name: `mix-track-filter` (CMake target: `mix-track-filter-plugin`)
  - Output binary: `mix-track-filter.dll`
- **Target Environment**:
  - OBS Studio 32.x / 33.x series (Windows 64-bit primary, designed with cross-platform portability in mind)
- **Language Standards & Toolchain**:
  - Language: C++17 (along with C helper `filter-helper.c`)
  - Build system: CMake (3.16+), MSVC (Visual Studio 2022)
- **Dependencies**:
  - `libobs` (OBS core engine & audio pipeline)
  - `obs-frontend-api` (OBS main UI & frontend events)
  - `Qt6` / `Qt5` (QtWidgets, QtCore)
- **UI Policy**:
  - All UI strings must be in **`en_US` (English)**. Never include Japanese text in UI components.
  - Maintain a clean, minimal layout; avoid excessive decoration or redundant descriptive text.

---

## 2. Coding Conventions & Naming Rules

- **File Naming**:
  - Lowercase kebab-case (e.g., `audio-processor.cpp`, `mix-track-dock.cpp`, `settings-dialog.cpp`, `plugin-main.cpp`).
  - Extensions: `.hpp` for C++ headers, `.h` for C headers, `.cpp` / `.c` for source files.
- **Symbol Naming**:
  - **Classes / Structs**: PascalCase (`AudioProcessor`, `MixTrackFilterDock`, `SettingsDialog`)
  - **Methods / Functions**: camelCase (`hookAudio`, `unhookAudio`, `rehookAudio`, `setTrackMask`)
  - **Member Variables**: camelCase (`dummySource`, `isHooked`, `trackMask`)
  - **Static Variables / Singletons**: `s_` prefix (`s_activeDock`, `s_settingsDialog`)
  - **Constants / Macros**: UPPER_SNAKE_CASE (`MIX_TRACK_FILTER_VERSION`, `UNCLAMPED_OFFSET_FLOATS`)
- **Logging**:
  - Use OBS native `blog()` and always prefix messages with `[MixTrackFilter]`.

---

## 3. Architecture & Constraints (Critical)

### Audio Pipeline (Crash & Stutter Prevention)
- **Zero-Allocation & Lock-Free in Audio Threads**:
  - In real-time audio callbacks (`onAudioData`), **never use dynamic memory allocation (`new`, `malloc`, etc.) or mutex locking**.
  - Synchronize with the UI thread using `std::atomic` and RCU-style immutable snapshot (`FilterChainSnapshot`) pointer swaps. Retire old snapshots safely on the main thread.
- **Write Back to Both Normal and Unclamped Buffers**:
  - OBS streaming reads from normal buffers, while recording may read from unclamped buffers (`mix->buffer_unclamped`).
  - Processed audio data must always be written back to **both the normal buffer and the unclamped buffer at offset `UNCLAMPED_OFFSET_FLOATS`**.
- **Dynamic Rehook for LIFO Output Distribution**:
  - OBS distributes audio to handlers in reverse order (LIFO: array end to start).
  - When output events (streaming, recording, replay buffer, etc.) trigger, call `rehookAudio()` to move the plugin hook back to the end of the array (executed first) before encoders receive audio.

### UI & Settings Design
- **Bidirectional Sync with Native Filter Dialog**:
  - Monitor private dummy source signals (`filter_add`, `filter_remove`, `reorder_filters`, etc.) to keep dock items in sync with native windows.
  - Delegate detailed filter adjustments to the native OBS filter properties window (`Open Filter Window...`) rather than embedding them inside the dock panel.
- **Per-Track Bypass & Safe Persistence**:
  - Manage Tracks 1–6 via bitmask (`trackMask`). Inactive tracks must bypass processing to minimize CPU overhead (Default: Track 1 only).
  - Persist settings to `%APPDATA%/obs-studio/plugin_config/mix-track-filter/settings.json` safely.

---

## 4. Versioning & Release

- **Semantic Versioning**:
  - Follow `MAJOR.MINOR.PATCH` (e.g., `0.3.10`).
  - Keep `MIX_TRACK_FILTER_VERSION` macro, `CMakeLists.txt`, and settings JSON versions synchronized.
  - Always increment the patch version for any bug fix, refactor, or minor UI adjustment.

- **Workflow Rules for Push and Release Publishing**:
  1. **Commit and Push Preparation**:
     - Increment the patch version and synchronize all version references across the codebase.
     - Commit changes using Conventional Commits (`feat:`, `fix:`, etc.) and push to `main`.
  2. **CI Build Verification**:
     - Verify that the GitHub Actions automated build finishes with success before proceeding to release.
     - CI artifacts must be saved as uncompressed directories (`mix-track-filter-X.Y.Z-windows`) to prevent nested zip archives and accidental packaging errors.
  3. **Git Tagging**:
     - Create and push an annotated tag corresponding to the version (`git tag -a vX.Y.Z -m "Release vX.Y.Z"`).
  4. **Release Title & Notes Guidelines**:
     - **Release title**: Format as `vX.Y.Z - Brief summary of primary change in Japanese` (e.g., `v0.3.10 - 軽微なUIの変更`).
     - **Language**: Provide release notes in **both Japanese and English** (e.g., Japanese section followed by English section).
     - **Scope**: Include only **user-facing changes to the deliverable / plugin itself** (omit documentation updates, CI changes, and verbose installation steps).
  5. **Release Execution (Verification, Explicit Zipping, API Execution)**:
     - Retrieve the uncompressed plugin directory from CI artifacts and verify directory structure and binaries (e.g., presence of DLL).
     - After verification, explicitly package the zip archive locally as `mix-track-filter-X.Y.Z-windows.zip`.
     - Present the release title and release notes to the user for review.
     - Upon explicit user confirmation, execute the GitHub Releases API directly to create the release and upload the zip asset.

