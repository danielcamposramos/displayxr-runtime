# View class: the kind of display a legacy application's views are made for

A legacy OpenXR application (one that does not enable `XR_DXR_display_info`, an OpenVR title through xrizer or
OpenComposite, a WebXR page) does not choose its own eye separation or screen plane: the runtime owns the camera
(`docs/architecture/extension-vs-legacy.md`, "The View Path"). The right separation depends on what shows the
pictures. A headset can take a larger separation than a framed 3D screen (a television, a projector, a monitor
with glasses) or a screen that shows the two views in colours (anaglyph), where the frame is part of the scene
and the pictures mix a little.

The **view class** is the person's choice of that kind of display. The runtime never derives it from the output:
one desktop can drive a 3D television and a viewer at the same time, and several outputs can show one window.

| Class | Display | Separation | Zero-parallax plane |
| --- | --- | --- | --- |
| `screen` (the default) | a framed 3D screen, or a screen in colours | `ipdFactor` 0.43 | 0.89 diopters (1.12 m) |
| `headset` | a headset, or a phone in a viewer | `ipdFactor` 1.075, 2.5 times the screen's | 0.89 diopters (1.12 m) |

The 2.5 is `libstereo-policy`'s factor for a headset, the depth source of the desktop and the browser. The other
numbers follow from two measured sides of the screen class, with the display's reported eye separation of 60 mm
(`sim_display`), `tan(vFov/2)` 0.3249 and a 16:9 canvas, by the closed form in
[legacy-camera-profile.md](legacy-camera-profile.md) (a fraction of the canvas width, positive behind the plane):

```
p(Z)/W = ipd * m2v * (convergence - 1/Z) * eyeSeparation / (2 * tan(vFov/2) * aspect)
```

| Point | `screen` | `headset` |
| --- | --- | --- |
| at infinity | 2.0 % behind the plane | 5.0 % behind |
| at 2 m | 0.9 % behind | 2.2 % behind |
| at 0.8 m | 0.8 % in front | 2.0 % in front |

Infinity at 2.0 % is the edition's browser on a 3D screen. The pop at 0.8 m is 0.4 of the sunk side, the ratio
of `libstereo-policy`'s popped limit (0.079 degrees) to its sunk limit (0.198 degrees). 0.8 m as the nearest
working distance is a proposal.

## Where the class comes from

In this order, the first that is set:

1. `DXR_VIEW_CLASS` in the process environment.
2. The first line of the file `view-class` in the runtime's configuration directory (the directory of
   `u_file_get_config_dir`, where `app-profiles/` lives). POSIX only; on Windows the environment alone until the
   machine-wide configuration path is decided.
3. The display's default, `screen`.

A value that is not `screen` or `headset` is **refused and said**: the runtime logs an error that names the source,
the value and the accepted names, and applies the display's default; it does not fall through to the next source.

## What it sets

The class is the default of the legacy camera profile ([legacy-camera-profile.md](legacy-camera-profile.md)): when
no `DXR_LEGACY_CAMERA_RIG` override and no `app-profiles/<executable>.json` exists, the selected class's
`ipdFactor` and `convergenceDiopters` seed the qwerty camera rig, once per instance, and one `View class:` line is
logged. A per-application profile or the override still wins over it, as the person's more specific choice.
The live keyboard adjustments (Shift+wheel for the separation, wheel for the plane) and Space (back to the
resolved class) still work. `xrLocateViews` then returns the eye poses (half the separation to each side) and the
asymmetric fields of view for that class.

Tests: `tests_view_class` (the names, the order, the refusal, the numbers above from the closed form).
