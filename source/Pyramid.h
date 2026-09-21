#pragma once

#include <vector>

/**
    The Laplacian pyramid, as arithmetic.

    This is the plugin's maths written down once, on the CPU, in one dimension.
    The shaders in Shaders.cpp are the same arithmetic in two dimensions and
    separable; the harness's `--band` check puts a one-pixel vertical line
    through the GPU, reads the row through it back, and compares that row with
    `Reconstruct()` here. Because a vertical line is constant in y, every
    vertical pass acts on a constant and returns it (the kernel sums to one and
    the taps are clamped, not zero-padded), so the row through the middle of
    the frame IS the one-dimensional pipeline -- and the comparison is between
    the shipped shader and this file, to float precision, not between two
    pictures that look alike.

    **Burt and Adelson, exactly.** The generating kernel is [1 4 6 4 1]/16.
    REDUCE convolves and keeps the even samples; EXPAND inserts zeros between
    samples, convolves with the same kernel and doubles, which is REDUCE's
    adjoint and has unit DC gain on both parities. Sizes halve as ceil( n/2 ),
    so a 1080-line picture has levels of 540, 270, 135, 68, 34, 17, 9 and 5.
    Taps that fall outside a level are clamped to its edge.

    **Reconstruction is exact by construction, and it does not go through the
    Laplacian bands at all.** The obvious form stores every band, multiplies
    each by its gain and sums them back. That costs a full-resolution buffer
    per band and, worse, its exactness depends on the buffer precision. The
    recursion used here is

        R_L     = r * G_L                              (the residual)
        R_k     = EXPAND( R_{k+1} - g_k * G_{k+1} ) + g_k * G_k

    which by linearity equals sum_k g_k L_k + r G_L, with L_k = G_k -
    EXPAND( G_{k+1} ) -- but when every gain is 1 the thing being expanded is
    `G_{k+1} - G_{k+1}`, which is exactly zero in floating point, and the
    output is G_0 to the bit. So `vctest --identity` reports a maximum error
    of zero at every size, and that is not a rounding coincidence.

    Nothing here is GL. It is compiled into the plugin's object library so the
    harness and the plugin agree about level sizes and level counts, and so a
    change to the kernel has to be made in the shader and here, where the
    check will notice if it is made in only one.
*/
namespace vocoder::pyramid
{

/// Bands the plugin has. Fixed; the eighth is 128 px.
constexpr int kMaxLevels = 8;

/// The generating kernel. Mirrored as a literal in Shaders.cpp.
constexpr float kKernel[ 5 ] = { 1.0f / 16.0f, 4.0f / 16.0f, 6.0f / 16.0f, 4.0f / 16.0f, 1.0f / 16.0f };

/// The size of the next level down: ceil( n / 2 ).
int ReducedSize( int n );

/// How many Laplacian levels a picture of this size supports, up to
/// kMaxLevels. A level exists while the Gaussian below it is at least two
/// texels in both directions -- below that a 5-tap kernel is sampling one
/// texel five times and the band it would produce is noise about nothing.
/// So a 320x180 picture has 7 bands and its residual is the 3x2 Gaussian; a
/// 640x360 picture and everything larger has all 8.
int ActiveLevels( int width, int height );

/// REDUCE: `out` has ReducedSize( in.size() ) samples.
void Reduce( const std::vector< float >& in, std::vector< float >& out );

/// EXPAND to `outSize` samples, which must be a size `in` could have been
/// reduced from (2n or 2n-1).
void Expand( const std::vector< float >& in, int outSize, std::vector< float >& out );

/// The whole thing: build the pyramid of `signal` with `levels` Laplacian
/// levels, weight band k by `gains[ k ]` and the residual by `residual`, and
/// reconstruct with the recursion above. This is what the shaders compute
/// along any row of a vertically constant picture.
std::vector< float > Reconstruct( const std::vector< float >& signal, const float gains[ kMaxLevels ],
                                  float residual, int levels );

} // namespace vocoder::pyramid
