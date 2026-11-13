# 3DS Couch Multiplayer Architec | Google AI Studio

> Exported from https://aistudio.google.com/prompts/19U8mN1xxZtKEr4Ifr-Yqqp1itR5W5iZx on 2026-11-13 10:12 · AI Studio · 4 messages

## Assistant

**Analyzing the User's Request**

I'm digging into the specifics of the user's ask, focusing on their desire for a 3DS-focused sister project to those split-screen emulators, particularly eyeing the Azahar engine. I'm assessing if Azahar's capabilities align with the project's requirements.

**Verifying URL Accessibility**

I'm now focused on the feasibility of accessing necessary URLs. It seems I can't directly search GitHub for the user's projects because there's no search capability. Instead, I'll attempt to verify a known URL, specifically https://github.com/azahar-emu/azahar, using the available tools, or if that is somehow impossible, will ask the user. I'm being cautious to avoid making assumptions about unknown URLs or the user's GitHub username.

**Refining Browse Logic**

I've just confirmed google:browse is URL-dependent, so I need to only trigger it when a URL is available from the user or search results. This constraint refines how I'll handle requests, making the logic more precise. I'm focusing on aligning with the provided developer persona for interaction style.

**Analyzing Radical Differences**

I'm now diving deep into the complexities that set 3DS emulation, especially Azahar, apart from GBA and DS when considering split-screen multiplayer. I'm focusing on those critical differences: the underlying hardware, the software architectures, and how that impacts multiplayer approaches. I'm thinking of probing the user on their architectural choices and domain expertise to drive the conversation forward.

**Evaluating Architecture Approaches**

I'm currently dissecting the technical underpinnings to understand split-screen multiplayer's complexities in GBA/DS versus 3DS emulation (Azahar). I'm looking at two primary architecture patterns: the multi-process orchestrator (wrapper) and the single-process multi-instance fork. For Azahar, I foresee potential challenges in a 'fork' approach due to its heavy reliance on global/singleton state, like memory hooks and the JIT code cache. I want to challenge and confirm the user's architectural approaches.

**Assessing Architectural Hurdles**

I'm now really grappling with the architectural challenges for split-screen 3DS emulation, specifically within Azahar. I'm hitting a roadblock. I'm focusing on the non-reentrant state issues and dual-screen layout concerns, finding these elements are significant UX and resource problems when compared to simpler GBA/DS systems. The multi-process approach seems better, despite the added resource contention.

**Addressing Touch Input**

I'm now fully immersed in the touch input challenge for 3DS emulation split-screen. I'm visualizing 4- and 8-viewport layouts, and the cramped display scenarios this presents. It looks like I need to ask the expert about a YAGNI layout strategy, such as main top screen with a tiny inset for the bottom screen, or a toggleable view button on the controller. I'm wondering how they're handling touch input on a couch and considering virtual touch hotspots or right-stick cursor mapping.

**Investigating Networking Strategy**

I'm now zeroing in on 3DS multiplayer via local rooms. I've realized Azahar has existing network room infrastructure (like Citra), making a custom bridge unnecessary. My thinking is to leverage this by spinning up a local azahar-room on 127.0.0.1 and auto-joining instances. Now I'm focused on the need for unique console identities.

**Examining File System Conflicts**

I'm now zeroing in on data separation to avoid save data corruption and duplicate player issues. Sharing a user directory in a multi-instance setting is problematic due to conflicts in nand/, sdmc/, and config/. I'm exploring per-player profile overlays or isolated user directories with symlinked shared ROMs and base system files. I need to consider Download Play support, asking if each player needs their own ROM, updates, and DLC, given Azahar's history. I also need to understand the need for shared versus separate character saves across different game types.

**Assessing Real World Constraints**

I'm now grappling with the harsh realities of local split-screen performance. Running four emulated 3DS instances will hit shader compilation bottlenecks, audio routing challenges and frame sync issues. These are significant hurdles, as compared to GBA. I need a user-defined audio focus to solve these issues. I need to know how to resolve the lag issue due to sub-10ms Wi-Fi polling.

**Considering Window Management**

I'm now focused on handling window management and the user experience for split-screen. I'm thinking about borderless window tiling versus a compositing window approach, examining how existing emulators handle it. I'm focusing on compositing challenges such as screen layouts, and multi-display VSync and HDR, considering tradeoffs. I now have to consider background throttling.

**Evaluating Architecture Approach**

I'm now considering the ideal approach for rendering. I think a unified SDL/Qt/ImGui couch overlay with shared memory or external memory for video capture offers the cleanest TV experience. This permits player badges, overlays, a unified pause menu, and custom screen ratios, although at the cost of compositor complexity. I'm also confirming the target Azahar fork (azahar-qt, SDL, or fork) to guide this decision.

**Prioritizing Architectural Strategy**

