#include "stagehand/Present.h"

#include <algorithm>
#include <vector>

namespace stagehand
{
namespace
{

/*
	One quad built from gl_VertexID. No vertex buffer, because there is no
	geometry worth storing -- four corners derived from an integer.

	Mind the names in any shader added here: `filter`, `active`, `flat`,
	`input`, `output`, `sample`, `common` and `patch` are GLSL reserved words,
	and a shader that fails to compile surfaces only at runtime, as a plugin
	that appears to do nothing.
*/
const char* kVertexShader = R"(#version 410 core
uniform vec2 Scale;

out vec2 vUv;

void main()
{
	vec2 corner = vec2( float( gl_VertexID & 1 ), float( ( gl_VertexID >> 1 ) & 1 ) );

	vUv         = corner;
	gl_Position = vec4( ( corner * 2.0 - 1.0 ) * Scale, 0.0, 1.0 );
}
)";

const char* kFragmentShader = R"(#version 410 core
uniform sampler2D Picture;

in vec2 vUv;
out vec4 fragColour;

void main()
{
	fragColour = vec4( texture( Picture, vUv ).rgb, 1.0 );
}
)";

GLuint compile( GLenum type, const char* source, std::string& error )
{
	GLuint shader = glCreateShader( type );
	glShaderSource( shader, 1, &source, nullptr );
	glCompileShader( shader );

	GLint ok = GL_FALSE;
	glGetShaderiv( shader, GL_COMPILE_STATUS, &ok );
	if( ok == GL_TRUE )
		return shader;

	GLint length = 0;
	glGetShaderiv( shader, GL_INFO_LOG_LENGTH, &length );
	std::vector< char > log( size_t( length > 0 ? length : 1 ), '\0' );
	glGetShaderInfoLog( shader, length, nullptr, log.data() );
	error = log.data();

	glDeleteShader( shader );
	return 0;
}

} // namespace

void ComputeFit( Fit mode, int frameWidth, int frameHeight, uint32_t pictureWidth,
				 uint32_t pictureHeight, float pixelAspect, float& sx, float& sy )
{
	sx = 1.0f;
	sy = 1.0f;

	if( frameWidth <= 0 || frameHeight <= 0 || pictureWidth == 0 || pictureHeight == 0 )
		return;

	if( mode == Fit::Stretch )
		return;

	if( mode == Fit::Integer )
	{
		/*
			Whole-number pixel multiples only. Aspect correction is deliberately
			ignored in this mode: the entire point is that one source pixel is
			an exact square block of output pixels, and a non-integer aspect
			correction makes that impossible by definition.
		*/
		const int kx = frameWidth / int( pictureWidth );
		const int ky = frameHeight / int( pictureHeight );
		const int k  = std::max( 1, std::min( kx, ky ) );

		sx = float( pictureWidth * uint32_t( k ) ) / float( frameWidth );
		sy = float( pictureHeight * uint32_t( k ) ) / float( frameHeight );

		// A picture larger than the frame cannot be shown at 1x or more. Fall
		// through to Contain rather than overflowing silently.
		if( sx <= 1.0f && sy <= 1.0f )
			return;

		/*
			Reset before falling through. Without this the Contain branch below
			assigns only ONE axis -- that is its whole trick -- and the other
			keeps the oversized value computed just above, so a picture too big
			for the frame is drawn overflowing on one side. Invisible in the
			common case because Integer usually fits.
		*/
		sx = 1.0f;
		sy = 1.0f;
	}

	const float par = pixelAspect > 0.0f ? pixelAspect : 1.0f;
	const float pictureAspect =
		( float( pictureWidth ) / float( pictureHeight ) ) * par;
	const float frameAspect = float( frameWidth ) / float( frameHeight );

	const bool wider = pictureAspect > frameAspect;
	const bool cover = mode == Fit::Cover;

	// Contain shrinks the long axis to bring the whole picture in; Cover grows
	// the short one until nothing is uncovered. Same comparison, opposite branch.
	if( wider != cover )
		sy = frameAspect / pictureAspect;
	else
		sx = pictureAspect / frameAspect;
}

Presenter::~Presenter()
{
	// No GL calls here: a destructor may well run with no current context, and
	// deleting a name in the wrong context is worse than leaking one. Hosts
	// call Destroy from wherever they were given a context.
}

