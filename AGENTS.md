# vocoder — orientation for another LLM (or a newcomer)

**What it is:** an FFGL 2.1 **effect** (`VC01`, `SW Vocoder`) for Resolume
Arena/Avenue that treats the picture as a vocoder's carrier and the audio as its
modulator. C++17 + GLSL 4.10, CMake, universal macOS `.bundle` and a Windows
`.dll`. MIT. Intended home `github.com/stoatworks-labs/vocoder`.

`CLAUDE.md` is the command reference — build, verify, the one-line rules. This
file is the *why*: read it before touching the pyramid, the reconstruction
recursion, or the audio analysis.

---

## The one idea

A channel vocoder splits the **carrier** into frequency bands and sets each
band's gain from the matching band of the **modulator**. Here the carrier is the
picture and its frequency axis is **spatial**: the frame becomes a Laplacian
pyramid of eight octave bands plus a residual, each band is multiplied by a
gain, and the bands are summed back.

Almost everything else follows from that rather than having been arranged.

- **The null is the identity.** Every gain at 1× returns the input exactly. An
  effect whose neutral position is provably "do nothing" can be dropped on a
  layer without deciding anything first, and every other claim can be stated as
  a departure from it.
- **Without audio it is still a whole plugin.** Nine faders across scale is a
  graphic EQ for detail: cut at 4 px and only 4 px detail goes; boost and the
  picture rings at that scale; tilt and it sharpens or softens. That this is
  useful with nothing routed to it is the reason the audio side can be honest
  about being a modulation source rather than the point.
- **"Spectrum only" is not a mode.** It is the Residual fader at zero, which
  removes the picture's DC and coarsest shapes and leaves the detail bands. No
  branch anywhere implements it.
- **Audio multiplies, never adds.** A band cut to zero stays cut however loud
  the music is. That is what makes the two halves composable instead of
  fighting.

### The exactness is load-bearing, and it is not luck

The obvious reconstruction stores every Laplacian band, scales each and sums
them. It costs a full-resolution buffer per band and its exactness depends on
the buffer precision.

This one walks a recursion down the pyramid instead:

    R_L = r · G_L
    R_k = EXPAND( R_{k+1} − g_k · G_{k+1} ) + g_k · G_k

By linearity that equals `Σ g_k L_k + r G_L` with `L_k = G_k − EXPAND(G_{k+1})`,
so it is the same effect. But when every gain is 1, the thing being expanded is
`G_{k+1} − G_{k+1}` — **identically zero in floating point** — and the output is
`G_0` to the bit. `vctest --identity` therefore asserts a maximum difference of
exactly **0**, not a tolerance, at 720p, 1080p and 4K.

If someone ever "simplifies" this into the textbook form, that check will start
reporting 1e-7 and the temptation will be to loosen it. Do not. The zero is the
design.

---

## What is genuinely verified, and what is assumed

**Verified, by measurement, on this machine (M4 Max, macOS 26.4), through
`tools/verify.sh` against a fresh universal Release build:**

- **The reconstruction is exact.** Max difference **0** over RGBA at 1280×720,
  1920×1080 and 3840×2160, every gain at 1×. Through the Luma carrier — which
  adds a Y/Cb/Cr round trip — **5.96e-08**.
- **The bands are a partition, not merely something that cancels.** The eight
  single-band cuts sum to `(L−1)·input + G_L` to **9.54e-07**. This is the check
  that matters more than the identity, because it drives every expand at every
  level with a *non-zero* difference; the identity's zeros could hide a broken
  expand and this cannot.
- **The shipped shaders compute what the CPU pyramid computes.** `vctest --band`
  puts a one-pixel vertical line through the GPU, reads the row back, and
  compares it against `Pyramid.cpp`'s one-dimensional Burt–Adelson: worst
  disagreement **1.2e-07** over all eight bands. The comparison is legitimate
  because a vertical line is constant in y, so every vertical pass returns its
  input and the row *is* the 1-D pipeline.
