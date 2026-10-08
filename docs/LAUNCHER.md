# GUI launcher (first milestone)

The launcher is a separate SDL3 / Dear ImGui process. It does not initialize
Vulkan Video, decode streams or take ownership of the video pipeline. The
visible UI uses the neutral title **Панель управления** and a **Запустить**
button, not the repository's temporary working name.

## Run

Windows: build with scripts/windows/build-netimgui.ps1. The script invokes
cmake/DeployWindowsRuntime.cmake to collect SDL3, FFmpeg and UCRT64 runtime
dependencies beside reg_launcher.exe. Double-click
build-netimgui/reg_launcher.exe. Compilers/MSYS2 are still required to BUILD
but not to RUN a previously deployed bundle.

Ubuntu: build with the usual CMake workflow. Launch build/reg_launcher from
your desktop. Optionally install with cmake --install build --prefix ~/.local.
The Linux install layout copies the directly linked FFmpeg and SDL3 shared
libraries into lib/ and sets an executable-relative RPATH. The NVIDIA driver
and ordinary system libraries are intentionally not bundled. A packaged build
must still be tested on the target Ubuntu desktop.

Profiles:
- Windows: %LOCALAPPDATA%/Reg/profiles/*.json
- Linux: $XDG_DATA_HOME/reg/profiles/*.json or ~/.local/share/reg/profiles

Enter an RTSP URL, save the profile and click **Запустить**. The last
selected profile is restored at the next launch. Advanced options
can create named profiles. Names are restricted to ASCII letters, digits,
underscore and dash for safe cross-platform filenames.

Each launch writes data/sessions/<timestamp>:
- session.json (RTSP authentication redacted)
- stdout.log and stderr.log (complete child output)
- logs/frame_timing_*.csv (the existing timing trace, inside session cwd)
- screenshots/ (when screenshots are requested)

Continuous Blackbox recording lives outside the session at
data/recordings/blackbox, preserving retention across restarts.

The standalone CLI works as before. GUI replay, NetImgui port override,
display mapping and automatic session cleanup are future increments; these
are not currently exposed as non-functional controls.

## Windows self-updates (Development and Stable)

Only the first installation requires downloading/extracting portable.zip from
a successful Windows CI artifact or GitHub Release. Run Launcher.exe afterward.
Development build trees are deliberately never overwritten by the updater.

Windows CI caches compiled FFmpeg, builds and tests, deploys DLLs, verifies the
package and publishes immutable GitHub Releases: dev-<full SHA> for branch
pushes and vMAJOR.MINOR.PATCH for stable version tags. Pull requests are
tested but never publish executables. Failures block release publication.

Each release contains a portable archive, a JSON manifest with per-file
SHA-256/size, and independent file assets. The launcher checks the official
GitHub Releases API over HTTPS, hashes local runtime files, downloads only
changed files, checks their SHA-256 and stages them in the user-data updates
folder under %LOCALAPPDATA%/Reg/updates.

Development automatically checks and downloads; it only installs after the
video child has stopped. Stable uses explicit download/install buttons.
A separate updater waits for the launcher to exit, backs up modified files,
atomically replaces them, attempts rollback on failure and restarts the GUI.

The portable installation folder must be writable by the logged-in user;
do not extract it to Program Files. RTSP profiles, logs and recordings remain
under %LOCALAPPDATA% and cannot appear in the managed file whitelist.

Current limitations: signing/Authenticode and handling sudden power loss
during multi-file replacement are not yet implemented. Retained backup files
are available for manual recovery in rare incomplete updates. On first install,
Windows SmartScreen may warn for an unsigned build; validation on clean target
Windows with the NVIDIA driver is still needed.

## Compact control panel

The launch panel uses the project's pinned Roboto and Google Material Symbols
fonts. Its primary launch/stop button stays visible in a fixed footer; session
logs and advanced diagnostics are collapsed by default. Separate tinted cards
group source, output monitors, recording, network and reconnect settings.

The per-profile monitor selection is stored alongside the RTSP source:
- **Automatic (0):** retain historical mapping. On Windows, Raw is monitor 3,
  CV Overlay is monitor 2 and Telemetry is monitor 1, as shown by Windows
  Display Settings -> Identify.
- **Monitor 1..N:** explicitly choose the monitor number shown by Windows
  Display Settings. On Linux this is a one-based SDL display index.
- Topology changes reapply the selected mapping without restarting the viewer.

NetImgui can be disabled independently or configured to listen on a profile
specific TCP port (default 8888). The listener starts after command-line
parsing, before the RTSP/Vulkan runtime. A binary compiled without
REG_ENABLE_NETIMGUI_REMOTE still accepts these startup options but does not
open a remote listener.

The new flags accepted by reg_probe are --raw-display, --overlay-display,
--telemetry-display, --netimgui-port and --disable-netimgui. Profiles saved by
earlier versions are upgraded on read with compatible defaults.

## Update compatibility fix

MinGW's standard runtime includes libstdc++-6.dll. Update manifests and asset
names now allow '+' in an otherwise tightly restricted basename and include a
specific regression test. Old portable installations can install the next
release after the update has been published.

## One-time legacy updater migration

The original Windows updater wrongly rejected + in the standard MinGW DLL
name libstdc++-6.dll. A single Development release marked [legacy-bridge]
in its commit message omits that DLL from the **delta manifest only**.
The DLL remains in the complete portable archive. A later release returns
to the complete manifest after the corrected validator is deployed.

The updater executable is now statically linked against MinGW runtime
libraries, because it must run from a staging directory while the installed
runtime DLLs are being replaced. Windows CI checks PE imports for
libstdc++/libgcc/libwinpthread to prevent this regression.

The old updater's dependency search could also prevent it from launching
after being copied to the staging directory. In that case, the initial
legacy installation cannot update itself: install the newly produced
portable.zip once to establish a working statically linked updater.
Subsequent updates are in-app. Do not delete %LOCALAPPDATA%/Reg, which
contains profiles and recordings.

## Portrait Full HD layout and launcher window placement

The launcher defaults to a 960 x 840 window, so it fits comfortably on a
1080 x 1920 portrait display without occupying the full vertical desktop.
The launch/stop footer remains visible. Display selection combos are now
168 logical pixels wide and placed inline with monitor names; other numeric
fields are 116 pixels wide without redundant increment/decrement buttons.
The two-column card layout fits a 1080-wide display and falls back to a
single column on small windows. Detailed settings and logs are collapsible.
The scrolling region only becomes necessary if the window is resized small
or extra sections are expanded.

An **Окно: Монитор 3** selector in the profile toolbar configures which
Windows Settings -> Display -> Identify monitor the launcher should open on.
It uses the same QueryDisplayConfig CCD numbering as video output mapping,
rather than SDL enumeration or GDI display suffixes. The default is monitor
3 on Windows, with fallback to the system placement when that monitor does
not exist. The choice is stored in
%LOCALAPPDATA%/Reg/launcher-display.txt, independent of video profiles;
0 means the system's primary display. Changing the selection immediately
moves and centers the existing launcher window.

Windows CI restores the previously built FFmpeg dependencies from a stable
content-addressed cache key, skipping its expensive compilation when those
sources remain unchanged. An MSYS2 ccache experiment was removed after
diagnostics reported 144 unsupported compiler-option invocations and 4
precompiled-header incompatibilities; it produced no reusable C++ objects.
C++ object caching should not be advertised until its compatibility is fixed
and measured in consecutive successful CI runs.

## Portrait dashboard layout fix

The settings dashboard is now centered in an at-most **750 logical pixel**
column; the two-column card table therefore stops expanding across the whole
portrait desktop. Each colored card uses Dear ImGui child auto-height
(ImGuiChildFlags_AutoResizeY) and disables its own scrollbar. Card height
follows the visible controls, including changing Overlay/Telemetry or
Blackbox visibility. This also prevents the earlier RTSP URL clipping.

Only the outer settings region may scroll when the window is genuinely too
small or advanced/log sections are expanded. The launch/stop footer remains
fixed. All delay/port inputs and the Overlay delay slider now share a compact
116-pixel control column. Profile and monitor selectors retain their own
bounded widths and settings.

## GitHub Releases ordering and updater recovery

The GitHub Releases listing is not guaranteed to be sorted by creation or
publication date. A real October 8 response listed a previously published
Development build first, ahead of a newer release. The launcher now selects
the matching non-draft release with the greatest UTC published_at value,
then reads the installed manifest's tag to refuse a downgrade. If the
installed version is not included in the first API page, the updater fetches
that version's release metadata separately to compare dates. Invalid dates
are never interpreted as an update.

The updater reports the selected version identifier in its status text and
does not install an older release. Unit tests use an intentionally shuffled
GitHub response to guard against regressions.

IMPORTANT: a launcher built before this fix will keep selecting the wrong
release because the bug is inside that installed Launcher.exe. If it cannot
discover the repaired version, replace **only Launcher.exe** from the latest
verified Development release (download the standalone launcher EXE asset,
close the application, rename the downloaded file to Launcher.exe, and
replace it in the portable directory). Then reopen and check for updates;
the new updater can fetch remaining changed files incrementally. No
reinstallation of settings or complete portable ZIP is required.

## Responsive portrait layout (1200 x 1920)

The previous 750-pixel dashboard width cap has been removed. The launcher
uses the full available content width with responsive ImGui tables:

- available width >= 1080: three equal columns for Screens, Blackbox and
  Delay/Recovery; Network and Updates share a two-column row
- 760 <= available width < 1080: the original two-column grouping
- available width < 760: one vertical column, avoiding clipped input labels

All coloured cards keep automatic content-based height and have no internal
scrollbar. The source RTSP URL spans the viewport width. The launcher monitor
selector is right-aligned in the profile toolbar on wide windows. The
advanced section and session log viewer retain the full width, with the
expanded log viewer filling the remaining height above the pinned Stop/Start
footer, so a 1920-tall screen is useful for inspecting streaming output.

Responsive breakpoints have a dedicated standalone C++ regression test.

## Windows 10 portrait workspace with Mission Planner

Windows 10 does not provide a native horizontal 50/50 Snap layout occupying
both full-width halves of a portrait monitor. The launcher can directly
arrange two *existing, top-level* Windows windows using Win32:

- Select the Windows monitor in the launcher toolbar (for example, monitor 4).
- Start Mission Planner; its main window title must include "Mission Planner".
- Click **Разместить 50/50 с Mission Planner**. Launcher occupies the upper
  half, Mission Planner occupies the lower half of the monitor's **work area**.
  Windows taskbar space is excluded and either app may be maximized before
  arranging (both are restored).
- Optionally enable **При запуске**. This persists in
  `%LOCALAPPDATA%/Reg/mission-planner-layout.txt`. The launcher waits for
  Mission Planner if it is not running (retry every 3 seconds) and tiles
  windows once; it will not fight later manual window adjustments.
- This is independent of profile monitor mappings and the RTSP/Blackbox
  process. The launcher never launches, kills or injects into Mission Planner.
- If Mission Planner is elevated and Launcher is not, Windows UIPI may deny
  repositioning. Run both apps at the same integrity level.
- No extra snapping utility, Windows 11, PowerToys or separate scripts are
  required. The feature compiles to a stub on Linux.

The two-window arrangement uses monitor coordinates from QueryDisplayConfig
and the selected monitor's Win32 rcWork. A disconnected monitor, a missing
Mission Planner window or an undersized work area produces a readable status
and no attempted two-window layout.

### CI repair

The first full-width dashboard commit was blocked by a missing namespace
closing brace in LauncherLayout.hpp, breaking the layout test on Windows and
Linux. This release repairs the header; both CI suites must be green prior
to publishing an update.
