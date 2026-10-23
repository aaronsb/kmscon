# Fonts pinned for the tier 2 pixel tests

The tier 2 test loads these files explicitly (through a private fontconfig
configuration containing only this directory), so a system font update cannot
move the goldens.

| File | Source | Licence |
|---|---|---|
| `DejaVuSansMono.ttf`, `DejaVuSansMono-Bold.ttf` | DejaVu fonts 2.37 (Arch `ttf-dejavu`) | Bitstream Vera licence, `LICENSE-DejaVu.txt` |
| `NotoSansMonoCJKJP-Subset.otf` | Noto Sans Mono CJK JP, Regular, subset to the characters in the wide-CJK screen | SIL OFL 1.1, `LICENSE-NotoSansCJK.txt` |
| `ColrTest.ttf` | Built by `make-colr-test.py` for `test_font_freetype`: a monospaced text 'A' beside a COLRv1 and a COLRv0 glyph | written for this fork, MIT |
| `NotoColorEmoji-Subset.ttf` | Noto Color Emoji 2.057 (Arch `noto-fonts-emoji 1:2.057-1`, pinned in the dev image), subset to the emoji screens' codepoints | SIL OFL 1.1, `LICENSE-NotoColorEmoji.txt` |

The CJK subset keeps the original name table. It was produced with fonttools
(`fontTools.subset`, layout features dropped, `post.isFixedPitch` set) from
`NotoSansCJK-Regular.ttc`, font index 5, for: every non-ASCII character in
`tests/test_pixels.c` above U+2FFF (CJK, kana, hangul), all of hiragana and
katakana (U+3040-30FF), fullwidth ASCII (U+FF01-FF5E) and U+3000. DejaVu covers
the rest. Adding a character to a CJK screen means regenerating the subset and
every golden.

The emoji subset is CBDT colour bitmaps (one strike, 136x128 at 109 ppem) with
the font's substitutions kept, so the ZWJ family, the skin tone, the flag and
the keycap still shape to one glyph each. `subset-emoji.sh` regenerates it in
the dev image, which pins `noto-fonts-emoji` and `python-fonttools` for it;
the codepoints it keeps are listed there. Adding an emoji to a tier 2 screen
means adding it to that list, regenerating the subset and every golden that
draws an emoji.
