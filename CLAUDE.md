# stagehand

**Host a frame-producing sidecar inside a plugin.** Loads a private copy of a
source library, pays out its clock against elapsed real time, and presents its
frames as one letterboxed quad.

C++17 + C11, CMake, OBJECT library. Depends on **OpenGL and `libdl` only** —
deliberately not on FFGL, so it is not tied to one host. **MIT**, public.

Extracted from resodoom, which is GPL-2.0. Read the licence note at the bottom
of `README.md` before moving code in either direction.

## Commands
- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- Fast dev build: add `-DCMAKE_OSX_ARCHITECTURES=arm64`
- Build: `cmake --build build`
- Test: `./build/stagetest`
- Everything: `tools/verify.sh`

As a dependency: `add_subdirectory` and link `stagehand::stagehand`. The tests
switch off automatically when it is not the top-level project.

## Notes
- **One loaded copy is one instance.** `dlopen` keys on path and returns the
  same image for a repeated path, so `Sidecar` stages a uniquely-named copy per
  instance. Removing that makes two hosts share one source's globals.
- **Time is granted, never read.** A source must not consult the wall clock.
  `Pump` converts elapsed time into ticks; a speed of 0 grants nothing, which
  parks the source rather than spinning it.
- **`Pump` releases elapsed time, not one tick per call.** A host may render
  the same logical frame to an output, a preview and a thumbnail; anything
  counting calls then runs at 2-3x speed only while the preview is open.
- **Opening can succeed and then fail.** Sources load their input on their own
  thread, so a host must poll `Failed()` every frame or the failure is silent.
- **`pixelAspect` is pixel WIDTH over HEIGHT.** Doom's 320x200 at 4:3 is 5/6,
  not 1.2. The reciprocal is just as natural to write and gives a picture that
  is wrong in a way that looks deliberate.
- **`ComputeFit` must reset both axes before falling through from Integer.**
  The Contain branch assigns only one axis by design, so the other would keep
  the oversized multiple and overflow the frame. Covered by a test.
- **Run the fit checks at two aspects.** A sign error in one branch is
  invisible whenever the picture happens to be the wider one.
- **`Presenter`'s destructor makes no GL calls** — it may run with no current
  context. Hosts call `Destroy()` where they were given one.
- No FBO is allocated anywhere, and no scoped-binding helpers: several of those
  clear a binding to zero on scope exit rather than restoring it.
- `flat`, `active`, `filter`, `input`, `output`, `sample`, `common`, `patch`
  are GLSL reserved words. Shader errors surface only at runtime.
- Public repo. "Commit" = commit **and** push.

## The test source
`tests/testsource` is a complete worked example of the ABI in ~300 lines of C,
and the whole suite runs against it — so stagehand needs nothing downloaded.
Its pattern is not decoration: four differing corner primaries catch a channel
swap and a flip at once, a bar whose column is the tick count catches a stale
or double-stepped frame, and the opaque alpha gives a host that passes an
undefined alpha through something to fail against.
