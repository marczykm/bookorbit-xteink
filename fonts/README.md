# Fonts
Noto Serif (Regular, Bold, Italic, BoldItalic), copyright The Noto Project Authors, SIL Open Font License 1.1
(https://openfontlicense.org). Subset to Basic Latin, Latin-1, Latin Extended-A/B and common punctuation with
`pyftsubset` (hinting removed, `kern` and `liga` kept), which is a modification of the original fonts.

`tools/make_fontpack.py` packs them into `build/fontpack.bin`, which `tools/flash_fonts.sh` writes to the `fonts`
flash partition.
