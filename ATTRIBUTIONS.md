# Attributions

Vocoder is built on other people's work. This file lists what that work is, who
did it, and what it is doing here.

> **Provisional.** Across the fleet this file is generated from master lists in
> `stoatworks-backend` by `scripts/sync-attributions.py`. Vocoder is in that
> script's `names.json` but not in its component lists, so this copy is still
> hand-written; v0.1.0 shipped that way. Finishing the registration and re-running
> the sync is the fix — and note that the script's `--only` flag truncates the
> file rather than filtering it.

## Third-party code this project uses

Libraries, SDKs and frameworks the project is built on or bundles.

### Resolume FFGL SDK

<https://github.com/resolume/ffgl>
Licence: BSD-3-Clause
Copyright: FreeFrame

Vendored as a git submodule at `external/ffgl`, pinned to `b1afaf9`.

The plugin ABI itself. An FFGL effect is defined by this SDK's headers — there is
no other way to be loadable by Resolume Arena and Avenue.

### GLEW — the OpenGL Extension Wrangler Library

<https://github.com/nigels-com/glew>
Licence: BSD-3-Clause (with Mesa 3-D and Khronos components)
Copyright: Milan Ikits, Marcelo E. Magallon and Lev Povalahev

Arrives inside the FFGL submodule at `external/ffgl/deps/glew-2.1.0`. Not fetched
separately.

Resolves OpenGL entry points on Windows, where the system headers stop at OpenGL
1.1.

### libpng

<http://www.libpng.org/pub/png/libpng.html>
Licence: PNG Reference Library License (libpng)
Copyright: the PNG Reference Library authors

Arrives inside the FFGL submodule, under the SDK's CustomThumbnail sample.

Part of the upstream SDK tree rather than something this plugin calls directly —
listed because it is present in the checkout. The harness writes its PNGs with
the system zlib and fifty lines of its own.

## Methods

The Laplacian pyramid is Burt and Adelson's, from *The Laplacian Pyramid as a
Compact Image Code* (IEEE Transactions on Communications, 1983): the 5-tap
generating kernel, the reduce/expand pair and the exact-reconstruction
recursion are as described there. The channel vocoder it stands in for is
Homer Dudley's (1939). Both are published methods, implemented here from the
description; nothing was copied from anyone's source.

## Getting this wrong

If your work is here and the description is inaccurate, the licence is wrong, or
you would rather not be listed — open an issue and it will be fixed.
