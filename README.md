# sokaster

An AI co-host inside OBS Studio. The plugin watches a source you pick, listens to the audio
tracks you tick, and talks back through a browser overlay — no second application to launch.

**Status: early development.** The skeleton is in place, features are being built. No releases yet.

## What it does

- **Eyes** — periodically captures the selected source and sends the frame to the sokaster service
- **Ears** — takes audio from a dedicated OBS track, cuts it into speech segments with Silero VAD, and sends those
- **Control panel** — a dock inside the OBS window: order a co-host, review the queue, moderate viewer orders

Replies come back into a browser overlay — an ordinary Browser Source that the plugin can add
to your scene for you.

The plugin is a client for the [sokaster.ru](https://sokaster.ru) service and needs an account.

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
src/plugin-main.cpp   # module entry points, dock registration
src/dock/             # Qt dock — the control panel inside OBS
data/locale/          # en-US, ru-RU — every user-facing string
cmake/, build-aux/    # build helpers from the template
buildspec.json        # module name, version, pinned dependencies
```

Installs like any modern OBS plugin, into
`%ProgramData%\obs-studio\plugins\sokaster\` (`bin\64bit\sokaster.dll` and `data\locale\`).

## License

GPL v2.0 or later — see [LICENSE](LICENSE). The plugin links against libobs and is distributed
under the same terms.

## Use of AI tools

Parts of the code and documentation were written with the help of an AI assistant (Claude),
reviewed and edited by the author. Changes go through the usual review before landing.
