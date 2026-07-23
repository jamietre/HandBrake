# HandBrake fork: multi-GPU NVENC AV1 fix

## Why this fork exists

On a system with two NVIDIA GPUs — for example an RTX 3090 (Ampere, 7th-gen
NVENC — encodes H.264/HEVC only, **no AV1 encode**) alongside a newer RTX
5060 Ti (Blackwell — supports AV1 NVENC encode) — HandBrake's Video Encoder
dropdown never shows `AV1 (NVEnc)` at all, only `AV1 (SVT)` (software).
Manually disabling the older GPU in Device Manager makes `AV1 (NVEnc)`
appear immediately, confirming this is a device-selection bug, not a
driver/support issue.

Goal: patch HandBrake so it (1) correctly detects AV1 NVENC support when
*any* installed GPU supports it, and (2) correctly routes the actual encode
to a capable GPU, without requiring the non-AV1 GPU to be disabled by hand.

No ffmpeg patch is needed — ffmpeg's own `libavcodec/nvenc.c` already probes
each CUDA device and skips ones that fail the codec-capability check, *when
no hw_device_ctx is handed to it*. The bugs are entirely in how HandBrake
wires device selection around ffmpeg, confined to `libhb/`.

## Confirmed root causes

**Bug 1 — dropdown detection never checks device 1+**
`libhb/nvenc_common.c:153`, inside `hb_nvenc_probe_caps()`:
```c
if (cu->cuDeviceGet(&dev, 0) != CUDA_SUCCESS)   // hardcoded device 0
```
This function probes exactly one CUDA device (device 0) to decide whether
`has_av1`/`has_hevc`/etc. are true, then that result populates the whole
encoder dropdown menu. It already fetches `dev_count` a few lines above but
never loops over it. Fix: loop `i = 0..dev_count`, probe each device, OR the
capability flags together.

**Bug 2 — encode-time routing hardcodes device 0 when hardware decode is active**
Three-part chain, all in `libhb/`:

- `libhb/scan.c:723` — the shared decode-side CUDA device context is created
  with the device index hardcoded to `-1`:
  ```c
  hb_hwaccel_hw_device_ctx_init(hwaccel->type, -1, &hw_device_ctx);
  ```
  This always lands on whatever CUDA treats as its default device (device 0)
  regardless of which GPU can actually do AV1.

- `libhb/hwaccel.c:230-251` — `hb_hwaccel_hw_device_ctx_init()` takes a
  `device_index` param, but only ever wires it into an ffmpeg `AVDictionary`
  key called `"child_device"` (line 241), which is meaningful for QSV's
  D3D11VA child-device pattern — **not** for CUDA. The actual CUDA device
  selector in ffmpeg is the 3rd positional arg to `av_hwdevice_ctx_create()`
  (a string like `"1"`), and this code always passes `NULL` for it (line 251)
  regardless of device_type. So even a real device index passed in here would
  silently do nothing for CUDA today.

- `libhb/encavcodec.c:932-937` — when `job->hw_pix_fmt != AV_PIX_FMT_NONE`
  (true whenever hardware decode is active, e.g. NVDEC on a typical 2160p
  source), the NVENC encoder just reuses that already-wrong, already-bound-to-
  device-0 `hw_device_ctx` directly. This **bypasses ffmpeg's own
  known-good fallback loop entirely** — that loop (in ffmpeg's
  `nvenc_setup_device`, see below) only runs when no hw_device_ctx is
  pre-supplied. With hardware decode on, the AV1 encode session gets opened
  directly against the non-AV1 GPU's context and just fails (codec not
  supported), rather than falling back to the capable GPU.

Note: HandBrake already has this whole device-selection mechanism built and
working for Intel QSV — `--qsv-adapter` CLI flag (`test/test.c:221,2337-2339,
2546,2603,3618-3619,4925-4928`), `job->hw_device_index`
(`libhb/handbrake/common.h:991`), `AdapterIndex` JSON field
(`libhb/hb_json.c`). It was just never extended to cover CUDA/NVENC — there is
currently zero user-facing GPU selection for NVENC.

