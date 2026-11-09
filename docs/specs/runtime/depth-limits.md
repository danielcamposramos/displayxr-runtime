# Depth limits: how far a legacy application's picture sinks and pops

A legacy OpenXR application (one that does not enable `XR_DXR_display_info`, an OpenVR title through xrizer or
OpenComposite, a WebXR page) does not choose its own eye separation or screen plane: the runtime owns the camera
(`docs/architecture/extension-vs-legacy.md`, "The View Path"). How much separation suits depends on the display and on
the person. The runtime does not choose a number for it. It follows the **depth limits** of the desktop.

## Where the limits come from

A stereo compositor knows, for the output a window mainly sits on, how far behind and in front of the screen anything
may go. Stereo KWin announces it to a program that follows its window's desktop depth
(`kde_stereo_depth_v1.depth_limits`, since version 5 of `kde-stereo-content-v1`): *"sunk and popped are how far behind
and in front of the screen the compositor lets anything go on the output the surface mainly sits on, in pixels of a
view 1920 wide. They are the user's limits built on the output's display: a headset gets more separation, a framed 3D
screen less."* A program draws inside them, so its nearest thing and the desktop's popped menu agree on one screen.
The runtime's camera rig does the same.

The order of importance is the browser's and the desktop's: **the person's choice, built on the display.**

1. The display: the limits the compositor announced (`u_depth_limits_publish`, read with
   `u_depth_limits_announced`). Nothing here is a number of ours.
2. The person's choice on top (`u_depth_resolve`):
   - the class, `tv` or `headset` (the browser's flat modes): `tv` leaves the limits as announced, `headset` takes
     libstereo-policy's headset factor over a framed screen (2.5). Unset is the limits as announced.
   - the strength, a percent of the limits (100 is as announced, 0 is depth off);
   - the screen plane's distance (the browser's `dom.vr.flat.screen-plane-mm`; default 1 m);
   - an absolute sunk value and an absolute popped value in pixels of a view 1920 wide, which replace the limit they
     name (libstereo-policy's absolute override).

Where the person's choices live (the environment wins over the file, key by key):

| Choice | Environment | File in the configuration directory |
| --- | --- | --- |
| class | `DXR_VIEW_CLASS` (`tv`, `headset`) | `view-class`, first line |
| strength | `DXR_VIEW_DEPTH_STRENGTH` (0 to 1000) | `view-depth`: `depth-strength=` |
| screen plane | `DXR_VIEW_SCREEN_PLANE_MM` | `view-depth`: `screen-plane-mm=` |
| sunk | `DXR_VIEW_SUNK_PX` | `view-depth`: `sunk-px=` |
| popped | `DXR_VIEW_POPPED_PX` | `view-depth`: `popped-px=` |

The files are read again twice a second: a change reaches the next frame. A value that is wrong (not a number, out of
range, an unknown class or key) is **refused and said**: the runtime logs which source held what and that it is not
applied, never falls through to the next source, and never drops it silently.

## From the limits to the rig

With the sunk limit `s` (sunk pixels / 1920) and the popped limit `q` as fractions of the canvas width, the plane at
`D` metres, the disparity of a point `Z` in front of the camera, as a fraction of the canvas width and positive behind
the plane (the closed form of [legacy-camera-profile.md](legacy-camera-profile.md)), is

```
p(Z) = s (1 - D / Z)
ipdFactor = s D (2 tan(vFov/2) aspect) / (m2v eyeSeparation)         convergence = 1 / (D m2v) diopters
```

so a point at infinity sinks exactly `s`, the plane is at `D`, and the popped limit is met at `Z = D s / (s + q)`. The
eye separation is the display's reported one, divided out, so the limits hold on any display. A scene nearer than that
distance pops past the popped limit: the camera does what the geometry says and a finished picture is never shifted.
A person who wants a scene to reach nearer moves the plane (`D = Znear (s + q) / s`). The runtime logs one
`Depth limits:` line with the values in force, the nearest working distance included, whenever they change.

The runtime's comfort rule still holds (the zero-parallax plane no nearer than the viewer, `dxr_rig_clamp_for_comfort`);
when it binds, the line says so. The person's live keys (Shift+wheel for the separation, wheel for the plane) work on
top of the values in force, and Space returns to them.

## When there are no limits

An X11 window, which is what a hosted legacy session has today, receives no limits from Stereo KWin: they are an event
on a Wayland surface. With no announcement and no absolute sunk value, the runtime logs `Depth limits: none` with the
reason and keeps DisplayXR's own rig (ipdFactor 1, convergence 0.5 diopters). The class, strength and plane then have
nothing to scale. Nothing is chosen quietly.

Tests: `tests_depth_limits` (the closed form at every depth, the plane, the scale, the display's eye separation, the
person's choices on top, the refusals), `tests_view_class` (the names).