I'm now zeroing in on the architectural design, carefully considering the multi-process orchestrator versus a forked core approach for Azahar, I'm leaning heavily against an in-process, multi-core fork due to potential singleton conflicts and overall complexity. I'm focusing on the significant differences with 3DS emulating, especially the challenges of Identity and Data Isolation, dual-screen UX, the multiplayer bridge, and GPU/audio contention.

---

## Assistant

**Defining the Architecture**

I've got a much clearer signal now. The PM is keen on a Rust/Tauri orchestrator – it'll be a web frontend with OS builds, managing 2-4 simultaneous Azahar sessions from a single ROM file. Pattern A is the central approach plus a Tauri wrapper for integration.

**Analyzing the Implications**

I'm now digging into the architectural implications. The concept of a "downstream fork of Azahar" feels strong, and that alongside the Tauri wrapper for multi-session support looks promising. My next challenge is to weigh "Downstream fork of Azahar" vs. "Thin patches / CLI flags" for maintainability. I'm keen on a YAGNI check here. I am examining this sweet spot, if the Azahar fork stays lean.

**Refining the Approach**

I've refined the approach. I'm now clear: the Azahar fork should be surgical, focused on critical orchestration gaps. Specifically, I'll need a CLI flag to bind game controllers and enable background events. I also need a controller hotkey for swapping screens and right-stick to virtual touch pointer functionality, all implemented within Azahar's input layer for cross-platform compatibility. I've noted a major Wayland/Accessibility limitation with the alternative.

**Evaluating the Requirements**

I'm now zeroing in on the specific CLI flags needed. The focus is cross-platform compatibility, specifically regarding window management. I'm prioritizing direct CLI args for window geometry and auto-join room configurations to sidestep OS-specific hacks within the Tauri wrapper. I'm also carefully assessing the trade-offs between window tiling and compositing across platforms, with a focus on Wayland and macOS, to ensure a seamless cross-platform experience. "Right-stick touch" and a screen-swap hotkey within the Azahar fork feel correct.

**Investigating Compositing Options**

I'm now deep-diving into the windowing challenges. Wayland's restrictions, particularly the prohibition on external apps manipulating other windows, are a major hurdle. I'm also contending with macOS's fullscreen behavior and potential App Nap throttling. I need to understand how other emulators achieve splitscreen, specifically mgba and melonds, as a guide. I'm exploring tiling multiple OS windows, and also compositing frames within Tauri (and the performance implications). A third option of SteamOS windowed borderless also shows promise.

**Assessing Memory Implications**

I'm now focusing on memory and startup overhead. I must determine if Azahar has an SDL frontend, as running four Qt instances would be resource-intensive compared to an SDL alternative. Also, I'm checking for an Azahar output via shared memory, to a lightweight native surface, as a possible memory-efficient solution. This is essential for optimal performance across multiple simultaneous sessions.

**Evaluating Controller UX**

I'm now focused on refining the single-screen toggle UX concept. I'm thinking through Mario Kart 7 specifically. In MK7, the top screen displays the race, while the bottom shows the minimap. A pure "toggle top/bottom" solution is clean in 2P or 4P scenarios. This controller-based UX approach needs more analysis before committing.

**Refining the UI/UX**

I'm making progress on refining the single-screen toggle. I'm prioritizing TV readability and hardware performance by deprioritizing rendering the hidden screen. I'm focusing on two UX refinements: a sticky press toggle for switching screens, and the implications for touch input when the top screen is active, and how that is handled in menus and lobbies. A single sticky toggle is simpler. The automatic touch disable should prevent footguns.

**Clarifying Launch Strategy**

I'm now zeroing in on the launch strategy. I've realized single ROM file support is easiest with decrypted .3ds/.cci files. For installed .cias, I need to account for saves living inside the game's sdmc folder, which complicates the setup because Azahar saves are written into the same directory! I'll clarify what file formats are supported in v1. I'm also ensuring each player gets a unique console identity.

**Defining Network Connectivity**

I'm now zeroing in on network connectivity. I'll focus on pre-linked local wireless functionality, where Tauri will spawn an azahar-room server on 127.0.0.1 at an ephemeral port. I will have Tauri pass command line arguments to each Azahar instance for automatic room joining. Also, I'm considering cloning save data, to ensure all players unlock progress like in Mario Kart 7.

**Prioritizing Performance Metrics**

I'm now zeroing in on performance. I realize that for a wide range of devices, we need to consider specific adjustments. We must target a 1x native resolution by default for 3–4 players, potentially 2x for 2 players. I must also consider making audio P1-only by default with a null backend for P2-P4, to save audio mixer overhead. Additionally, the P1 instance (or the compositor) should drive VSync.

---

## Assistant

**Confirming Previous Recommendations**

I'm revisiting the PM's confirmation. It's great they're in sync with our recommendations: specifically the approach used by mgba-splitscreen and melonds-splitscreen for tiling borderless emulator windows (Path 1). Also, the agreement covers controller binding and screen toggle hotkey placement. Looks like we're on the right track!