**Bug 4 (found during implementation, not in the original analysis above) —
the real encode job never gets a device index at all.** `libhb/work.c:1770-1775`
calls `hb_qsv_setup_job(job)` when `job->hw_decode & HB_DECODE_QSV`, and that
function (`libhb/qsv_common.c:2230-2244`) sets
`job->hw_device_index = hb_qsv_get_default_adapter_index()` whenever the user
hasn't forced one. There was no NVDEC/NVENC equivalent — for CUDA,
`job->hw_device_index` stayed at its `common.c:4978` default of `-1` all the
way into the `hb_hwaccel_hw_device_ctx_init()` call at `work.c:1816-1818`.
Fixing scan.c alone (bug 2's first part) only fixes the scan/preview-time
context, which is a *separate* `hw_device_ctx` from the one work.c builds for
the actual encode job — so without this, the real encode would still land on
CUDA's default device (device 0) even after patches 1-3.

## What we verified in ffmpeg (no patch needed there)

`libavcodec/nvenc.c`, `nvenc_setup_device()`: when no `hw_frames_ctx`/
`hw_device_ctx` is provided, it iterates every CUDA device via
`nvenc_check_device(avctx, i)`, which calls `nvenc_check_capabilities()` →
`nvenc_check_codec_support()` — this correctly rejects a device that doesn't
support the requested codec GUID (e.g. AV1) and moves on to the next device.
`ctx->device` defaults to `ANY_DEVICE` (auto), so this already picks the first
capable GPU automatically. This confirms: pure software-decode + NVENC-encode
jobs likely already work correctly today; it's specifically the
hardware-decode-enabled path that breaks it by pre-binding a context.

## The fix (4 patches, no UI redesign required for v1)

1. **`libhb/nvenc_common.c`** — loop `hb_nvenc_probe_caps()` over all CUDA
   devices (`dev_count`), OR the `has_h264`/`has_hevc`/`has_av1`/etc. flags
   across devices instead of checking device 0 only. Per-device probing was
   pulled into a new static helper `hb_nvenc_probe_device_caps()`.
2. **`libhb/hwaccel.c`** — in `hb_hwaccel_hw_device_ctx_init()`, when
   `device_type == AV_HWDEVICE_TYPE_CUDA` and `device_index > -1`, pass the
   index as the actual CUDA device string (3rd arg to
   `av_hwdevice_ctx_create`), mirroring the existing QSV special-case already
   in that function.
3. **`libhb/scan.c`** — stop hardcoding `-1` at line 723. For
   `AV_HWDEVICE_TYPE_CUDA`, call the new `hb_nvenc_av1_device_index()`
   (auto-picks the first CUDA device that supports AV1 encode) instead of
   blindly using device 0.
4. **`libhb/work.c`** — bug 4 above. Added an `HB_DECODE_NVDEC` block next to
   the existing `HB_DECODE_QSV`/`hb_qsv_setup_job()` block (`work.c:1770ish`)
   that sets `job->hw_device_index = hb_nvenc_av1_device_index()` when it's
   still `-1` — but only when the chosen encoder is NVENC-family (checked
   via a new `hb_video_encoder_is_nvenc()` helper in `common.c`, mirroring
   `hb_video_encoder_is_vaapi()`). `hw_device_index` is shared with other
   vendors' adapter selection (QSV's `child_device` in `encavcodec.c`), so
   a CUDA ordinal must not leak into a job that encodes elsewhere.

New shared helper: `hb_nvenc_av1_device_index()` in `nvenc_common.c`
(declared in `nvenc_common.h`) — returns the index of the first CUDA device
with AV1 encode support, or `-1` if none/NVENC unavailable. The index is
recorded during `hb_nvenc_probe_caps()`'s single pass over all devices (no
second probe), and the cache is computed into locals and published once at
the end, so a concurrent first call can never observe a half-written result.
Used by both patch 3 and patch 4.

