<img src="resources/icon.png" alt="Moltorino" width="128">

Moltorino
============

Moltorino is a fork of [Chatterino7](https://github.com/SevenTV/chatterino7), which is based on [Chatterino 2](https://github.com/Chatterino/chatterino2). It supports Twitch, YouTube, Kick and TikTok chat.

### Features of Moltorino

- YouTube and TikTok accounts, live chat and moderation.
- Poll and prediction menus for voting, betting and moderation.
- A theme editor with share codes and image or video backgrounds.
- A vanity menu for choosing paints, badges and badge order.
- GIF search and sending from chat.
- FFZ and BTTV emote modifiers, with controls for individual effects.
- Message translation and local chat recording.

### Screenshots from Chatterino7

![Example of Personal Emotes](https://user-images.githubusercontent.com/27637025/227032811-837c56eb-7724-431b-b00e-b944c9289dff.png)
![Example of Paints](https://user-images.githubusercontent.com/27637025/227034147-cb1fcd76-dbae-4878-9551-96ffa64dd1a9.png)

### Downloads

**Stable builds** can be downloaded from [moltorino.com](https://moltorino.com), or from the [releases section](https://github.com/EEVA-inc/Moltorino7/releases) once my free CI minutes reset

### Issues

If you have issues such as crashes or weird behaviour, report them in [GitHub issues](https://github.com/EEVA-inc/Moltorino7/issues).

### Discord

You can join the 7TV community using the link here: <https://discord.com/invite/7tv>.

### AVIF Support

When building Moltorino, you might not have access to a static build of `libavif`. In that case, you can define `CHATTERINO_NO_AVIF_PLUGIN` in CMake. If you have `qavif.so` from [kimageformats](https://invent.kde.org/frameworks/kimageformats) installed on your system, Chatterino will pick it up and use AVIF images.

## Original Chatterino 2 Readme

Chatterino 2 is a chat client for [Twitch.tv](https://twitch.tv).
The Chatterino 2 wiki can be found [here](https://wiki.chatterino.com).
Contribution guidelines can be found [here](https://wiki.chatterino.com/Contributing%20for%20Developers).

## Download

Current releases are available at [https://chatterino.com](https://chatterino.com).
Windows users can also install Chatterino [from Chocolatey](https://chocolatey.org/packages/chatterino).

## Nightly build

You can download the latest Chatterino 2 build over [here](https://github.com/Chatterino/chatterino2/releases/tag/nightly-build)

You might also need to install the [VC++ Redistributables](https://aka.ms/vs/17/release/vc_redist.x64.exe) from Microsoft if you do not have it installed already.  
If you still receive an error about `MSVCR120.dll missing`, then you should install the [VC++ 2013 Restributable](https://download.microsoft.com/download/2/E/6/2E61CFA4-993B-4DD4-91DA-3737CD5CD6E3/vcredist_x64.exe).

## Building

To get source code with required submodules run:

```shell
git clone --recurse-submodules https://github.com/EEVA-inc/Moltorino7.git
```

or

```shell
git clone https://github.com/EEVA-inc/Moltorino7.git
cd Moltorino7
git submodule update --init --recursive
```

- [Building on Windows](BUILDING_ON_WINDOWS.md)
- [Building on Windows with vcpkg](BUILDING_ON_WINDOWS_WITH_VCPKG.md)
- [Building on Linux](BUILDING_ON_LINUX.md)
- [Building on macOS](BUILDING_ON_MAC.md)
- [Building on FreeBSD](BUILDING_ON_FREEBSD.md)

## Git blame

This project has big commits in the history which touch most files while only doing stylistic changes. To improve the output of git-blame, consider setting:

```shell
git config blame.ignoreRevsFile .git-blame-ignore-revs
```

This will ignore all revisions mentioned in the [`.git-blame-ignore-revs`
file](./.git-blame-ignore-revs). GitHub does this by default.

## Code style

The code is formatted using [clang-format](https://clang.llvm.org/docs/ClangFormat.html). Our configuration is found in the [.clang-format](.clang-format) file in the repository root directory.

For more contribution guidelines, take a look at [the wiki](https://wiki.chatterino.com/Contributing%20for%20Developers/).

## Doxygen

Doxygen is used to generate project information daily and is available [here](https://doxygen.chatterino.com).