**Validating Implementation Scope**

I'm focused on the minimal downstream Azahar fork. My current thinking revolves around a Tauri orchestrator for ROM selection, controller handling, and instance management, with .3ds/.cci ROM scope for the first version. Low-end hardware considerations are also a priority, including resolution scaling and audio nulling for additional players. Now I'm shifting to addressing two crucial, open questions before proceeding: platform compatibility for window tiling and the practical implications of integrating with the Azahar frontend.

**Clarifying Tiling Strategy**

I'm solidifying the cross-platform window tiling details. It's critical that the implementation guide explicitly addresses window management. Specifically, Windows and X11 offer straightforward solutions. However, Wayland and SteamOS/Gamescope require pragmatic tricks. Namely, forcing Azahar instances to XWayland for Wayland-based systems, or detecting pure Wayland and warning/recommending compositor rules, especially in SteamOS Gaming Mode. This is paramount! macOS is less of a headache by comparison.

**Evaluating Frontend Dependencies**

I'm now weighing the impact of Azahar's frontend choice: SDL or Qt. Since the PM didn't specify, I'm analyzing the implications of each, as historical releases used both. If Azahar still supports SDL, patching its lighter frontend is ideal. Otherwise, I'll need to adapt my approach for Qt6. The precise binary and symbol paths need verification against the checked-out Azahar HEAD! I'm now comparing patchability and weight!

**Specifying Input Layer**

I'm solidifying the implementation's technical underpinnings, and ensuring these are clearly documented. It's crucial the Implementation Guide specifies shared input_common and core code. The initial step for the engineers must involve checking CMakeLists.txt in Azahar's HEAD. The ultimate aim is a crisp PRD and Implementation Guide for azahar-splitscreen.

**Defining Implementation Checklist**

I'm now outlining acceptance criteria. The guide will include a testing checklist, focusing on Mario Kart 7 as a key reference. This involves adding specific CLI arguments, input and HID patches, and config/renderer overrides. I am detailing the Tauri orchestrator's structure and lifecycle, starting with port allocation.

**Detailing Instance Management**

I'm now detailing instance management. This includes graceful shutdown to prevent zombies, with slot provisioning for unique console identities and save cloning versus independent mode. I'm also planning shader cache pre-seeding. Gamepad enumeration using the gilrs crate, along with SDL2 GUID matching, is also in scope to improve reliability, with the aim of using a small command on the Azahar binary to guarantee match. Finally, grid geometry, staggered spawning, and process log tagging are now on the checklist.

---

## Assistant

Here is the **PRD + Implementation Guide** to lock down scope before we start writing code. I’ve included explicit cross-platform realities for Path 1 (window tiling) and a couple of pragmatic shortcuts to prevent classic multi-instance bugs.

---

# PRD: azahar-splitscreen (v1.0)

## 1. Objective

Deliver a plug-and-play couch split-screen experience for Nintendo 3DS multiplayer using 2–4 concurrent, pre-linked Azahar instances orchestrated by a cross-platform Tauri wrapper.

## 2. Scope & YAGNI Boundaries

### In Scope (v1)

### Out of Scope (v2+)

## 3. Core User Flow

## 4. Platform Support & Path 1 Caveats

## 5. v1 Acceptance Criteria (Mario Kart 7 Benchmark)

---

# Implementation Guide

## Part A: Downstream Azahar Fork (Minimal Patch Surface)

Keep C++ changes isolated from emulation internals so rebasing onto upstream Azahar stays painless.

### 1. New CLI Flags

Add headless-friendly orchestration flags to Azahar's entrypoint:

### 2. Input & HID Patches

---

## Part B: Tauri Orchestrator (Rust + Web UI)

### 1. Project Structure

Match a clean Tauri v2 layout:

```
azahar-splitscreen/
├── src/                      # Web UI (ROM picker, controller mapper, launch controls)
├── src-tauri/
│   ├── src/
│   │   ├── main.rs
│   │   ├── lib.rs            # Tauri commands & app state
│   │   ├── controllers.rs    # Invokes `azahar --list-controllers`
│   │   ├── slots.rs          # User-dir isolation, unique ConsoleID/MAC, save cloning
│   │   ├── layout.rs         # Monitor work-area -> 2x1 / 2x2 rect math
│   │   └── process.rs        # `azahar-room` + staggered Azahar spawn + cleanup
│   └── binaries/             # Bundled/sidecar `azahar` and `azahar-room` binaries
└── patches/                  # Tracked git patches / submodule ref for Azahar fork
```

### 2. Orchestration Sequence (launch_session)

---

## Part C: Phase 0 Verification (Do This First!)

Before writing the Tauri UI or C++ patches, we need a 30-minute baseline check on your target Azahar repo commit:

---

### Next Step

Where do you want to start?
