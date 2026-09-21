#pragma once

#include <FFGLSDK.h>

namespace vocoder
{
/**
    An off-screen buffer for one stage of the chain.

    Three things on top of the SDK's FFGLFBO.

    **It reallocates only when it has to.** Ensure() is called every frame for
    every level of the pyramid -- twenty-odd buffers -- and is a no-op in the
    overwhelming majority of them. It reallocates when the picture changes
    size and never otherwise, so this path has to be cheap.

    **It actually frees its colour texture.** `ffglex::FFGLFBO::Release()`
    deletes the framebuffer and the depth renderbuffer, then tests
    `depthBufferID` a second time where it plainly meant `colorTextureID` --
    so the colour texture is leaked on every release (SDK b1afaf9,
    `FFGLFBO.cpp`). `Destroy()` deletes it first.

    **It owns its filtering.** Every buffer in this plugin is read with
    `texelFetch` at integer coordinates -- the pyramid's reduce and expand
    kernels are exact integer-tap filters, and a bilinear fetch anywhere in
    the chain would silently add a second, unaccounted-for filter to it. So
    everything here is `Sampling::Nearest`. `Linear` and `Mipmapped` are kept
    because this file is copied around the fleet (tinsel, where it started,
    uses all three) and a PassBuffer that differs per repo is a PassBuffer
    nobody can copy.
*/
class PassBuffer : public ffglex::FFGLFBO
{
public:
	enum class Sampling
	{
		Nearest,  ///< for data read texel-for-texel. No filtering, no mip chain.
		Linear,   ///< for pictures read between texels. Bilinear, no mip chain.
		Mipmapped ///< for pictures that also get reduced. Trilinear + GenerateMipmaps().
	};

	~PassBuffer();

	/// Allocate at this size and format, reusing the existing buffer if it
	/// already matches. Newly allocated buffers are cleared: a buffer whose
	/// contents are undefined is not "a bit of noise on the first frame", it is
	/// whatever texture memory the driver handed back.
	bool Ensure( GLsizei requestedWidth, GLsizei requestedHeight, GLint format, Sampling sampling );

	/// Rebuild the mip chain from level 0. Call after rendering into a
	/// Sampling::Mipmapped buffer and before anything samples it; a stale chain
	/// does not look like an error, it looks like the wrong footage.
	void GenerateMipmaps();

	/// Highest mip level this buffer has, i.e. the 1x1 one. `textureQueryLevels`
	/// is GLSL 4.30 and these shaders are 4.10.
	float MaxMipLevel() const;

	/// Clear to transparent black.
	void Clear();

	/// The colour texture, for binding as an input to a later pass.
	///
	/// The SDK keeps `colorTextureID` protected and offers only
	/// `GetTextureInfo()`, which builds and returns an `FFGLTextureStruct` --
	/// six fields assembled to reach one of them, at every bind of every pass
	/// of every frame. A subclass can just say which texture it is.
	GLuint TextureID() const
	{
		return colorTextureID;
	}

	/// Release everything, including the colour texture the SDK forgets.
	void Destroy();

	bool IsValid() const
	{
		return GetGLID() != 0;
	}

private:
	Sampling sampling = Sampling::Nearest;
};

} // namespace vocoder