- **Each band is an octave band-pass in the right place.** Measured, by the
  radial peak of the band's own power spectrum:

  | band | level pitch | peak period | ÷ pitch |
  |---|---|---|---|
  | 1 | 1 px | 2.00 px | 2.00 |
  | 2 | 2 px | 10.11 px | 5.05 |
  | 3 | 4 px | 21.47 px | 5.37 |
  | 4 | 8 px | 43.52 px | 5.44 |
  | 5 | 16 px | 87.32 px | 5.46 |
  | 6 | 32 px | 174.54 px | 5.45 |
  | 7 | 64 px | 347.04 px | 5.42 |
  | 8 | 128 px | 677.13 px | 5.29 |

  The peaks double, as an octave bank must. They sit at about **5.4× the level's
  pitch** rather than at the pitch itself, which is the actual physics of a
  Burt–Adelson band — the generating kernel's response is `cos⁴(ω/2)`, so one
  reduce-and-expand is `cos⁸(ω/2)` and the band is centred well below its
  level's Nyquist. Band 1 is the exception at exactly 2.00 px, because
  `1 − cos⁸(ω/2)` peaks at Nyquist. **The sliders are named for the pitch, not
  the peak**, and the README says so; naming them for the peak would be more
  accurate and less usable.
- **Audio band *k* drives picture band *k* and no other.** Sixteen cases (eight
  bands × two mappings) exact as gains, and ten of them re-checked *through the
  picture*: the frame rendered with audio in band *k* is byte-identical (max
  difference **0**) to the frame rendered with the corresponding slider at the
  gain the Drive implies. That second half is what proves the gain reaches the
  right level of the pyramid rather than the right slot in an array.
- **The followers keep their time constants.** Attack and release measured to
  **0.0%** error at three settings, by finding where a step crosses 63.2% and
  36.8% with log interpolation between frames (exact for an exponential, so
  60 fps does not limit the measurement).
- **No dead controls.** All **19** swept parameters measurably change the
  picture. Two are skipped with reasons: `Audio` (the FFT buffer, whose scalar
  value is meaningless) and the About block (browser buttons).
- **The bundle is what a host thinks it is.** `oxbow probe` reads **SW
  Vocoder**, `VC01`, effect, 2.1, 24 parameters in four groups, no name
  truncated. A local build is universal (`arm64 x86_64`), exports `plugMain`, ad-hoc signs, and
  `CFBundleExecutable`/`CFBundleIdentifier` both check out.
- **Render cost**, 60 frames after a 20-frame warm-up with `glFinish` on both
  sides: **0.77 ms** at 720p, **0.66 ms** at 1080p, **1.89 ms** at 4K. 1080p
  being faster than 720p is real and repeatable: there are 33 passes for eight
  levels and most are tiny, so per-pass overhead dominates until the pixels
  start to matter.

**Verified in a real host, on Windows, 2026-09-21:**

Everything above is macOS and offline. On 2026-09-21 the plugin met **Resolume
Arena 7.27.1** (build 15990) on **win-lab** — an x64 Windows 11 Pro VM with no
GPU, so OpenGL came from **Mesa llvmpipe** dropped in beside Arena
(`opengl32.dll` + `libgallium_wgl.dll`, `GALLIUM_DRIVER=llvmpipe`). The plugin
reported the renderer itself:
`Mesa … llvmpipe (LLVM 22.1.8, 256 bits) … 4.5 (Core Profile) Mesa 26.2.0`.

- **The x64 DLL builds and exports the entry point.** Cross-compiled in the
  Parallels guest on the Mac (ARM64 Windows 11, MSVC 2022 Build Tools,
  `cmake -A x64`, vcpkg triplet `x64-windows-static-md`) — the same route the
  fleet's `~/Projects/resolume/winbuild` scripts use. There is no x64 Windows
  machine in the build loop. **372,736 bytes**, and `dumpbin /EXPORTS` shows
  `plugMain`.
- **Arena registers it.** Arena's own REST API lists **SW Vocoder** among 112
  video effects, under `idstring` `VC01`, with the description the plugin
  declares.
