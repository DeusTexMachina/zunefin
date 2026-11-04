# ZuneFin engineering notes

Findings from bringing up Jellyfin playback on a Zune HD (firmware 4.5) running
[Project Lyra](https://github.com/project-lyra-zune/project-lyra). Everything here
was confirmed on a real device unless marked otherwise.

## Platform

- The Zune HD runs Windows CE 6 on an NVIDIA Tegra APX 2600 (ARM11, ARMv6).
- XNA apps can't use the network. Native code needs an exploit; Lyra provides a
  persistent one plus a daemon (`nativeapp`) that answers on TCP 1337 over Wi-Fi.
- Lyra's RPC server handles **one client at a time**. An idle connection (for
  example a forgotten `nc`) blocks everyone else for up to 30 s.

## Toolchain: GCC instead of Visual Studio 2008

- We build with CeGCC 9.3 (`arm-mingw32ce`), which runs on Linux. No Windows needed.
- **Always build with `-march=armv5te -mno-unaligned-access`.** With
  `-march=armv6`, GCC emits unaligned `str`/`ldr` (for example when zero-filling
  `char buf[16] = "file"`). Windows CE has alignment faults enabled, so these
  instructions fault. Inside Lyra's kernel-privileged daemon that fault
  **reboots the Zune**. `tools/build.sh` rejects any binary with a misaligned
  immediate-offset access.
- Binaries only import `COREDLL.dll` and `WS2.dll`; no GCC runtime DLL is needed
  (`-static-libgcc`).

## Running code: plugin vs. separate process

- Lyra opcode 20 loads a DLL into `nativeapp` and calls
  `int Run(const void *arg, int arg_len, void *out, int out_max, int *out_used)`.
  `nativeapp` owns Winsock there (don't call `WSAStartup`). Its SEH guard doesn't
  catch faults in GCC code, so any crash takes the device down.
- Safer: `jflaunch.dll` is a tiny plugin that only calls `CreateProcessW`, and the
  real work runs in `jfplay.exe` as a normal user-mode process. A crash there
  only kills that process.

## Playback

All from `zdksystem.dll` (exports resolved with `GetProcAddressW`):

| Call | Result |
| --- | --- |
| `ZDKMedia_Queue_PlaySongFromURL(title, L"http://...mp3")` | Song is queued, then "this item is missing or cannot be played". The stock `http` DirectShow source (zwmtstreamer) doesn't handle a plain MP3 stream. |
| `ZDKMedia_Queue_PlaySongFromFile(title, path, duration_ms)` | Plays. Creates a **library-kind** song (id `0x01xxxxxx`) that ignores artist/album/art. The file must be complete when opened. |
| `ZDKMedia_Queue_PlaySongFromURL(title, L"\\flash2\\...\\x.mp3")` | **Plays, with full metadata.** Creates a **runtime-kind** song (id `0xfd000001`), which accepts the property setters below. This is what ZuneFin uses. |

- The player keeps the file open, so overwriting it fails with error 32
  (sharing violation). Use one cache file per song.
- Download speed from Jellyfin to the Zune was ~115–131 KB/s, about 5× the
  data rate of 192 kbps MP3. Streaming with a buffer is feasible.

## Now Playing metadata (runtime-kind songs)

Internal setters in `zdksystem` (XIP at a fixed address on firmware 4.5, found by
Lyra's YouTube mod):

- `0x4198474c` string props: `0x20002` artist, `0x20003` album
- `0x4198464c` int props: `0x10003` duration (ms), `0x10008` has-art

Get the song handle with `ZDKMedia_Queue_GetActiveSongIndex` +
`ZDKMedia_Queue_GetSongAtIndex`.

Album art: put a JPEG at
`\Flash2\Content\<id>>16 as %04x>\<(id>>8)&0xff as %02x>\art\<id&0xff as %02x>_art.jpg`
and set prop `0x10008` to 1. Copy the art into place right after starting playback.

## Jellyfin API

- `api_key` works as a query parameter on 10.10.
- Random song: `/Items?IncludeItemTypes=Audio&Recursive=true&SortBy=Random&Limit=1`
- MP3 for the Zune: `/Audio/{id}/stream.mp3?audioCodec=mp3&audioBitRate=192000&audioChannels=2&audioSampleRate=44100`
- Song artist: first of `"Artists":[...]`. `AlbumArtist` is often "Various Artists".
- Covers live on the **album**: `/Items/{AlbumId}/Images/Primary?maxWidth=300&maxHeight=300&format=Jpg`.
  Songs usually return 404 for their own image.
- Strings contain `\uXXXX` escapes (curly apostrophes and so on); decode them.
- Durations come as `RunTimeTicks` (100 ns). Parse with integers only.
