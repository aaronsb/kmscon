# Fonts pinned for the tier 2 pixel tests

The tier 2 test loads these files explicitly (through a private fontconfig
configuration containing only this directory), so a system font update cannot
move the goldens.

| File | Source | Licence |
|---|---|---|
| `DejaVuSansMono.ttf`, `DejaVuSansMono-Bold.ttf` | DejaVu fonts 2.37 (Arch `ttf-dejavu`) | Bitstream Vera licence, `LICENSE-DejaVu.txt` |
| `NotoSansMonoCJKJP-Subset.otf` | Noto Sans Mono CJK JP, Regular, subset to the characters in the wide-CJK screen | SIL OFL 1.1, `LICENSE-NotoSansCJK.txt` |

The CJK subset keeps the original name table. It was produced with fonttools
(`fontTools.subset`, layout features dropped, `post.isFixedPitch` set) from
`NotoSansCJK-Regular.ttc`, font index 5, for: every non-ASCII character in
`tests/test_pixels.c` above U+2FFF (CJK, kana, hangul), all of hiragana and
katakana (U+3040-30FF), fullwidth ASCII (U+FF01-FF5E) and U+3000. DejaVu covers
the rest. Adding a character to a CJK screen means regenerating the subset and
every golden.
