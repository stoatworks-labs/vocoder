#pragma once

/**
    The passes, as GLSL source.

    Four fragment shaders and one vertex shader. Two of them run many times a
    frame with different sizes and different textures, which is what makes an
    eight-level pyramid five shaders rather than thirty-three:

    1. **copy**     picture size, once. The host's texture into a float buffer
                    of ours, and RGB into Y/Cb/Cr when the carrier is Luma.
    2. **reduce**   twice per level, once per axis: a 5-tap Gaussian that
                    keeps the even samples. Builds G_1 .. G_L.
    3. **expandV**  once per level, on the way back up: the vertical half of
                    EXPAND, applied to `ReconGain * R_{k+1} - LevelGain * G_{k+1}`.
                    Two textures at every tap; that difference is the whole
                    trick, see Pyramid.h.
    4. **expandH**  once per level: the horizontal half, plus
                    `LevelGain * G_k`, which finishes R_k. With `Final` set
                    it is also the output pass -- Y/Cb/Cr back to RGB, Master,
                    Mix, and straight into the host's framebuffer.

    Every fetch is `texelFetch` at an integer coordinate with the index
    clamped by hand. The kernels are exact integer-tap filters, and a
    bilinear read anywhere in the chain would be a second, unaccounted-for
    filter on top of them -- one the CPU reference in Pyramid.cpp does not
    have, so `vctest --band` would fail, which is the point of it.

    The kernel weights are a literal here and a constexpr in Pyramid.h. The
    `--band` check is what keeps them the same.
*/
namespace vocoder
{

extern const char* const kVertexShader;
extern const char* const kCopyShader;
extern const char* const kReduceShader;
extern const char* const kExpandVShader;
extern const char* const kExpandHShader;

} // namespace vocoder
