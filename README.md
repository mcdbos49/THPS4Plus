# Tony Hawk's Pro Skater 4+

The definitive way to play THPS4!

![Logo](https://github.com/user-attachments/assets/d4093c1d-047a-48e6-9f27-20c536c4e051)


## Features (planned & included)

- Properly Scaled Widescreen (Hor+ UI and FOV)
- Proper Controller Support for Xinput, SDL, and Dinput, with matching controller glyphs based on the controller you use. (work in progress, Xbox and PS5 controllers mostly work)
- HD UI textures (see notes)
- Online Server Connection (planned)
- D3D8 fix support
- Dumping and loading textures.

This mod is *NOT* complete, I was just very eager to release it. Expect more to come in the future!

## Textures

THPS4+ comes with its own set of HD textures. You can dump and make your own by pressing the dump hotkey shown in the THPS4Plus.ini file. They will appear in `Game\THPS4Plus\dump\textures`. Other folders or files you see are for debugging purposes. When you're done making your HD texture, drop it into `Game\THPS4Plus\import\textures\custom`. Make sure it has the same name. The game will load it at the same size as the original, but in higher resolution. Note that most textures will export either upside-down or flipped. In most cases, it will also want to load flipped, so import it in the same orientation that it was exported.

## Comparison Screenshots

This is how the console versions of the game, as well as other widescreen mods of the game, handle widescreen. The HUD is stretched. This is from the GameCube version.

<img width="1920" height="1080" alt="c" src="https://github.com/user-attachments/assets/cbfb48cf-f552-43df-a43f-c102f2dc9531" />


On the other hand, this is how THPS4+ does it. Proper Hor+ widescreen support for both the 3D rendering and the HUD.

<img width="1920" height="1080" alt="d" src="https://github.com/user-attachments/assets/e53cebc2-b184-4354-9de6-3c22d9cd9aa1" />

But that's not all. THPS4+ comes bundled with its own HD UI textures. Have a look.

<img width="1920" height="1080" alt="f" src="https://github.com/user-attachments/assets/5082ce37-9f69-43c1-8ea8-3b5bebb12c67" />

The game also renders perfectly at any resolution. Here it is at `3840 x 1080`.

<img width="3840" height="1080" alt="p" src="https://github.com/user-attachments/assets/532ddf3a-3b45-46a7-8c2a-e13bcb5f0fdd" />


## Installation


Installation is super simple. Dowload the .zip file from the Releases page and extract it into your `THPS4/Game/` folder.  On Wine, you will need to add `D3D8.dll` as a .dll override.

## Notes

This covers some misc. stuff about the mod.

- If you use dgVoodoo, or another Direct3D 8 wrapper, you MUST delete it. This mod will overwrite it and may break your game. The mod comes bundled with DXVK, so it won't be necessary.
- A lot of the HUD textures were either taken from reTHAWed or AI upscaled. I'm a modder, not a graphic designer. If you are willing to make some higher quality textures, feel free.

## Building

Building is going to require MINGW32, part of MSYS2.

Open MINGW32. To ensure you've got the right thing, run `echo $MSYSTEM`. You should see `MINGW32` as the reply.

Fully update MINGW32 by running `pacman -Syu` and answering `Y` when prompted. You may have to close the window. If you do, re-open it and run `pacman -Syu` again. Keep doing this until there are no more updates.

Install the compiler by running `pacman -S --needed mingw-w64-i686-gcc` and answering `Y` when prompted.

Clone the repository to anywhere on your computer and cd into it in MINGW32.

Run `sh build.sh`. Ignore all warnings, they're just about what will happen if you put the game in an absurdly long file path.

Copy the `d3d8.dll`, `dxvk_d3d8.dll`, `d3d9.dll`, `SDL2.dll`, `THPS4Plus.ini`, and `THPS4Plus` folder into your game directory. 

Run `Skate4.exe` and enjoy!

## Roadmap

I plan to keep working on this mod, but so I'm leaving a list of things to work on in the future from most likely to least likely.

- Ports to THPS2, THPS3, THUG, THUG2, and maybe even THAW (THUG is already in the works!)
- A simpler, easier to read `THPS4Plus.ini` file 
- High Quality audio loading
- Fixes involving distance clipping
- 3D rendering optimization on wider screens

## Credits


Thanks to the DXVK team for integration of their software into this project.

Thanks to 10K Rising and THPSGoat for reTHAWed, the inspiration and source for a lot of the textures in this mod.  
