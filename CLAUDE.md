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

## Not done yet
- Never loaded into Resolume, and never installed into Extra Effects.
- No real audio spectrum has ever reached it; what Resolume's FFT bins mean is
  assumed (see `AGENTS.md`).
- No release tag, no website registration, no browser demo, no OpenFX port, no
  factory presets. `StoatworksAbout.h` and `ATTRIBUTIONS.md` are provisional
  hand copies with `guide=""`.

## Diagnostics

`source/Diag.{h,cpp}` — log file only, no crash handler (this runs inside
Resolume), no bundle command. It records which shader failed to compile, the GL
vendor/renderer/version, the host clock unit once it is decided, and — once —
whether an audio spectrum ever arrived.

    ~/Library/Logs/vocoder/vocoder.YYYY-MM-DD.log
