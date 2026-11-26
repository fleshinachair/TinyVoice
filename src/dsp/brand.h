/* Copyright (c) 2026 Mathew Taylor — Licensed under GPL-3.0-only */
/*
 * The product name and ID the engine is compiled under. A host that builds the engine into a product of its own overrides
 * them on the compiler's command line:
 *
 *     -DVT_BRAND_NAME='"Other Name"' -DVT_BRAND_ID='"otherid"'
 *
 * They are pasted into C string literals, printf / scanf formats, a folder path and JSON, so keep them plain: no quotes,
 * backslashes, slashes or %. Both are also pasted into the generated JSON (module_meta.h), where a much longer one can push
 * a string past the compiler's length limit (the build then fails, -Woverlength-strings).
 */
#ifndef TINYVOICE_BRAND_H
#define TINYVOICE_BRAND_H

#ifndef VT_BRAND_NAME
#define VT_BRAND_NAME "TinyVoice"       /* shown to people: the "name" reply, the root page, Looper Save's file names */
#endif

#ifndef VT_BRAND_ID
#define VT_BRAND_ID "tinyvoice"         /* for machines: preset files' "module", the widget kinds */
#endif

#ifndef VT_LOOPSAVE_DEFAULT_DIR         /* where Looper Save writes until loopsave_set_dir says otherwise */
#define VT_LOOPSAVE_DEFAULT_DIR "/data/UserData/UserLibrary/Samples/" VT_BRAND_NAME
#endif

#endif
