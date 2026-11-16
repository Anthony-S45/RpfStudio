# RpfStudio

A GTA V archive explorer and modding tool for Linux, in the spirit of OpenIV. Works with both GTA V Legacy and GTA V Enhanced. Qt 6 GUI, C++17 core, plus a command-line tool.

Not affiliated with Rockstar Games or the OpenIV team. No game data or encryption keys are included: keys are read from your own copy of the game.

## Features

**Archives (RPF7)**
- Browse the game folder and archives as one tree, including archives nested inside archives.
- Reads all archive encryption types: NG (most PC archives), AES, OPEN and none.
- Edit mode, like OpenIV: import, replace, rename and delete files and folders, create folders and new archives (top-level or nested). Drag and drop works in both directions.
- Changes are applied in place. New data is appended and the table of contents is rewritten, so editing a 2 GB archive does not copy 2 GB. **Defragment archive** rebuilds and compacts it.
- `mods` folder support: the first edit of an original game archive offers to copy it to `<game>/mods/...` and edit the copy instead.
- Export single files, folders or whole archives. Search for files across all archives (Ctrl+F).

**Viewers and editors**
- **Textures (.ytd)**: preview with zoom, mip levels and an alpha toggle. Export to DDS or PNG. Replace, add, rename and remove textures. Import DDS as-is, or PNG/JPG/BMP/TGA encoded to DXT1, DXT5 or A8R8G8B8 with mipmaps. Decodes DXT1/3/5, BC4, BC5, BC7, A8R8G8B8, X8R8G8B8, A8B8G8R8, A1R5G5B5, A8 and L8.
- **Models (.ydr, .ydd, .yft)**: OpenGL 3D preview with orbit, pan and zoom, wireframe, and embedded textures mapped through the shaders' diffuse samplers. Export to OBJ, MTL and PNG.
- **Text (.xml, .meta, .txt, .dat, .ide, .ipl, ...)**: editor with XML highlighting and a well-formedness check on save.
- **Binary XML (RBF)**: converted to readable XML (read-only).
- **String tables (.gxt2)**: table editor. Add entries by label name or hash; import/export as text.
- **Everything else**: hex viewer.

**Mod packages (.oiv)**
- Installs OpenIV packages (format 2.x) into the mods folder or the game folder. Supported commands:
  - `<add>` and `<delete>`
  - nested `<archive>`, including `createIfNotExist`
  - `<text>`: add, insert, replace and delete lines
  - `<xml>`: add, replace and remove, using an XPath subset
- Shows the package's icon, metadata and the full list of operations before installing. Has a dry-run mode.

## Building

Ubuntu/Debian:

```bash
sudo apt install cmake g++ qt6-base-dev libqt6opengl6-dev zlib1g-dev libssl-dev libgl-dev
```

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
```

```bash
cmake --build build -j$(nproc)
```

```bash
./build/rpfstudio
```

Fedora: `sudo dnf install cmake gcc-c++ qt6-qtbase-devel zlib-devel openssl-devel mesa-libGL-devel`.
Arch: `sudo pacman -S cmake qt6-base zlib openssl`.

Run the tests (no game files needed):

```bash
./build/rpfcore_tests
```

## Setting up keys

Open **Settings**, set the GTA V folder (or click **Detect**: Steam, Proton and Heroic locations are checked), then click **Find keys in GTA5.exe**.

- The AES key is found in GTA5.exe by searching for a known SHA-1 hash.
- Current GTA5.exe builds no longer contain the NG keys in plain form (this is why CodeWalker switched to its `magic.dat` file). If the search reports that NG keys are missing, the dialog asks for CodeWalker's `magic.dat` and decodes it with your AES key. It is in the CodeWalker repository under `CodeWalker.Core/Resources/magic.dat`.
- You can also drop the usual key files into the key folder (`~/.config/RpfStudio/keys`): `gtav_aes_key.dat`, `gtav_ng_key.dat`, `gtav_ng_decrypt_tables.dat` and `gtav_hash_lut.dat`.

From the command line:

```bash
rpfstudio-cli keys-find "/path/to/GTA V/GTA5.exe" /path/to/magic.dat
```

## Using modified archives in game

Like OpenIV, RpfStudio writes archives with an unencrypted table of contents ("OPEN"). The game only loads these through an ASI loader plus a plugin that accepts them and redirects to the `mods` folder (OpenIV.asi is the usual one). Under Proton/Wine, ASI loaders need the matching DLL override (for example `WINEDLLOVERRIDES="dinput8=n,b"`).

## Command-line tool

`rpfstudio-cli help` lists every command. Examples:

```bash
rpfstudio-cli list update.rpf x64/dlcpacks -r
```

```bash
rpfstudio-cli extract update.rpf common/data out/
```

```bash
rpfstudio-cli add mods/update/update.rpf common/data my.meta
```

```bash
rpfstudio-cli ytd-export vehicles.ytd textures/ --png
```

```bash
rpfstudio-cli ytd-replace vehicles.ytd new_livery.png livery_1
```

```bash
rpfstudio-cli model-obj adder.yft adder.obj
```

```bash
rpfstudio-cli oiv-install mymod.oiv --game "/path/to/GTA V" --dry-run
```

```bash
rpfstudio-cli pack myfolder/ dlc.rpf
```

In `pack`, sub-folders named `*.rpf` become nested archives.

## Limitations (compared with OpenIV)

- Tested against a real GTA V Enhanced install: every archive (including nested DLC archives) opens and every file extracts. Legacy-version files have not yet been tested against a real install.
- GTA V Enhanced (gen9) textures and models are view/export only. Saving changes to an Enhanced .ytd is not supported yet (archives themselves can be edited normally).
- Fragments (.yft) show the main body only; child parts such as wheels are not drawn.
- Not implemented:
  - audio (.awc) playback
  - script (.ysc) viewing and importing
  - map and meta files (.ymap, .ytyp, .ymt PSO/RSC meta) to XML
  - collision (.ybn) viewing
  - model import and editing (models are view and export only)
  - ASI manager
- The model viewer shows the highest available LOD. Textures from separate .ytd files are not applied automatically.
- Writing NG-encrypted archives is not supported. Edited archives are always OPEN, as with OpenIV.

## Credits

- Format and crypto knowledge: [CodeWalker](https://github.com/dexyfex/CodeWalker) by dexyfex, and RageLib by Neodymium (MIT). `src/core/KeyHashes.inc` holds only SHA-1 hashes used to locate keys.
- BC1–BC7 decoding: [bcdec](https://github.com/iOrange/bcdec) (MIT / Unlicense), in `third_party/bcdec`.
