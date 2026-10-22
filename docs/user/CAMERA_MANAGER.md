# Camera Manager for the GT Sport PC Port

Camera Manager adds an in-game menu for adjusting the existing Cockpit, Chase,
Bumper and Roof views while playing. Changes apply live, so players can tune
their view without restarting the game or using a separate application.

## In-game controls

- **F4** opens or closes Camera Settings.
- Selecting a camera in the menu switches the game to that view. Switching views
  with the controller also updates the selected camera in the menu.

## Camera adjustments

All four views support custom FOV from **30 to 100 degrees**, or the original
game-provided FOV for that camera and vehicle. There is no fixed replacement
"default FOV": Restore Defaults returns to the native value.

Position values are offsets from the game's original camera, in metres.
Positive height moves up; negative height moves down. Positive distance moves
forward; negative distance moves backward. Zero restores the original position.

| Camera | Height offset | Distance offset |
| --- | --- | --- |
| Cockpit | -0.600 to +0.600 m | -0.600 to +0.600 m |
| Chase | -1.000 to +1.000 m | -3.000 to +3.000 m |
| Bumper | Live road-clearance minimum to +0.550 m | Not exposed |
| Roof | -0.350 to +0.350 m | -1.000 to +1.000 m |

Low external-camera positions use terrain-based ground-clearance protection.
Existing saved Roof presets remain compatible.

## Chase-camera orbit

- Right-stick orbit around the vehicle with 360-degree horizontal rotation.
- Sensitivity from **-15 to +15**, with **1** retaining the initial response.
- Motion smoothing from **0 to 1000 ms**.
- Separate **Invert X** and **Invert Y** options.
- Optional smooth auto-centering after a configurable **0 to 10 second** delay.
- Smooth return to the normal position rather than an instant snap.

## Global settings and vehicle presets

The **Global Settings** tab applies camera settings across vehicles.
The **Custom Car Presets** tab detects the current supported vehicle in a race
and allows a separate override for each camera. Cameras without an enabled
override continue to follow Global Settings.

**Save** stores presets in `user/camera_presets.json`. **Restore Defaults** restores
the selected camera's original settings in the selected scope. For a custom
preset, this restores native defaults even if Global Settings were customized.
**Use Global Settings** removes the per-car override for that camera.

## Interface

The menu is available in the main menu as well as during gameplay. Live camera
data and vehicle-specific presets require an active supported in-race camera.