A `--nvenc-adapter` CLI flag mirroring `--qsv-adapter` would be a nice-to-have
follow-up for explicit manual control, not implemented — v1 auto-picks.

## Building and testing (from WSL/Linux, no Windows machine required)

HandBrake's official Windows CI (`.github/workflows/windows.yml`)
cross-compiles `HandBrakeCLI.exe` + `hb.dll` **from Ubuntu**, using a vendored
mingw-w64 toolchain — it does not build the native core on Windows at all.
Everything below can be done from WSL. No Visual Studio, no MSYS2-on-Windows
needed.

```bash
# one-time environment setup — CI's own apt-get list is NOT sufficient,
# GitHub's hosted runner image ships extra tools preinstalled that CI's
# apt-get line doesn't mention. Confirmed needed on a bare WSL/Ubuntu box:
sudo apt-get install automake autoconf build-essential libtool libtool-bin \
    make meson nasm patch tar yasm zlib1g-dev ninja-build gzip pax libssl-dev \
    cmake clang zip
rustup target add x86_64-pc-windows-gnu
cargo install cargo-c   # slow (several min), compiles from source — normal, not a hang

# vendored cross-toolchain (checksum-verified — see windows.yml for the
# current URL/sha1, versions rotate over time)
wget https://github.com/HandBrake/HandBrake-toolchains/releases/download/1.0/mingw-w64-toolchain-11.0.1-ucrt-linux-x86_64.tar.gz
sha1sum mingw-w64-toolchain-11.0.1-ucrt-linux-x86_64.tar.gz   # compare to windows.yml
mkdir -p toolchains && mv mingw-w64-toolchain-*.tar.gz toolchains/
cd toolchains && tar xvf mingw-w64-toolchain-*.tar.gz && cd ..

# build (from repo root)
export PATH="$PWD/toolchains/mingw-w64-toolchain-11.0.1-ucrt-linux-x86_64/mingw-w64-x86_64/bin:$PATH"
./configure --cross=x86_64-w64-mingw32 --enable-qsv --enable-vce --enable-nvenc --enable-nvdec --launch-jobs=0 --launch
# --launch is what actually runs the build (configure alone only writes the
# Makefiles) — easy to drop by accident, and if you do, `make pkg.create.zip`
# fails cryptically ("cp: cannot stat 'HandBrakeCLI.exe'") since nothing was
# ever compiled. Re-running configure on an existing build/ dir needs --force
# or it aborts with exit 1 and prints nothing useful to stdout/stderr (the
# real error is `AbortError('build directory already exists')` — check
# build/log/build.txt, NOT the configure invocation's own output, for what
# --launch's internal `make` actually did/failed on).
cd build
make pkg.create.zip   # only needed for the .zip; skip if `zip` isn't installed —
                       # HandBrakeCLI.exe and libhb/hb.dll already exist after
                       # the --launch step above regardless.
```

**Verified working end-to-end against real dual-GPU hardware (RTX 3090 +
RTX 5060 Ti).** `HandBrakeCLI.exe` and `libhb/hb.dll` compiled and linked
cleanly with all 4 patches. WSL can run the Windows `.exe` directly via its
interop layer — no copy to the Windows side needed for CLI validation:

```
$ ./HandBrakeCLI.exe --version
nvenc: caps probe -> h264=1 h264_10bit=1 hevc=1 av1=1     # patch 1: was av1=0 before
$ ./HandBrakeCLI.exe -e list | grep nvenc_av1
nvenc_av1
nvenc_av1_10bit                                            # now listed, wasn't before
```

Then a real encode — NVDEC hardware decode active + `nvenc_av1` encoder,
exactly the combination Bug 2 broke — against a generated 1080p H.264 test
source: decoder picked `nvdec hwaccel h264 8-bit (nv12, cuda)`, encoder
`AV1 (NVEnc)`, `Encode done!`, exit 0, valid `av01` stream confirmed via
`ffprobe`. This is the actual compiled production binary, not a scratch
harness — full proof all 4 patches work together on real dual-GPU hardware.

