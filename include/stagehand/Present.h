#pragma once

#include "SourceAbi.h"

#include <cstdint>
#include <string>

#if defined( __APPLE__ )
	#include <OpenGL/gl3.h>
#else
	#include <GL/glew.h>
#endif

namespace stagehand
{

/// How a picture is fitted into a frame that is a different shape.
enum class Fit
{
	Contain, ///< the whole picture, letterboxed or pillarboxed
	Cover,   ///< fill the frame, cropping the overhang
	Stretch, ///< ignore the aspect entirely
	Integer  ///< whole-number pixel multiples, falling back to Contain
};

/// `sx`/`sy` are the half-extents of a quad in clip space: 1.0 fills the axis.
void ComputeFit( Fit mode, int frameWidth, int frameHeight, uint32_t pictureWidth,
				 uint32_t pictureHeight, float pixelAspect, float& sx, float& sy );

/**
	Uploads a source's frames and draws them as one quad.

	Deliberately knows nothing about FFGL, or about any host: it takes a
	framebuffer and draws into whatever is currently bound. That is what lets
	the same code serve a plugin, a test harness and a standalone viewer.

	## What it is shaped by

	**Nearest filtering is the default and it matters.** A 320-pixel-wide
	picture scaled to a 4K output with linear filtering is a blurred mess, and
	large hard-edged pixels are usually the entire reason the content looks
	like itself.

	**State is restored by hand.** Several GL helper libraries clear a binding
	to zero on scope exit rather than putting back what was there, which
	corrupts the host's state in ways that show up somewhere else entirely.
	Nothing here uses one.

	**No framebuffer object is allocated.** Drawing a textured quad needs none,
	and allocating one is a common way to unbind a texture the host was using.
*/
class Presenter
{
public:
	Presenter() = default;
	~Presenter();

	Presenter( const Presenter& )            = delete;
	Presenter& operator=( const Presenter& ) = delete;

	/// Compiles the shader and allocates the texture. `error` says why on false.
	bool Create( uint32_t width, uint32_t height, std::string& error );
	void Destroy();

	bool Ready() const { return mShader != 0 && mTexture != 0; }

	/// Uploads one frame of BGRA, bottom-up, `width * height * 4` bytes.
	void Upload( const void* pixels );

	/// True once anything has been uploaded. Drawing before that shows an empty
	/// texture, which reads as the plugin flashing black.
	bool HasPicture() const { return mUploaded; }

	void SetSmoothing( bool smooth );

	/// Draws the quad at the given half-extents. Puts GL state back afterwards.
	void Draw( float sx, float sy );

private:
	GLuint   mShader  = 0;
	GLuint   mVAO     = 0;
	GLuint   mTexture = 0;
	GLint    mScaleUniform = -1;
	GLint    mPictureUniform = -1;
	uint32_t mWidth   = 0;
	uint32_t mHeight  = 0;
	bool     mUploaded = false;
	bool     mSmooth   = false;
};

} // namespace stagehand
