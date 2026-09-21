cmake_minimum_required( VERSION 3.20 )

project( stagehand VERSION 0.1.0 LANGUAGES C CXX )

set( CMAKE_C_STANDARD 11 )
set( CMAKE_C_STANDARD_REQUIRED ON )
set( CMAKE_CXX_STANDARD 17 )
set( CMAKE_CXX_STANDARD_REQUIRED ON )

# Off when stagehand is consumed as a subdirectory: a host does not want the
# test source and harness in its own build.
if( CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR )
	set( STAGEHAND_IS_TOP_LEVEL ON )
else()
	set( STAGEHAND_IS_TOP_LEVEL OFF )
endif()

option( STAGEHAND_BUILD_TESTS "Build the synthetic source and the test harness"
		${STAGEHAND_IS_TOP_LEVEL} )

if( APPLE AND NOT CMAKE_OSX_ARCHITECTURES )
	set( CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE )
endif()

# ---------------------------------------------------------------------------
# The library
#
# An OBJECT library rather than STATIC: consumers link it into a shared module,
# and a non-PIC archive cannot be linked into one at all on Linux.
# ---------------------------------------------------------------------------
add_library( stagehand OBJECT
	src/Sidecar.cpp
	src/Present.cpp
	src/Diag.cpp
)
add_library( stagehand::stagehand ALIAS stagehand )

target_include_directories( stagehand PUBLIC
	$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
set_target_properties( stagehand PROPERTIES POSITION_INDEPENDENT_CODE ON )

if( APPLE )
	target_link_libraries( stagehand PUBLIC "-framework OpenGL" ${CMAKE_DL_LIBS} )
else()
	find_package( OpenGL REQUIRED )
	find_package( GLEW REQUIRED )
	target_link_libraries( stagehand PUBLIC OpenGL::GL GLEW::GLEW ${CMAKE_DL_LIBS} )
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if( STAGEHAND_BUILD_TESTS )
	# A shared library, loaded by path exactly as a real source is -- so the
	# suite exercises the loader rather than a linked-in copy of the source.
	add_library( stagehand_testsource MODULE tests/testsource/testsource.c )
	target_include_directories( stagehand_testsource PRIVATE include )
	set_target_properties( stagehand_testsource PROPERTIES
		C_VISIBILITY_PRESET hidden
		PREFIX "lib"
	)
	if( APPLE )
		# .dylib rather than CMake's default .so for a MODULE, so it looks like
		# what a host would actually ship.
		set_target_properties( stagehand_testsource PROPERTIES SUFFIX ".dylib" )
	endif()

	add_executable( stagetest tests/stagetest/main.cpp )
	target_link_libraries( stagetest PRIVATE stagehand )
	add_dependencies( stagetest stagehand_testsource )
	target_compile_definitions( stagetest PRIVATE
		STAGEHAND_TESTSOURCE_PATH="$<TARGET_FILE:stagehand_testsource>"
	)

	enable_testing()
	add_test( NAME stagetest COMMAND stagetest )
endif()
