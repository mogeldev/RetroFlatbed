# Notices

RetroFlatbed is an independent project, not affiliated with or endorsed by Canon Inc. or the SANE project.
"Canon", "CanoScan" and "LiDE" are trademarks of Canon Inc., used only to identify supported hardware.

## SANE (plustek backend, sanei_lm983x)

`third_party/sane/` contains files derived from the SANE project
(<http://www.sane-project.org>, <https://gitlab.com/sane-project/backends>):

* `backend/plustek*.c`, `backend/plustek*.h` – Copyright (C) 2000-2007 Gerhard Jaeger and contributors
* `sanei/sanei_lm983x.c`, `include/sane/sanei_lm983x.h` – Copyright (C) Gerhard Jaeger and contributors
* `include/sane/sane.h` – SANE standard header
* `include/sane/config.h`, `sanei_debug.h`, `sanei_usb.h`, `sanei_access.h` – small replacements written for this port

They are licensed under the GNU General Public License v2 or later, with the SANE
exception permitting linking with other files (see the headers of the individual
files). Local modifications are marked with `_WIN32` conditionals or are limited to include paths.

Because the engine is GPL, the combined work (`lide20core`, `LiDE20.ds`, tools) is distributed
under **GPL-2.0-or-later**; the full text is in `LICENSE`.

## WIA

`wia/lide20wia.cpp` was written against the public WIA/STI interfaces documented by Microsoft
(`wiamindr.h`, `wiamdef.h`, `sti.h`, `stiusd.h` from the Windows SDK). Microsoft's
Windows-driver-samples (MS-PL) were read as API reference only; no sample code was copied,
so the work stays GPL-compatible.

## TWAIN Data Source Manager

`third_party/twain-dsm/win64/TWAINDSM.dll` is the unmodified, Authenticode-signed binary from the
official TWAIN DSM 2.5.1 release (<https://github.com/twain/twain-dsm/releases>), (C) TWAIN Working
Group, licensed under the **GNU LGPL v2.1** (`third_party/twain-dsm/license.txt`). It is a separate
shared library loaded at run time, not linked into this project; source is available at the
URL above.

## TWAIN

`src/twain/twain.h` is the unmodified header from the TWAIN Working Group
(<https://github.com/twain/twain-dsm>), distributed under its own permissive license.
