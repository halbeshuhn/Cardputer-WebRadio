# Tools

Three things live here. None of them is needed to build or run the radio.

## Making a station logo

**[`logo565.html`](https://halbeshuhn.github.io/Cardputer-WebRadio/v3.4.0/tools/logo565.html)**
— open it in a browser, no install, nothing leaves your computer. It is the
whole job in one page: paste the stream URL, drop an image in, fit it into the
square, press the button. The file arrives in your downloads already named
`A1B2C3D4.565`; copy it to `/logos/` on the card.

[![The logo tool](../images/logo565.png)](https://halbeshuhn.github.io/Cardputer-WebRadio/v3.4.0/tools/logo565.html)

Both halves of the job are places to get it wrong, which is why the page does
them for you:

- **The name** is the FNV-1a-32 hash of the stream URL in `%08X`, the same one
  the device computes when it looks for the picture. It is hashed character by
  character, so `http://` instead of `https://`, one more trailing slash or
  different capitalisation give a different file — and a station that never
  finds its logo.
- **The bytes** are RGB565, 76 × 76, **little-endian**, 11,552 bytes exactly.
  `pushImage()` reads its buffer as `swap565_t` (`create_pc()` in
  `LGFXBase.hpp`, with `_swapBytes` left at `false`), and `drawRawLogo()`
  swaps every word after reading — so in the file the low byte comes first.
  Red is `00 F8`. Big-endian gives you red and blue the wrong way round.

Doing it by hand with ffmpeg works too; `Anleitung-PNG-to-565-Logo.txt` has the
command and the reasoning (in German).

The device itself never writes `.565` — logos it fetches stay PNG or JPEG.
Raw files are always hand-made, and they are the ones that always draw: a PNG
needs 47,000 bytes in one piece for the decoder, which an https station does
not leave lying around.

## Seeing the screen without the device

`screen_main.py` draws the radio's main screen into a PNG on your computer.
Change a coordinate in the sketch, run it, look — no compiling, no flashing,
no cable. A round trip takes seconds instead of a minute.

```
python3 tools/screen_main.py
python3 tools/screen_main.py --station "Radio Paradise" --volume 220 --battery 24
python3 tools/screen_main.py --vis spectrum --spec 8,11,6,9,4,7,5,3,6,2
python3 tools/screen_main.py --failed --vis off      # dead stream
```

`--help` lists the rest: `--title`, `--wifi`, `--kbit`, `--vu`, `--scale`,
`-o`. Python 3, no packages to install.

`screen_eq.py` draws the equalizer, with the band values you give it:

```
python3 tools/screen_eq.py --gains 4,2,0,-3,-6 --sel 2
```

`screen_menu.py` does the rest — the floating system menu drawn over the radio
screen, the country list of the online search, and the station list, each with
or without the search field:

```
python3 tools/screen_menu.py menu   -o menu.png
python3 tools/screen_menu.py online -o online.png --sprache DE
python3 tools/screen_menu.py online --suche tai        # search field in use
python3 tools/screen_menu.py sender --seite 1/12       # station list
python3 tools/screen_menu.py sender --suche ndr
```

One honest gap: the station list uses **efont** on the device, a binary font
that ships with M5GFX and that these tools cannot read. The Noto face stands in
for it — same size, same line height, different letter shapes. Chinese station
names therefore cannot be rendered here at all; for those you need a photograph.

Every screenshot in the README above was produced this way.

It is not a mock-up. The values come from the sketch itself:

- coordinates and sizes from its `#define` lines
- fixed labels from the `drawString()` calls that use them, and tables like
  `marksTxt[]` straight from the array
- named constants inside a function, such as the battery's `battW`, by name
- menu and list wording from `Lang.h`, matched by the string id in the comment
  behind each entry, so `--sprache DE` really is the German build
- the country names from `rbCountries[]`, the equalizer band labels from
  `eqFreqLabel[]`
- colours resolved the way the library resolves them, then pushed through
  RGB565 like the panel does

The fonts are not redrawn either: `NokiaFC.h` is parsed from this repository
and `Font0` comes out of M5GFX's own `glcdfont.h`, with the typesetting rules
taken from `GFXfont::drawChar` and `LGFXBase::draw_string`. Glyph widths and
baselines therefore match to the pixel.

What is *not* picked up: a colour or a bare number written directly into a
drawing call, e.g. `fillRect(..., TFT_RED)`. Those have no name to look up.
Give the value a `#define` or a `const` and it becomes live.

The drawing *order* is transcribed in `screen_main.py`. Move a line in the
sketch and the picture follows; add a whole new element and the script needs
the same element added.

`measure.py` prints which colour occupies which area of the rendered image —
for checking that two parts really are flush.

```
python3 tools/measure.py --band 0 16 --von 190
```

M5GFX is expected under `~/Documents/Arduino/libraries/M5GFX`; set `M5GFX_DIR`
to its `src` folder if yours lives elsewhere.

## Rebuilding the font

`make_nokia_font.py` regenerates `NokiaFC.h` from `nokiafc22.ttf`, and
`make_noto_font.py` does the same for `NotoSans.h`, the proportional face used
for the stream title. Only needed if you want different sizes. Both need
`freetype-py`; the header in each generated file explains the rest.
