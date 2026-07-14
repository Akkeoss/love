LÖVE is an *awesome* framework you can use to make 2D games in Lua. It's free, open-source, and works on Windows, macOS, Linux, Android, and iOS.

[![Build Status: Windows](https://ci.appveyor.com/api/projects/status/chc0hdr08wv1d5c7?svg=true)](https://ci.appveyor.com/project/slime73/love)
[![Build Status: Github CI](https://github.com/love2d/love/workflows/continuous-integration/badge.svg)](https://github.com/love2d/love/actions?query=workflow%3Acontinuous-integration)

---

**This is a fork.** On top of upstream LÖVE 11.5, it builds a **libretro core**
(`love_libretro.so`), so `.love` games can run under RetroArch — and, the point of the
exercise, on Recalbox. Everything else here is upstream's.

The core is off by default: a normal LÖVE build is exactly what it was before. See
[Building the libretro core](#building-the-libretro-core) below.

---

Documentation
-------------

We use our [wiki][wiki] for documentation.
If you need further help, feel free to ask on our [forums][forums], our [Discord server][discord], or our [subreddit][subreddit].

Repository
----------

We use the 'main' branch for patch development of the current major release, and therefore it should not be considered stable.
There may also be a branch for the next major version in development, which is named after that version.

We tag all our releases (since we started using mercurial and git), and have binary downloads available for them.

Experimental changes are developed in a separate [love-experiments][love-experiments] repository.

Builds
------

Files for releases are in the [releases][releases] section on GitHub. [The site][site] has links to files and additional platform content for the latest release.

There are also unstable/nightly builds:

- Builds for some platforms are automatically created after each commit and are available through GitHub's CI interfaces.
- For ubuntu linux they are in [ppa:bartbes/love-unstable][unstableppa]
- For arch linux there's [love-git][aur] in the AUR.

Contributing
------------

The best places to contribute are through the issue tracker and the official Discord server or IRC channel.

For code contributions, pull requests and patches are welcome. Be sure to read the [source code style guide][codestyle].
Changes and new features typically get discussed in the issue tracker or on Discord or the forums before a pull request is made.

Compilation
-----------

### Windows
Follow the instructions at the [megasource][megasource] repository page.

### *nix
Run `platform/unix/automagic` from the repository root, then run ./configure and make.

	$ platform/unix/automagic
	$ ./configure
	$ make

When using a source release, automagic has already been run, and the first step can be skipped.

### macOS
Download or clone [this repository][dependencies-apple] and copy, move, or symlink the `macOS/Frameworks` subfolder into love's `platform/xcode/macosx` folder.

Then use the Xcode project found at `platform/xcode/love.xcodeproj` to build the `love-macosx` target.

### iOS
Building for iOS requires macOS and Xcode.

#### LÖVE 11.4 and newer
Download the `love-apple-dependencies` zip file corresponding to the LÖVE version being used from the [Releases page][dependencies-ios],
unzip it, and place the `iOS/libraries` subfolder into love's `platform/xcode/ios` folder.

Or, download or clone [this repository][dependencies-apple] and copy, move, or symlink the `iOS/libraries` subfolder into love's `platform/xcode/ios` folder.

Then use the Xcode project found at `platform/xcode/love.xcodeproj` to build the `love-ios` target.

See `readme-iOS.rtf` for more information.

#### LÖVE 11.3 and older
Download the `ios-libraries` zip file corresponding to the LÖVE version being used from the [Releases page][dependencies-ios],
unzip it, and place the `include` and `libraries` subfolders into love's `platform/xcode/ios` folder.

Then use the Xcode project found at `platform/xcode/love.xcodeproj` to build the `love-ios` target.

See `readme-iOS.rtf` for more information.

### Android
Visit the [Android build repository][android-repository] for build instructions.

### Building the libretro core

*This is the fork's addition, not part of upstream LÖVE.*

	$ cmake -B build -DCMAKE_BUILD_TYPE=Release -DLOVE_LIBRETRO=ON
	$ cmake --build build --target love_libretro -j$(nproc)
	# -> build/love_libretro.so

Drop the `.so` into a frontend's cores directory and give it a `.love` file. It runs
with no content too, in which case you get LÖVE's "nogame" screen.

The option defaults to `OFF`, and everything the port touches is behind
`#ifdef LOVE_ENABLE_LIBRETRO`, so a stock LÖVE build is unaffected.

**Dependencies differ from the normal build:** the core needs *neither SDL2 nor a GL
library*. The frontend owns the window, the input and the audio device, and glad
resolves every GL entry point at runtime through the address the frontend provides.
That is what lets one binary serve a desktop (OpenGL 3.3) and an ARM board
(OpenGL ES 3) — the core asks for each in turn and keeps the first the frontend
offers. Everything else (LuaJIT, OpenAL, FreeType, ModPlug, mpg123, Vorbis, Theora,
zlib) is as usual.

#### What works, and what does not

Video, audio, keyboard, mouse and gamepad all work; so do canvases, shaders and
mid-game resolution changes. Mr. Rescue -- a real, finished game -- plays from its
title screen through to game over.

Two things are worth knowing before you try a game:

- **Games must target LÖVE 11.x.** Anything written for 0.10 will fail on API changes
  that predate this port — official LÖVE 11.5 rejects those games too, and the core
  deliberately behaves the same rather than papering over it.
- **`love.run` is overridden.** Many games (nearly everything from the 0.10 era) ship
  their own `love.run` built around `while true do ... end`. That is fine in a normal
  LÖVE and fatal in a core: `retro_run()` would be entered once and never return, so
  the frontend freezes while the game runs happily inside it. Under libretro the
  frontend owns the main loop, so the core installs its own `love.run` over the
  game's.

Not implemented: save states (snapshotting a live Lua VM plus its GPU resources is a
research project, not an oversight), rumble, and `love.video`.

Dependencies
------------

- SDL2
- OpenGL 2.1+ / OpenGL ES 2+
- OpenAL
- Lua / LuaJIT / LLVM-lua
- FreeType
- ModPlug
- mpg123
- Vorbisfile
- Theora

[site]: https://love2d.org
[wiki]: https://love2d.org/wiki
[forums]: https://love2d.org/forums
[discord]: https://discord.gg/rhUets9
[subreddit]: https://www.reddit.com/r/love2d
[dependencies-apple]: https://github.com/love2d/love-apple-dependencies
[dependencies-ios]: https://github.com/love2d/love/releases
[megasource]: https://github.com/love2d/megasource
[unstableppa]: https://launchpad.net/~bartbes/+archive/love-unstable
[aur]: https://aur.archlinux.org/packages/love-git
[love-experiments]: https://github.com/slime73/love-experiments
[codestyle]: https://love2d.org/wiki/Code_Style
[android-repository]: https://github.com/love2d/love-android
[releases]: https://github.com/love2d/love/releases
