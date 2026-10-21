# Third-party notices

## SDL 3

The playable client links SDL 3.4.18 statically (<https://github.com/libsdl-org/SDL>). SDL is
not included in this repository; it is fetched at build time.

```
Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

This software is provided 'as-is', without any express or implied
warranty.  In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not
   claim that you wrote the original software. If you use this software
   in a product, an acknowledgment in the product documentation would be
   appreciated but is not required.
2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.
3. This notice may not be removed or altered from any source distribution.
```

## Microsoft Visual C++ runtime

Windows release builds ship the Visual C++ runtime libraries (`MSVCP140.dll`,
`VCRUNTIME140.dll`, `VCRUNTIME140_1.dll`) next to the program, as Microsoft's Visual Studio
license allows for redistributable code. They remain subject to Microsoft's terms.

## Operating system components

Music decoding uses Windows Media Foundation, and window, sound and multimedia functions come
from Windows itself. These are part of the operating system and are not distributed with the
engine.
