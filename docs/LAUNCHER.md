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
