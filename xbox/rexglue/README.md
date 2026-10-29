# ReXGlue for the Xbox

The Xbox build of the game needs a version of the ReXGlue SDK that runs as a
UWP app. Rather than keep a fork of the SDK, the changes are kept here as
patches against the v0.10.0 tag, and `.github/workflows/xbox-rexglue.yml`
builds the SDK from source with them applied.

- `patches/*.patch` - applied in name order with `git apply`. Uncommitted, so
  the SDK still calls itself 0.10.0 and the game's `find_package` accepts it.

The workflow then builds the game against that SDK, so a patch that breaks
the game is caught in the same run.

## Progress

1. Build the SDK from source with no patches, and the game against it. This
   proves the pipeline before anything is changed.
2. A UWP window and app loop in place of SDL3.
3. A CoreWindow swap chain in the D3D12 presenter.
4. Packaged library loading, logs and saves in LocalState.
5. The SDK's third-party libraries built for the app partition.
6. The game linked as an app and packaged with its data.
