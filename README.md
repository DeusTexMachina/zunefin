# zunefin

A Jellyfin music client for Zune HD running firmware 4.5.

ZuneFin plays music from your Jellyfin server over Wi-Fi through the Zune HD's
own player, with title, artist, album and cover art on Now Playing. It runs on
top of [Project Lyra](https://github.com/project-lyra-zune/project-lyra) and is
built with GCC, so you don't need Windows or Visual Studio.

## Status

Early prototype, driven from a Mac's Terminal.

- [x] Talk to Jellyfin over Wi-Fi from the Zune
- [x] Play a song through the native player
- [x] Title, artist, album, length and cover on Now Playing
- [ ] Play a whole album, with the next songs downloading in the background
- [ ] Start playback before the whole song has downloaded
- [ ] A Jellyfin tile on the Start menu for browsing

## Layout

```
src/jfping.c      network test plugin (GET /System/Info/Public)
src/jflaunch.c    tiny Lyra plugin that starts a normal program
src/jfplay.c      the player: pick a song, download, play, set metadata
tools/build.sh    safe build flags + misaligned-access check
tools/build-all.sh
tools/setup-toolchain.sh   download CeGCC (Linux)
tools/zf          Mac helper: push files, play, read the log
docs/NOTES.md     everything learned so far
```

## Building (Linux)

```sh
tools/setup-toolchain.sh          # installs CeGCC to /opt/cegcc
tools/build-all.sh                # outputs to build/
```

## Running (Mac)

You need Lyra installed on the Zune (open `http://install.zune.moe` in the Zune
browser) and a clone of Lyra's tools:

```sh
git clone https://github.com/project-lyra-zune/project-lyra.git ~/project-lyra
export ZUNE=192.168.1.208 JF="192.168.1.156 8096" JFKEY=your-jellyfin-api-key
tools/zf push build/jflaunch.dll build/jfplay.exe
tools/zf play
tools/zf log
```

## Credits

Built on Project Lyra (MIT) by magicisinthehole and contributors, whose YouTube
mod documented the Zune's queue and Now Playing internals.
