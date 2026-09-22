# vocoder

A channel vocoder with the picture as the carrier, as an FFGL **effect**
(`VC01`) for Resolume Arena/Avenue. The frame is split into a Laplacian pyramid
— eight octave bands of spatial frequency plus a residual — each band is
multiplied by a gain, and the bands are summed back. C++/GLSL, CMake MODULE →
universal `.bundle` (macOS) + Windows `.dll`.

Read `AGENTS.md` before changing the pyramid, the reconstruction recursion or
the audio analysis.

## Commands (CMake)
- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- Fast dev build: add `-DCMAKE_OSX_ARCHITECTURES=arm64`
- Universal (what ships): `cmake -B build-universal -DCMAKE_BUILD_TYPE=Release`
- Build: `cmake --build build`
- Install to Resolume: `cmake --install build`
  (**not** from an agent session — it writes into Arena's Extra Effects)
- Render a frame offline: `./build/vctest --out /tmp/frame.png`
- The test card alone: `./build/vctest --card /tmp/card.png`
- List parameters: `./build/vctest --list`
- Set a control: `--set "Band 3 (4 px)=1.0" --set "Residual=0"` (repeatable, by
  display name)
- Feed a synthetic spectrum: `--feed 1.0` (without it the Audio group is
  correctly dead — the host is the only thing that supplies bins)
- Film a clip through the plugin: `./build/vctest --pipe --width 1920 --height
  1080 [--script cues.txt] [--set …] [--feed L]` — raw RGBA on stdin, raw RGBA
  on stdout, one frame at a time, on a synthetic clock at `--fps`. The fleet's
  frame format, identical to `tinseltest`, so one filming script drives any of
  them:
  `ffmpeg -i in.mov -f rawvideo -pix_fmt rgba - | ./build/vctest --pipe --width 1920 --height 1080 | ffmpeg -f rawvideo -pix_fmt rgba -s 1920x1080 -i - out.mov`
- A cue sheet is one `frame  Parameter Name  value` per line, held at the ends
  and interpolated between. A name that is not a parameter is refused rather
  than ignored.

## Browser demo
- The page: `demo/`, live at `vocoder-demo.stoatworks-labs.com`
- Deploy: `cf-run npx wrangler deploy` from the repo root (no build step)
- Verify by CONTENT, never by status code — a stale page answers 200
- The shaders in `demo/plugin.js` are `source/Shaders.cpp`'s, copied across:
  `python3 demo/tools/check_shaders.py` (also run by `tools/verify.sh`)
- Re-vendor the shared kit:
  `~/Projects/infrastructure/stoatworks-backend/resolume-demo/sync.sh vocoder`
  — never edit `demo/vendor/` by hand
- The **whole Audio group is absent** from the page, not present and dead, and
  the band arithmetic there is a hand port of `Controls.cpp` that only a reader
  checks. See AGENTS.md for why, before changing either.

## Verify
- Everything, on a fresh universal build: `tools/verify.sh`
- The reconstruction is exact: `./build/vctest --identity`
- Each band against the CPU pyramid: `./build/vctest --band`
- Audio band *k* drives picture band *k*: `./build/vctest --audio`
- The followers' time constants: `./build/vctest --envelope`
- No dead controls: `python3 tools/sweep.py [--binary build/vctest]`
- Render cost: `./build/vctest --bench` (0.66 ms/frame at 1080p, 1.89 at 4K)
- Universal + exports: `lipo -archs build-universal/Vocoder.bundle/Contents/MacOS/Vocoder`
  and `nm -gU … | grep _plugMain`
- What a host reads: `~/Projects/resolume/oxbow/build/oxbow probe <bundle>`

## Notes
- **The null is the identity.** Every gain at 1× returns the input *to the bit*,
  and `--identity` asserts exactly 0 rather than a tolerance. If that ever
  becomes approximate, the recursion in `Pyramid.h` has been changed into the
  textbook "sum the weighted bands" form and the design has been lost.
- **The reconstruction never materialises a Laplacian band.** It walks
  `R_k = EXPAND( R_{k+1} - g_k G_{k+1} ) + g_k G_k` down the pyramid. That is
  what makes the identity exact — at every gain of 1 the difference is
  literally `G - G` — and it is why there is no band buffer anywhere.
- **Everything is `texelFetch` at integer coordinates.** The reduce and expand
  kernels are exact integer-tap filters; one bilinear read anywhere adds a
  second, unaccounted-for filter and `--band` stops matching the CPU.
- **A band's name is its level's pitch, not where its power peaks.** Band 3 is
  the "4 px" band and rings with a period of about 21 px. `--band` measures it.
- **The band gain sliders are 0..4× linear, so 1× is at 0.25**, not 0.5.
- **Audio gains multiply slider gains**, never add. A band cut to 0 stays cut.
- **`Mix` and `Carrier` are dead on the defaults**, correctly — both compare a
  reconstruction against an input it is equal to. The sweep carries a context
  that cuts a band first.
- **`Attack` is dead if sampled during a decay**, correctly — a follower has one
  coefficient at a time. The sweep puts it one frame past a rising edge.
- GLSL 4.10. Reserved words to avoid as identifiers: `patch sample input output
  filter common active half layout flat`.
- `SetParamInfo` clamps a STANDARD default into 0..1 before `SetParamRange` can
  widen it, so every ranged parameter is 0..1 and `Controls.cpp` holds the
  conversions.
- Override `SetTextParameter` to return `FF_SUCCESS` for the About block, or no
  host can instantiate the plugin at all.
- `ScopedFBOBinding` does not restore the viewport; every `ffglex::Scoped*`
  clears its binding to 0 on exit rather than restoring; `FFGLFBO::Release()`
  leaks the colour texture. Allocate every buffer before binding anything.
- `vocoder_core` is an OBJECT library, not STATIC — the plugin registers itself
  from a file-scope constructor nothing references by name.
- macOS build must be universal. Verify with `lipo`, never the build log.
- FFGL id is `VC01`; the display name is `SW Vocoder`.

## What a real host has confirmed
Registered, loaded and instantiated in **Resolume Arena 7.27.1** on Windows
(win-lab, 2026-09-21), with the shaders compiling — on **Mesa llvmpipe**, a
software rasteriser, so no GPU and no timings. The x64 DLL is cross-compiled in
the Parallels guest (`cmake -A x64`, MSVC 2022, vcpkg `x64-windows-static-md`):
372,736 bytes, `dumpbin /EXPORTS` shows `plugMain`. `oxbow selftest` on x64
Windows: 120 frames, gl error 0x0, PASS, 100% lit pixels. In Arena the plugin
logged **`host clock unit decided: milliseconds`**, against seconds under oxbow
— the first time the clock-unit detection has met a real host.

## Not done yet
- Never instantiated in Arena on macOS; never run on a GPU in Resolume.
- **No real audio has ever reached it, including in Arena** — the Windows run
  got as far as instantiation and no further. What Resolume's 64 FFT bins mean
  is still assumed rather than measured (see `AGENTS.md`), and it is still the
  repo's biggest open question.
- Never installed into Extra Effects from an agent session.
- No user guide, no OpenFX port, no factory presets. The browser demo is live
  but has never been through the audio side, which it does not carry.
- `ATTRIBUTIONS.md` is still a provisional hand copy — `sync-attributions.py`
  does not know this repo. `StoatworksAbout.h` is generated by `sync-about.py`
  now, with `guide=""`; do not hand-edit it.

## Diagnostics

`source/Diag.{h,cpp}` — log file only, no crash handler (this runs inside
Resolume), no bundle command. It records which shader failed to compile, the GL
vendor/renderer/version, the host clock unit once it is decided, and — once —
whether an audio spectrum ever arrived.

    ~/Library/Logs/vocoder/vocoder.YYYY-MM-DD.log
    %LOCALAPPDATA%\vocoder\        (Windows — where the Arena run was read from)
