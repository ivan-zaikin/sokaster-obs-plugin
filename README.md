# sokaster

An AI co-host inside OBS Studio. The plugin watches a source you pick, listens to the audio
tracks you tick, and talks back through a browser overlay — no second application to launch.

**Status: early access (0.1.x).** Works end to end on Windows; expect rough edges and frequent updates.

## Installation

1. Download `sokaster-<version>-windows-x64-installer.exe` from [Releases](https://github.com/ivan-zaikin/sokaster-obs-plugin/releases).
2. Close OBS and run the installer. It checks the OBS version and puts the plugin where OBS looks for it.
3. Start OBS, open **Docks → sokaster** and sign in.

To uninstall, use **Settings → Apps** in Windows ("sokaster for OBS Studio").

Prefer no installer? The same release has `sokaster-<version>-windows-x64.zip`: close OBS and unzip it
into `C:\ProgramData\obs-studio\plugins\`, so that you get `C:\ProgramData\obs-studio\plugins\sokaster\`.

## What it does

- **Eyes** — while you stream, captures the source you picked every few seconds and sends the frame to the sokaster service
- **Ears** — mixes the audio sources you tick onto a spare OBS track, cuts it into speech segments with Silero VAD, and sends those
- **Status panel** — a dock inside the OBS window that says whether the co-host is ready and, if not, what to fix

Replies come back into a browser overlay — an ordinary Browser Source that the plugin can add
to your scene for you. Orders, moderation, voice and everything else live in the browser dashboard.

The plugin is a client for the [sokaster.ru](https://sokaster.ru) service and needs an account.

## Privacy

Nothing leaves your computer until you sign in, and nothing is captured unless OBS is streaming.
Then the plugin sends to `https://sokaster.ru` (the only server it talks to, unless `backend_url` in the settings file says otherwise):

- JPEG frames of the one source you picked in the dock (never the program output), at most 1280×720
- WAV segments of speech heard on the plugin's own audio track — only from the sources you ticked;
  silence is never sent

Sign-in opens the sokaster site in your browser; the plugin stores the resulting access key in
its settings file in the OBS config folder. Signing out deletes it.

To listen on its own track, the plugin picks an OBS audio track that no stream or recording
output uses and unticks every source from it, then lets you tick the ones the co-host may hear.
It never touches tracks your outputs use.

## Requirements

- OBS Studio 31.1.1 or newer
- Windows x64 — the only platform at this stage

## Building

Based on [obs-plugintemplate](https://github.com/obsproject/obs-plugintemplate): CMake 3.28+,
dependencies are fetched according to `buildspec.json`.

```
cmake --preset windows-x64
cmake --build --preset windows-x64
```

## Layout

```
src/plugin-main.cpp   # module entry points, frontend events, dock registration
src/plugin-config.*   # settings file: server address, access key, chosen source and track
src/net/              # libcurl client for the sokaster API
src/dock/             # Qt dock, sign-in, server status polling, overlay detection
src/vision/           # source picker, GPU readback, JPEG upload loop
src/audio/            # audio track tap, Silero VAD, speech segment upload loop
data/locale/          # en-US, ru-RU — every user-facing string
data/silero_vad.onnx  # Silero VAD v5 model (MIT, see silero_vad.LICENSE)
cmake/sokaster/       # ours: ONNX Runtime, install layout, Windows installer (Inno Setup)
cmake/, build-aux/    # build helpers from the template
buildspec.json        # module name, version, pinned dependencies
```

Installs into `%ProgramData%\obs-studio\plugins\sokaster\`. The DLL sits there twice: next to
`data\` for OBS 33+ and in `bin\64bit\` for OBS 32 and older — each version loads only one of them
(see `cmake/sokaster/plugin-layout.cmake`).

## License

GPL v2.0 or later — see [LICENSE](LICENSE). The plugin links against libobs and is distributed
under the same terms.

## Use of AI tools

Parts of the code and documentation were written with the help of an AI assistant (Claude),
reviewed and edited by the author. Changes go through the usual review before landing.
