# stagehand

> **AI-assisted project.** This codebase was created with [Claude](https://claude.com/claude-code)
> (Anthropic), directed and reviewed by a human author. The macOS build is
> verified by a self-test that runs against a synthetic source built from this
> repo — 32 checks, no external dependency. The Windows and Linux branches
> compile in principle and **have never been built or run**.

**Host a frame-producing sidecar inside a plugin.**

Some things worth putting on a video layer are not libraries you can link: a
game engine, an emulator, a software renderer. They run on their own clock,
they are made of file-scope globals, and if they fall over they take the host
with them. stagehand is the small amount of machinery that sits between one of
those and the plugin showing it.

It does three things:

- **Loads a private copy** of a source library, so two instances are two
  instances rather than two views of one.
- **Pays out the source's clock** against elapsed real time, so speed, pause
  and single-stepping are one mechanism and the source is reproducible.
- **Presents its frames** as one correctly-letterboxed quad, with the
  texture, filtering and state restoration already right.

Written for FFGL plugins in Resolume, but it depends on **OpenGL and `libdl`
and nothing else** — not even on FFGL. Anything that can give it a current GL
context can use it.

---

## The two ideas

**One loaded copy is one instance.** `dlopen` keys on path: opening a library
that is already loaded hands back the *same* image with a bumped refcount, not
a second set of globals. Most code worth hosting this way has no instance
handle to pass — that is precisely why it is a separate library rather than a
class — so two hosts sharing an image share one game, one buffer and one
player. `Sidecar` stages a uniquely-named copy per instance, which is the only
in-process answer there is.

**Time is granted, not read.** A source must never consult the wall clock. It
runs on the ticks it is given and stops when it runs out. Speed, pause,
stepping in a test and bit-exact determinism then all fall out of the one
mechanism rather than being four features that fight each other.

That second rule also sidesteps a trap every stateful generator hits: a host
may render the same logical frame more than once — to an output, a preview and
a thumbnail — and anything advancing once per call runs at two or three times
speed, but only while the preview happens to be open. `Sidecar::Pump` releases
*elapsed time*, so two calls a microsecond apart release a microsecond.

## Using it

```cpp
stagehand::Sidecar source;
source.Open( "/path/to/libmysource.dylib", { { "file", path } } );

// once per rendered frame
source.Pump( speed );
if( source.Failed() ) { /* the source gave up; SourceStatus() says why */ }
if( source.Frame( buffer.data(), buffer.size() ) )
    presenter.Upload( buffer.data() );

float sx, sy;
stagehand::ComputeFit( stagehand::Fit::Contain, w, h,
                       source.Info().width, source.Info().height,
                       source.Info().pixelAspect, sx, sy );
presenter.Draw( sx, sy );
```

Writing a source means exporting one symbol and filling in a vtable — see
[`include/stagehand/SourceAbi.h`](include/stagehand/SourceAbi.h), and
[`tests/testsource`](tests/testsource) for a complete worked example in about
300 lines of C.

## Building

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/stagetest
```

As a dependency, `add_subdirectory` it and link `stagehand::stagehand`; the
tests switch themselves off when it is not the top-level project.

## Verifying

`tools/verify.sh` builds and runs the suite. Everything is exercised against
the synthetic source in this repo, which is the point — stagehand is not tied
to any particular source, and a suite that needed a real one would quietly
disprove that.

The pattern that source draws is not decoration. Each element pins one
specific host mistake: four different corner primaries catch a BGRA/RGBA swap
and a vertical flip at once, a bar whose column *is* the tick count catches a
stale or double-stepped frame, and the alpha byte is written opaque so a host
that passes an undefined alpha through has something to fail against.

## Licence

**MIT.** See [LICENSE](LICENSE).

That is deliberate and worth a sentence, because the project stagehand was
extracted from — [resodoom](https://github.com/stoatworks-labs/resodoom), which
runs Doom as a Resolume layer — is **GPL-2.0**, since it ships an engine
descended from id Software's Doom source. Everything in *this* repo is written
from scratch, knows nothing about Doom, and is useful without it, so it is
under the licence the rest of the fleet uses.

MIT code may be linked into a GPL work, and that is exactly what resodoom does.
The combined binary it distributes is GPL-2.0; these files stay MIT and can be
reused anywhere.
