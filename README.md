nxkrnl (Roswell)
================

An open-source kernel for the original Xbox, based on [ReactOS](https://reactos.org/).

Where ReactOS is able to run Windows software, this kernel is able to run Xbox software.

Status
------
Boots some popular software for Xbox. Still a work in progress. Make a backup of your data before using this kernel.

How to build
------------

```sh
cmake -G Ninja -B build \
    -DCMAKE_TOOLCHAIN_FILE=$PWD/toolchain-gcc.cmake \
    -DCMAKE_BUILD_TYPE=Release
cmake --build build
# -> build/flash.bin
```

The default `flash` target assembles `boot/nxldr` (our minimal Xbox
loader) with the xz-compressed `xboxkrnl.exe` into a flash image.
No third-party bootloader is required.

For a checked build (DBG=1, kernel debug channels live), add `-DDBG=1`
to the configure step.

How to run
----------
Boot build/flash.bin with xemu (no bootrom required, use 128M). See tools/run-xemu.

License
-------
GPL 2.0 (see [COPYING](COPYING)).

Much of the code is individually licensed GPL-2.0-or-later (new files
and most ReactOS-derived code), alongside MIT/BSD/LGPL/CC0 pieces.
Some components are GPL-2.0 only — notably the TV-encoder drivers in
`hal/halx86/xbox/video/`, ported from the xbox-linux kernel — so the
kernel as a combined work is distributed under GPL 2.0.  Per-file
headers govern individual files.

Contributing
------------
Contributions are welcome. Important: see CONTRIBUTING.md.

Acknowledgements
----------------
* [ReactOS](https://reactos.org/): Foundation of this project.
* [Cromwell](https://github.com/xboxDev/cromwell): Initial boot environment and reference.
* [Cxbx-Reloaded](https://github.com/Cxbx-Reloaded/Cxbx-Reloaded): Reference for the Xbox software model and Xbox<>NT translation.
* [xdvdfs](https://github.com/antangelo/xdvdfs): XDVDFS (XISO) reference.
* [nxdk](https://github.com/xboxDev/nxdk): XBE creation and platform references.
* [xemu](https://xemu.app/): Emulation based testing, hardware reference.
* [XboxDevWiki](https://xboxdevwiki.net/): Other misc. platform info.