- **Arena loads the DLL.** The plugin wrote `plugin loaded build=<stamp>` to its
  diag log under `%LOCALAPPDATA%\vocoder\`, with the stamp of the DLL built
  minutes earlier.
- **Arena instantiates it and the shaders compile.** It was applied from Arena's
  own effects browser and logged the GL strings followed by `initialised`, and
  Arena drew its inspector for it, groups and all.
- **The host clock unit detection works in a real host.** Under oxbow the plugin
  sees seconds; in Arena it decided **milliseconds** — it logged `host clock
  unit decided: milliseconds` there against `scale 1.0 (seconds)` offline. This
  is the first time that code has met a real host, and it is the one piece of
  the millisecond-bug machinery that could only ever be confirmed in one.
- **It instantiates and renders headlessly on x64 Windows too.** oxbow, built
  x64 in the same guest, ran `selftest`: **120 frames, gl error 0x0, PASS**, and
  921,600/921,600 lit pixels (100%).
- **No warnings or errors.** The diag log is clean of WARN/ERROR/FAIL.

**Assumed, or not done:**

- **No GPU was involved on Windows, and nothing was timed there.** Everything in
  Arena ran on llvmpipe, a software rasteriser. Nothing from that run says
  anything about performance on Windows; the ms/frame figures above stay
  macOS-only. It has **never run on a GPU in Resolume**.
- **Never instantiated in Arena on macOS.** The macOS numbers above were all
  compiled, rendered and measured offline against the real plugin class in a
  headless CGL context.
- **No real audio has ever reached it, in Arena or anywhere else.** The harness
  writes a synthetic spectrum. See the trap below — this is still the biggest
  open question in the repo, and the Windows run did not touch it.
- **No long session, no composition save/reload, no preset recall in the host.**
  The effect was applied to the **composition**, not to a clip:
  `/api/v1/…/clips/1` still showed only `Transform` afterwards, so the proof of
  instantiation is the diag log, not the clip's effect list.
- **No user guide, no OpenFX port, no browser demo, no factory presets.**
  `StoatworksAbout.h` is **generated** by `sync-about.py` now — the project is
  registered in the website's `projects.json`, in that script's TARGETS and in
  `attributions/names.json` — so do not hand-edit it; it still carries `guide=""`
  because no user guide exists. A `static_assert` in `Vocoder.cpp` fires if a
  regenerated About header changes the button count. `ATTRIBUTIONS.md` is still a
  provisional hand copy, because `sync-attributions.py`'s master lists do not know
  this repo yet.

---

## The traps

Ordered by how much time they will cost you.

**What Resolume's FFT bins mean is an assumption, and it is written down in one
place.** FFGL says a buffer with `FF_USAGE_FFT` "expects a spectrum" and says
nothing whatever about its frequency axis, its scale or its units.
`Audio.h`'s `kBandEdges` assumes **64 bins linear in frequency from 0 to
Nyquist** and partitions them as close to geometrically as that allows. Nothing
in this fleet has measured it — regauss, tinsel and macroblock all split at
fixed indices and call the bottom slice the woofer, which is the same assumption
wearing different trousers. If the bins turn out to be log-spaced, the partition
is still monotone, the bands still do not overlap, and every check in the
harness still passes; only the frequencies quoted in `Audio.h` are wrong.

The first host run has now happened — Arena 7.27.1 on win-lab, 2026-09-21 — and
it **did not settle this**: no real audio reached the plugin, because the run
proved registration, loading and instantiation and got no further. The
assumption is exactly where it was. **The next host run should check this before
anything else**, and it needs a host with audio actually routed in, which
win-lab (a headless VM on a software rasteriser) is not.

**An ssh session on Windows has no desktop, so Arena must be launched through a
scheduled task.** An ssh login lands on the *service* window station, which has
no desktop at all: Arena started from there sits at about 31 MB doing nothing,
never draws, and cannot be screenshotted. It has to be started in the console
session (session 1) through the scheduled-task wrapper `C:\arena-lab\s1.ps1`.
Every observation in the Windows run above depended on that, and an hour goes
into rediscovering it.

**Arena's REST API can tell you the plugin is registered, but it cannot
instantiate it for you.** `/api/v1/effects` and `/api/v1/sources` list every
plugin by its **FFGL id** as `idstring` — `VC01` here, not `SW Vocoder` — which
is how registration was proven. But the add-effect endpoint **returns 200
without adding anything**: nothing appears, and no log line is written.
Instantiation has to be driven from Arena's own effects browser in the GUI
(double-click applies to the current selection). Do not read a 200 from that
endpoint as a plugin that loaded.

**A follower has one coefficient at a time, so Attack is dead in a decay.** The
sweep reported `Attack` DEAD and was entirely right: it sampled the last frame
of a 20-frame run, which lands mid-decay, where the envelope is on its release
coefficient and has long since converged on the falling signal — how fast it
rose 200 ms earlier leaves no trace. Attack is now swept one frame past a rising
edge and Release deep in a decay, each with the other set fast so the state
being inherited is identical in both runs. A conditional control needs the
condition that makes it *mean* something, and for a time constant that condition
is which way the signal is going.

**A sampled step arrives one frame before the frame that filters it.** The
spectrum for frame N is written and then consumed by frame N's own update, which
advances the follower by a whole frame from frame N−1's time — so in the sampled
system the step happened at frame N−1. Measured from the onset frame itself,
every attack reads exactly one frame fast: the first version of `--envelope`
reported 0.0057 s for a 0.0224 s time constant and 0.1833 for 0.2000, both one
frame (16.7 ms) early, and the implementation was correct all along. The check
now references `(onset − 1)` and the errors are 0.0%.

**Cutting the residual clamps, because the residual is the DC.** The first
partition check summed all nine cuts — eight bands and the residual — and
reported 0.06 against a plugin that partitions to 1e-6. Removing the residual
leaves the picture centred on zero, half of it negative, and the output pass
clamps to the host's range, as it must. The residual is not one of the cuts any
more, and the algebra was adjusted to match: `Σ_k (in − L_k) = (L−1)·in + G_L`,
where `G_L` is one extra render with every band cut. A check that fails because
the *check* is outside the domain is worse than no check, because it trains you
to loosen tolerances.

**The harness must flip the card, and the flip is invisible until it is not.**
FFGL textures are bottom-row-first and the card is built top-row-first. The
first `--identity` compared the readback against the unflipped card and reported
**0.83** — a plugin that was, in fact, exact to the bit. A number that large
looks like a broken algorithm and sent the first investigation into the pyramid;
it was a transpose in the *test*. The flip now happens once, in `Session::begin`.

**A harness that calls the plugin in a tight loop has no clock.** The host-clock
calibration votes on the ratio of host time to wall time, and a test driving
`UpdateAudioForTest` in a loop delivers microseconds of wall time per frame, so
nothing ever votes, the scale stays undecided, and the followers run on a clock
that does not move. Every entry point the harness uses declares the unit
outright (`SetClockScaleForTest( 1.0 )`) rather than letting the calibration
infer one. An implicit unit is what cost this fleet the millisecond bug.

**`ScopedFBOBinding` does not restore the viewport.** It restores the
framebuffer binding and only that (SDK `b1afaf9`). Every pass's
`ResizeViewPort()` leaks into the next, and the output pass draws into the
host's own framebuffer, which has no buffer of its own to size itself from.
`ProcessOpenGL` captures the host viewport up front and restores it before the
output pass. Here it would happen to survive — the last expand is already at
picture size — and *luck is not a viewport*.

**Every `ffglex::Scoped*` binding clears to 0 on exit rather than restoring, and
`FFGLFBO::Initialise` allocates under one.** So allocating a buffer silently
unbinds the input texture from the active unit. The symptom is the dangerous
part: correct on every frame except the one that allocates. Every `Ensure()` in
`ProcessOpenGL` therefore happens before anything binds a texture — and this
plugin allocates *twenty-five* buffers, so it has twenty-five chances to get
that wrong.

**`ffglex::FFGLFBO::Release()` leaks the colour texture.** It deletes the
framebuffer and the depth renderbuffer, then tests `depthBufferID` a second time
where it plainly meant `colorTextureID`. `PassBuffer::Destroy()` deletes it
first. With 25 buffers per instance this is not pedantry.

**A ranged parameter cannot have a ranged default.** `SetParamInfo` clamps an
`FF_TYPE_STANDARD` default into 0..1 *before* returning, and `SetParamRange` can
only be called afterwards. So every host parameter here is 0..1 and the
conversions live in `Controls.cpp` — including the band gains, where 1× is at
**0.25** and not 0.5.

**`SetTextParameter` must return `FF_SUCCESS` for the About block.**
`instantiateGL` pushes every declared default back through the setters and
deletes the instance the moment one returns `FF_FAIL`, which is exactly what
`CFFGLPlugin`'s stub does. Omit the override and the plugin cannot be created in
any real host, while every in-repo harness still passes — because they drive the
class directly and never go through `plugMain`.

**The plugin registers itself from a file-scope constructor.** `CFFGLPluginInfo`
is never referenced by name, so in a STATIC archive the linker may drop the whole
translation unit, giving a bundle that loads, exports `plugMain`, and reports
that it contains no plugins. `vocoder_core` is an OBJECT library and
`EffectPlugin.cpp` is listed directly in the MODULE target for that reason.

**`oxbow selftest` is the wrong check for an effect.** It renders with no input
texture, and this plugin's `ProcessOpenGL` correctly returns `FF_FAIL` when
handed no picture, so a FAIL from it would say nothing. `verify.sh` uses `oxbow
probe` and asserts the name, the id and the type — which is what the brief for
this repo asks for and what a host actually reads.

On win-lab the x64 `oxbow selftest` nonetheless reported **120 frames, gl error
0x0, PASS** with 921,600/921,600 lit pixels. That was observed, not explained:
whatever that build hands the plugin is evidently enough for `ProcessOpenGL` to
render a full frame. Read the PASS as evidence the DLL instantiates and renders
on x64 Windows, not as a check of the effect's output, and leave `verify.sh` on
`probe`.

---

## Shape of the code

    source/Pyramid.{h,cpp}   the maths, on the CPU, in 1-D. The reference.
    source/Shaders.{h,cpp}   the same arithmetic in 2-D and separable: five
                             shaders, two of which run once per level per axis.
    source/Audio.{h,cpp}     64 bins to 8 bands, and the envelope followers.
    source/Controls.{h,cpp}  0..1 host parameters to physical units; Compose().
    source/PassBuffer.*      FFGLFBO with the leak fixed. Nearest only, here.
    source/Vocoder.*         the plugin: parameters, buffers, the passes.
    source/EffectPlugin.cpp  the CFFGLPluginInfo, and nothing else.
    source/Diag.*            a log file, for the shader that will not compile.
    tools/vctest/            the offline harness: four checks and a bench.
    tools/sweep.py           no control is silently dead.
    tools/verify.sh          all of it, on a fresh universal build.

**Five shaders, 33 passes at eight levels.** `copy` once; `reduce` twice per
level (one axis each); `expandV` and `expandH` once each per level on the way
back down. `expandH` with `Final` set is also the output pass — Y/Cb/Cr back to
RGB, Master, Mix, clip, straight into the host's framebuffer.

`R_L` is never materialised (it is `r · G_L`, and the top `expandV` reads `G_L`
with that gain) and `R_0` is the host's framebuffer, which is why there are
`levels − 1` recon buffers and not `levels + 1`.

**Levels are dropped, not faked, when the picture is too small.**
`ActiveLevels()` stops when a Gaussian would be under two texels on a side, so a
320×180 frame has seven bands and the eighth slider does nothing — the honest
answer, and the reason the CI sweep runs at 320×180 with a note rather than
somewhere smaller.

---

## Siblings

The CMake MODULE + FFGL-submodule pattern, the `Diag` logger, `PassBuffer`, the
`StoatworksAbout*` block, the offline-harness shape, `sweep.py` and `verify.sh`
all come from **tinsel** and **graticule**, with the audio input taken from
**regauss** and **macroblock**. The band split in `Audio.h` deliberately matches
the fleet's, so a patch that follows "low" here follows the same thing there.
Cross-cutting fleet knowledge lives in
[fleet-notes](https://github.com/stoatworks-labs/fleet-notes).
