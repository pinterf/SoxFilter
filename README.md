# SoxFilter
SoxFilter 2.0 for AviSynth by pinterf (20231201)
Based on Sox audio library 14.4.2

As of 2023/12/01 the actual soxlib version used in the plugin (as seen in sox.h):
`#define SOX_LIB_VERSION_CODE   SOX_LIB_VERSION(14, 4, 2)`

## Description

SoxFilter is an AviSynth plugin.

The original SoX as an application reads and writes audio files in most popular 
formats and can optionally apply effects to them.

The Avisynth plugin provides a filter to use an effect or effect chain on AviSynth audio streams. 
AviSynth acts both as writer and reader of audio data, no external audio files are involved.

SoxFilter will convert the audio to 32 bit integer format, this is how libsox works internally.

SoxFilter's actual effects and their parameters must be provided the very same way 
as one would put for the sox application, but one must separate the effects into a string list.
SoxFilter has one or more string parameter, each would describe a single effect and its parameters.

```
    ColorBars()
    SoxFilter("lowpass 120", "vol -0.5", \
    "sinc -n 29 -b 100 7000", "vol -3dB", "reverb 30 20", "compand 1.0,0.6 -1.3,-0.1")
```

Pseudo effects, which govern the behaviour of files (like `newfile` or `restart`) are not supported.

Sox effects heavily require linear audio stream flow. Seeking is probably not supported.
In order to work properly, SoxFilter would need a working AviSynth audio cache.
Use this plugin with at least AviSynth+ 3.7.3 (or Classic Avisynth 2.6).
Sox effectt


There existed a previous SoxFilter 1.1 for Avisynth, but since its creation libsox 
internals changed 100%. This project was created from scratch. Also, some effects were removed, 
and others added in the past ~15 years. The parameters of some effects had been changed as well.
E.g. reverb parameters were modified in 2008; "filter" was removed, use "sinc" instead 
(with a different syntax).

See also <http://avisynth.nl/index.php/SoxFilter>

## Functions in plugin

* Filter: apply one or more effect on audio data of clip

  `SoxFilter(clip, effect_and_params [, effect_and_params2, effect_and_params3, ...]`

* showing the list of possible effect names

  `SoxFilter_ListEffects()`

  This function returns an LF (\n) separated string with all internal effect names.
  List is long, SubTitle requires literal "\n" for line break, so use like this:

```
    BlankClip(10000, 1920, 1080)
    SubTitle(ReplaceStr(SoxFilter_ListEffects(), e"\n", "\n"), lsp = 0, size = 10)
```

* function returning the usage of a given effect

  `SoxFilter_GetEffectUsage(effect_name)`

  Each Sox effect has a short "usage" string. Empty string is returned if not found or when
  there is no usage info.
  The returned string contains LF (\n) characters as line separator.
  In order to display it with SubTitle replace control character \n with literal "\n" for 
  line break:

```
    BlankClip(10000, 1920, 1080)
    SubTitle(ReplaceStr(SoxFilter_GetEffectUsage("compand"), e"\n","\n"), lsp=0)
```

* Get all effects and their usage

  `SoxFilter_GetAllEffects()`

  Function returns a 2D array, presently with size [63][2]: there is now 63 effects reported
  by soxlib. Each main element contains a two-element subarray, of which element[0] is the 
  name of the effect, element[1] is the usage string. Latter can be empty.

  Note that not all functions will work in Avisynth (such as functions altering the number of 
  audio samples), some may even crash.

  The text file `SoxFilter_usages` was generated with this script:

```
    BlankClip(10000, 1920, 1080)

    global aAllEffects = SoxFilter_GetAllEffects()
    global numOfEffects = ArraySize(aAllEffects)

    WriteFileIf("SoxFilter_usages.txt", "current_frame < numOfEffects", \
    """ "Effect #" + String(current_frame+1) +e"\n" + aAllEffects[current_frame][0] + e"\n" + aAllEffects[current_frame][1] + e"\n" """, append=false)

    ScriptClip( function [] () {
       index = current_frame % numOfEffects
       name = aAllEffects[index][0]
       usage = "Effect: " + name  + e"\n" + aAllEffects[index][1]
       usage = ReplaceStr(usage, e"\n","\n")
       SubTitle(usage, lsp=0, size=10)
       } , local = true)
```

## Licencing

SoX (the original library) source code is distributed under two main 
licenses. The two licenses are in the files LICENSE.GPL and LICENSE.LGPL.

sox.c, and thus SoX-the user application (which this Avisynth filter does not 
use at all), is distributed under the GPL, while the files that make up libsox 
are licensed under the less restrictive LGPL.

Note that some of the external packages that can be linked into libsox
are GPLed and/or may have licensing problems, so they can be disabled
at configure time with the `relevant--with-*` options. If libsox is built
with such libraries, it must be distributed under the GPL.

## How to build:

1. Clone pinterf's SoxFilter repo <https://github.com/pinterf/SoxFilter.git>

2. Clone libsox repository from <https://git.code.sf.net/p/sox/code>

3. Copy the `src` folder content (c and h files and subfolders) into our SoxFilter repo
   into the `libsox` folder.
   
   The manually constructed custom `soxconfig.h` in `custom-files` folder does not need
   to be copied there, its folder is set in Visual Studio project configuration (.vcxproj) 
   in Additional Include Directories setting.

4. Edit `wav.c`, because it is using a gcc extension syntax for empty initialization which
   would result in compile error under MSVC.

  1. Find the lines

```
    static const struct wave_format wave_formats[] = {
    { WAVE_FORMAT_UNKNOWN,              "Unknown Wave Type" },
    ... etc ...
    { WAVE_FORMAT_OLIOPR,               "Olivetti OPR" },
    { }
    };
```

  2. Replace empty `{ }` with `{ NULL, NULL }`.

```
    static const struct wave_format wave_formats[] = {
    { WAVE_FORMAT_UNKNOWN,              "Unknown Wave Type" },
    ... etc ...
    { WAVE_FORMAT_OLIOPR,               "Olivetti OPR" },
    { NULL, NULL }
    };
```

5. Open Visual Studio solution

6. Build the two projects from Visual Studio's GUI: libsox, then SoxFilter

## Useful links

* SoxFilter repo: <https://github.com/pinterf/SoxFilter.git>
* Original SoX homepage: <https://sourceforge.net/projects/sox/>
* libsox git cloning source: <https://git.code.sf.net/p/sox/code>
* Documentation in human readable form: <https://manpages.ubuntu.com/manpages/lunar/en/man1/sox.1.html>
* Effects documentation: <https://manpages.ubuntu.com/manpages/lunar/en/man1/sox.1.html#effects>
* AviSynth community page: <http://avisynth.nl/index.php/SoxFilter>
* Doom9 forum of SoxFilter 1.1: <https://forum.doom9.org/showthread.php?t=104792>
