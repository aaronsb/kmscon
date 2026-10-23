# Build ColrTest.ttf, a tiny monospaced text font that also carries colour
# tables, for tests/test_font_freetype.c. Run it in the dev image, which pins
# python-fonttools, from the repository root:
#
#   docker run --rm --user $(id -u):$(id -g) -v $PWD:/src -w /src kmscon-dev \
#       python3 tests/fonts/make-colr-test.py tests/fonts/ColrTest.ttf
#
# Its glyphs, every one 600 units wide on a 1000-unit em:
# - 'A' (U+0041), a plain outline: text, which must draw on the baseline as
#   any text glyph does, not fitted like an emoji;
# - U+1F600, a COLRv1 glyph: a red solid paint clipped to "top", the top half
#   of the em plus a stub at the bottom left, so a picture drawn upside down
#   or mirrored shows;
# - U+1F601, a COLRv0 glyph: "top" as one blue layer.
# The timestamps are fixed, so the file regenerates byte for byte.
# Written for this fork, under kmscon's MIT licence; the shapes follow the
# test font a reviewer of PR #72 built.
import sys
from fontTools.colorLib.builder import buildCOLR, buildCPAL
from fontTools.fontBuilder import FontBuilder
from fontTools.pens.ttGlyphPen import TTGlyphPen


def rects(*boxes):
    pen = TTGlyphPen(None)
    for x0, y0, x1, y1 in boxes:
        pen.moveTo((x0, y0))
        pen.lineTo((x0, y1))
        pen.lineTo((x1, y1))
        pen.lineTo((x1, y0))
        pen.closePath()
    return pen.glyph()


order = [".notdef", "A", "top", "e1", "e2"]
fb = FontBuilder(1000, isTTF=True)
fb.setupGlyphOrder(order)
fb.setupCharacterMap({0x41: "A", 0x1F600: "e1", 0x1F601: "e2"})
fb.setupGlyf({
    ".notdef": rects((50, 0, 550, 700)),
    "A": rects((100, 0, 500, 700)),
    "top": rects((0, 400, 600, 800), (0, -200, 150, 400)),
    "e1": rects((0, -200, 600, 800)),
    "e2": rects((0, -200, 600, 800)),
})
fb.setupHorizontalMetrics({name: (600, 0) for name in order})
fb.setupHorizontalHeader(ascent=800, descent=-200)
fb.setupNameTable({"familyName": "ColrTest", "styleName": "Regular"})
fb.setupOS2(sTypoAscender=800, sTypoDescender=-200, usWinAscent=800, usWinDescent=200)
fb.setupPost(isFixedPitch=1)
fb.font["CPAL"] = buildCPAL([[(1, 0, 0, 1), (0, 0, 1, 1)]])
fb.font["COLR"] = buildCOLR(
    {"e1": {"Format": 10, "Paint": {"Format": 2, "PaletteIndex": 0, "Alpha": 1.0},
            "Glyph": "top"},
     "e2": [("top", 1)]},
    version=None)
fb.font["head"].created = fb.font["head"].modified = 0
fb.font.recalcTimestamp = False
fb.save(sys.argv[1])
