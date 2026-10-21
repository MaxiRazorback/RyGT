# Running RyGT on Linux (Steam Proton)

RyGT is a Windows build. On Linux it runs through Proton, Steam's version of Wine, or through Wine
itself. It needs **Proton 10 or newer** (or Wine 10 or newer): older versions lack Windows
functions that RyGT's libraries use, and the game does not start.

> **Status:** RyGT is checked against Proton 10 and 11: every program and library in a release
> loads there, and under Wine the launcher links the game's files with Linux symbolic links. The
> first test under Proton got through Set up, the menus and into a race. Please report what works
> and what doesn't.

## What you need

- A GPU with a Vulkan 1.3 driver: Mesa RADV (AMD), NVIDIA's driver, or Mesa ANV (Intel Arc).
- Steam with Proton 10.0 or newer (Proton Experimental and GE-Proton 10 or newer work too).
- The same files as on Windows: your own GT Sport dump (CUSA03220) with update 1.69, and the
  system modules and fonts from your own PS4 firmware.

## Setting up with Steam

1. Unzip the release into a folder in your home folder, for example `~/Games/RyGT`. Keep the
   game dump in your home folder or on a Steam library drive too, since Proton may not see other
   places. If both are on the same drive, Set up hard-links the game's files; otherwise it makes
   symbolic links. Both work.
2. In Steam, choose **Games > Add a Non-Steam Game to My Library > Browse**, pick `RyGT.exe`,
   then **Add Selected Programs**.
3. Right-click the new entry, open **Properties > Compatibility**, tick **Force the use of a
   specific Steam Play compatibility tool** and pick **Proton 10.0** or newer.
4. Start it from Steam. In the launcher, pick your game, update, system modules and fonts
   folders. Linux folders are under drive `Z:`, for example `Z:\home\you\Games\CUSA03220`.
   Press **Set up**, then **Play**.

The first boot builds shaders on the splash screen, as on Windows.

## Where things are

- Logs: the `logs` folder in the RyGT folder, as on Windows.
- Shaders: the `shaders` folder in the RyGT folder. Everything RyGT makes stays in its folder; only the
  game files are linked from where they are.

## Controllers and wheels

- Steam Input turns controllers into a virtual Xbox pad. For a DualSense or DualShock 4 with
  adaptive triggers, touchpad and motion, turn it off for this entry: **Properties > Controller >
  Disable Steam Input**.
- Racing wheels reach the game through Proton. Force feedback depends on the Linux driver for the
  wheel, for example `hid-fanatecff` for Fanatec or `universal-pidff` for Moza. This is untested.

## Upscaling

- AMD FSR works on any Vulkan driver.
- DLSS under Proton is untested. Add NVIDIA's DLSS files as on Windows, and set the entry's launch
  options to `PROTON_ENABLE_NVAPI=1 %command%`.

## Without Steam (Wine, Lutris, Bottles)

Use Wine 10 or newer with a 64-bit prefix and a Vulkan driver, and run `wine RyGT.exe` from the
RyGT folder. In Lutris or Bottles, add `RyGT.exe` with a Wine 10 or newer runner.

## If something goes wrong

- The game closes at once: look at `logs\stderr.log`. For Proton's own log, set the launch options
  to `PROTON_LOG=1 %command%`; Proton writes `steam-<number>.log` to your home folder.
- Errors that mention `msvcp140` or `vcruntime140`: install Microsoft's Visual C++ runtime into the
  prefix with protontricks: `protontricks <number> vcrun2022`.
- "Could not link ..." during Set up: move the RyGT folder and the game dump into your home folder.

On a Steam Deck the steps are the same, in Desktop Mode. Expect lower frame rates than on a
desktop PC: GT Sport is heavy on the CPU.

## What RyGT does differently under Wine

- The launcher links `app0` with Linux symbolic links (`ln -s`),
  because Wine before 11 cannot make Windows links.
- `GTSport.exe` keeps Wine's default program icon; the icon only matters to Windows' Explorer.
- libkernel converts local time with the time zone's rules for the year when Windows' `...Ex` time
  zone functions are missing, which gives the same results.