### GUI build — also works from WSL/Linux

The WPF GUI cross-compiles and *runs* from WSL too, no real Windows machine
needed to build it (though you'll need one to actually use the GUI):

```bash
# .NET 10 SDK, user-local install, no sudo needed (project targets
# net10.0-windows; a distro-packaged .NET 8 SDK won't cut it):
curl -fsSL -o dotnet-install.sh https://dot.net/v1/dotnet-install.sh
chmod +x dotnet-install.sh
./dotnet-install.sh --channel 10.0 --install-dir "$HOME/.dotnet10"
export PATH="$HOME/.dotnet10:$PATH"

cd win/CS
# EnableWindowsTargeting=true lets a WPF/WinForms project ("net10.0-windows...")
# restore/build on non-Windows by pulling the WindowsDesktop reference-assembly
# NuGet packages instead of requiring a real Windows Desktop install.
dotnet restore HandBrakeWPF/HandBrakeWPF.csproj -p:EnableWindowsTargeting=true
# ^ FAILS as-is: NU1101 "Unable to find package System.CodeDom" (also hits
# System.Drawing.Common, Microsoft.Win32.Registry, etc. depending on flags
# tried). Root cause: HandBrakeWPF/packages.lock.json was generated on
# Windows and doesn't validate cleanly against the SDK's automatic
# PrunePackageReference resolution when EnableWindowsTargeting fakes the
# platform on Linux — NOT a real missing-package problem (confirmed via the
# nuget.org API directly: System.CodeDom 10.0.10 exists fine). Deleting
# packages.lock.json (in a scratch copy — it's a tracked file that affects
# the real Windows build's reproducibility guarantees, don't delete it for
# real without deciding to regenerate it properly) makes restore succeed
# cleanly with no other changes needed.

dotnet build HandBrakeWPF/HandBrakeWPF.csproj -c Release -p:EnableWindowsTargeting=true
dotnet publish HandBrakeWPF/HandBrakeWPF.csproj -p:PublishProfile=publish_x64 -p:EnableWindowsTargeting=true
# -> HandBrakeWPF/bin/publish/HandBrake.exe (~34MB, PE32+ GUI x86-64, not
# self-contained — needs .NET 10 Desktop Runtime on the target Windows
# machine). Copy libhb/hb.dll from the CLI build (same patched source) next
# to HandBrake.exe — the WPF app is just the UI layer, all real
# encode/decode logic is in hb.dll via HandBrake.Interop's P/Invoke.

# publish_x64.pubxml only carries the Worker's apphost .exe into the output —
# NOT the managed HandBrake.Worker.dll/.deps.json it actually needs, nor its
# HandBrake.Interop.dll dependency (both exist already in
# HandBrake.Worker/bin/Release/ from the earlier `dotnet build` step). Without
# these, the GUI's "Start Encode" hangs ~20s then fails: "Unable to connect to
# the HandBrake Worker instance" / "The application to execute does not
# exist: '...\HandBrake.Worker.dll'" — the GUI spawns Worker.exe as a
# subprocess over a local HTTP port (127.0.0.1:8037) to actually run encodes,
# and that subprocess can't start without its own managed assembly present.
# Fix: copy these three alongside the publish output before running:
cp HandBrake.Worker/bin/Release/HandBrake.Worker.dll \
   HandBrake.Worker/bin/Release/HandBrake.Worker.deps.json \
   HandBrake.Worker/bin/Release/HandBrake.Interop.dll \
   HandBrakeWPF/bin/publish/
```

**If running the built `.exe` via WSL's interop layer** (`./HandBrake.exe &`
from WSL, so it launches as a real Windows process on your desktop) rather
than copying to Windows first: it opens and initializes fine, but "Start
Encode" fails with the same Worker-connection error above **even after**
adding the missing DLLs — because `Process.Start` can't launch a subprocess
from a `\\wsl.localhost\...` UNC path. Copy the whole publish folder to a
real Windows-local path first (e.g. via `/mnt/c/...` from the WSL side) and
launch from there.