bool Presenter::Create( uint32_t width, uint32_t height, std::string& error )
{
	Destroy();

	if( width == 0 || height == 0 )
	{
		error = "a picture with no size";
		return false;
	}
	mWidth  = width;
	mHeight = height;

	std::string log;
	GLuint      vs = compile( GL_VERTEX_SHADER, kVertexShader, log );
	if( !vs )
	{
		error = "vertex shader: " + log;
		return false;
	}
	GLuint fs = compile( GL_FRAGMENT_SHADER, kFragmentShader, log );
	if( !fs )
	{
		glDeleteShader( vs );
		error = "fragment shader: " + log;
		return false;
	}

	mShader = glCreateProgram();
	glAttachShader( mShader, vs );
	glAttachShader( mShader, fs );
	glLinkProgram( mShader );
	glDeleteShader( vs );
	glDeleteShader( fs );

	GLint linked = GL_FALSE;
	glGetProgramiv( mShader, GL_LINK_STATUS, &linked );
	if( linked != GL_TRUE )
	{
		GLint length = 0;
		glGetProgramiv( mShader, GL_INFO_LOG_LENGTH, &length );
		std::vector< char > buf( size_t( length > 0 ? length : 1 ), '\0' );
		glGetProgramInfoLog( mShader, length, nullptr, buf.data() );
		error = std::string( "link: " ) + buf.data();
		glDeleteProgram( mShader );
		mShader = 0;
		return false;
	}

	mScaleUniform   = glGetUniformLocation( mShader, "Scale" );
	mPictureUniform = glGetUniformLocation( mShader, "Picture" );

	// A core profile refuses to draw with no vertex array bound, even though
	// the shader builds its geometry from gl_VertexID and sources nothing.
	glGenVertexArrays( 1, &mVAO );

	glGenTextures( 1, &mTexture );
	glActiveTexture( GL_TEXTURE0 );
	glBindTexture( GL_TEXTURE_2D, mTexture );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, GLsizei( mWidth ), GLsizei( mHeight ), 0,
				  GL_BGRA, GL_UNSIGNED_BYTE, nullptr );

	// CLAMP_TO_EDGE, not the default REPEAT. The picture fills the texture, so
	// REPEAT shows only at the seam -- but it shows there as a one-pixel stripe
	// of the opposite edge, which across the bottom of a picture is a bright
	// line nobody can explain.
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0 );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0 );
	SetSmoothing( mSmooth );
	glBindTexture( GL_TEXTURE_2D, 0 );

	mUploaded = false;
	return true;
}

void Presenter::Destroy()
{
	if( mShader )
	{
		glDeleteProgram( mShader );
		mShader = 0;
	}
	if( mVAO )
	{
		glDeleteVertexArrays( 1, &mVAO );
		mVAO = 0;
	}
	if( mTexture )
	{
		glDeleteTextures( 1, &mTexture );
		mTexture = 0;
	}
	mUploaded = false;
	mWidth = mHeight = 0;
}

void Presenter::SetSmoothing( bool smooth )
{
	mSmooth = smooth;
	if( !mTexture )
		return;

	const GLint filter = smooth ? GL_LINEAR : GL_NEAREST;
	glActiveTexture( GL_TEXTURE0 );
	glBindTexture( GL_TEXTURE_2D, mTexture );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter );
}

void Presenter::Upload( const void* pixels )
{
	if( !mTexture || !pixels )
		return;

	glActiveTexture( GL_TEXTURE0 );
	glBindTexture( GL_TEXTURE_2D, mTexture );
	glPixelStorei( GL_UNPACK_ALIGNMENT, 4 );

	// BGRA because that is the byte order a little-endian XRGB8888 framebuffer
	// already has, so the common case costs no conversion at all.
	glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, GLsizei( mWidth ), GLsizei( mHeight ),
					 GL_BGRA, GL_UNSIGNED_BYTE, pixels );
	mUploaded = true;
}

void Presenter::Draw( float sx, float sy )
{
	if( !Ready() )
		return;

	glBindVertexArray( mVAO );
	glUseProgram( mShader );

	glActiveTexture( GL_TEXTURE0 );
	glBindTexture( GL_TEXTURE_2D, mTexture );

	if( mPictureUniform >= 0 )
		glUniform1i( mPictureUniform, 0 );
	if( mScaleUniform >= 0 )
		glUniform2f( mScaleUniform, sx, sy );

	glDisable( GL_BLEND );
	glDrawArrays( GL_TRIANGLE_STRIP, 0, 4 );

	// Put it back by hand. Nothing here leaves a binding cleared to zero,
	// because the host's next draw is entitled to whatever it had set up.
	glUseProgram( 0 );
	glBindVertexArray( 0 );
	glBindTexture( GL_TEXTURE_2D, 0 );
	glEnable( GL_BLEND );
	glBlendFunc( GL_ONE, GL_ONE_MINUS_SRC_ALPHA );
}

} // namespace stagehand