**Confirmed working end-to-end:** a real encode through the actual HandBrake
GUI (not just the CLI) on Windows, both GPUs installed, encoding at
500-700 fps on an RTX 5060 Ti. Full validation of all 4 patches through the
real user-facing application.

Not covered here: `makensis`/NSIS installer packaging (only needed for a
distributable installer, not for running/testing the app itself).

## Real-hardware validation notes (WSL)

If you're doing this kind of work in WSL2 with NVIDIA GPUs passed through,
worth knowing: both GPUs are visible to WSL via CUDA out of the box on a
current WSL2 + NVIDIA driver setup (`nvidia-smi`, `/usr/lib/wsl/lib/libcuda.so`,
`libnvidia-encode.so`, `/dev/dxg` are all present with no extra setup).
**CUDA's own driver-level device enumeration order can differ from
`nvidia-smi`'s PCI-bus order** — on the hardware this fix was developed
against, CUDA put the RTX 3090 at device 0 and the RTX 5060 Ti at device 1,
confirmed via a standalone dlopen-based probe using `ffnvcodec/dynlink_loader.h`
(the same loader `nvenc_common.c` uses) against `nv-codec-headers` (matches
the version pinned in `contrib/nvenc/module.defs`). This let the actual bug
be reproduced live in WSL, not just inferred from reading ffmpeg source.

This means real hardware-in-the-loop testing is possible for this whole class
of fix without cross-compiling to Windows first:
- A distro-packaged `ffmpeg` with `cuda` hwaccel and `av1_nvenc`/
  `hevc_nvenc`/`h264_nvenc` support (dlopens the driver at runtime, no
  special build flags needed) is enough to validate the underlying device-
  routing mechanism directly: `ffmpeg -init_hw_device cuda=cu:<N> ... -c:v
  av1_nvenc` on the non-AV1 device fails with "Codec not supported"; on the
  AV1-capable device it encodes cleanly. This is the same
  `av_hwdevice_ctx_create(..., "<N>", ...)` call patch 2 makes.
- For `libhb`-internal logic that can't be exercised without a full
  HandBrake build (there's no test harness in `libhb/`), a useful bridge is:
  port the exact logic under test into a small standalone harness built
  against the real `ffnvcodec` headers and real dlopen'd driver, run it
  against real hardware, confirm it fails against the old logic and passes
  against the new logic, then port the same structure verbatim into the
  actual `libhb/` source. The full cross-compiled `HandBrakeCLI.exe` is the
  stronger, final confirmation once it exists.

## Status

All 4 patches are implemented in `libhb/` and validated end-to-end (CLI and
GUI, real dual-GPU hardware, real encodes). Whether to upstream this as a PR
to HandBrake/HandBrake is an open question — this started as a personal-use
fix.

### Known limitations

- **Device selection is AV1-specific, not codec-aware.** The shared device
  picker always steers CUDA hardware decode (and any NVENC encode) toward
  the first AV1-capable GPU, even for pure HEVC/H.264 NVENC jobs, or for
  decode-only jobs paired with a non-NVENC encoder. On a system where the
  AV1-capable card isn't also the best HEVC/H.264 encoder, non-AV1 jobs
  would still get steered to it for no codec-relevant reason. Not an issue
  on the 2-GPU setup this fix targets (no third option to prefer), but a
  real scope narrowing worth knowing about before generalizing this beyond
  a 2-GPU box.
- **No multi-GPU load balancing.** The AV1-capable device index is a
  single process-wide value — concurrent jobs on a system with 2+
  AV1-capable GPUs all pin to whichever is found first, not split across
  them. No `--nvenc-adapter`-style override exists to control this.
