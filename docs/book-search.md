# Bookshelf search

[简体中文](book-search_zn.md)

Search matches filenames throughout `metalio/e-ink/books` and all its subfolders. English letters are case-insensitive; book contents are not searched. Normal browsing still shows the current folder. Confirming an empty keyword returns to that folder. Results retain pagination and cover/list views. The keyword limit is 80 characters.

## Input and candidates

Tap the bookshelf search field to open the keyboard at the bottom of the same page. A magnifying glass appears on the left; a circular clear button appears on the right when the field contains text, clearing both text and composition while keeping the keyboard open. It hides when the field becomes empty. Pinyin and English letters appear in the field as they are typed. Selecting a candidate replaces the complete pending segment with the chosen character, phrase or English word.

The candidate bar is a keyboard header that appears after the first pending-letter lookup completes, without a separate Pinyin title. Continued typing keeps the bar and keyboard layout in place, reusing candidate buttons and updating only changed text and paging state. Old candidates cannot be selected while a new lookup is pending. Candidates are separated by space rather than individual boxes. Each group fits complete words using the current font and available width; the next word moves as a whole to the next group when it cannot fit. Back/forward grouping preserves word boundaries. An unusually long single candidate wraps in full instead of being clipped. The bar collapses and releases its height when composition ends.

For example, `nihao` offers **你好**, and `zhongguorenmin` offers **中国人民**. Pinyin mode also includes matching English words. The former shift-key position cycles through **CN / abc / ABC**. English mode offers word-prefix completion without converting letters into Chinese. Space selects the first visible candidate during Chinese composition; in English mode it keeps the typed word and inserts a space. Confirmation commits the current Chinese candidate and searches; in English mode it searches the letters shown in the field.

Backspace removes pending letters first, then complete characters before the cursor. Holding backspace clears the pending segment, or clears the field if the press started without a pending segment. Moving the cursor keeps typed letters and ends that composition. Back or tapping outside the field and keyboard cancels editing and restores the previous keyword. The cursor stays steady without blinking.

The four-row key area uses 32.5% of screen height and fits the drawable width. Scaled side, top and bottom margins are retained. Keys reuse the system keyboard layout, borders and press feedback.

## SD dictionary

Extract the dictionary package onto the SD card so that the dictionary is at `metalio/e-ink/fonts_ime/zh-CN/pinyin-dictionary.bin`, with `pinyin-licenses` beside it. `fonts_ime` groups resources by language code; Chinese uses `zh-CN`, while keyboard layouts are still supplied by firmware. Extended characters continue to use `metalio/e-ink/fonts/extended-fonts.fontpack`; glyphs come from the system UI font and that extension. Missing, unreadable or invalid dictionary records produce a message while retaining typed letters for literal search.

Fonts provide glyphs rather than pronunciations, phrases or frequencies. The dictionary filters pypinyin mappings and phrase data through the character coverage of the supplied `misans_25_2.ef` and `extended-fonts.fontpack`, with wordfreq ordering. It contains 41853 mapped characters, 217016 Chinese phrases and phrase-boundary prefixes, and 60000 English words. It supports dictionary words and prefix candidates, without unrestricted sentence decoding, user learning or online prediction. Pronunciations and rankings are not individually hand-verified.

A bounded binary index stays on the SD card. Background lookups binary-search the current key and read at most 32 KiB of candidate data into PSRAM. Neither the full dictionary nor font files are loaded into RAM. New input invalidates old queries; existing storage transactions and cancellation handle standby, unmount and input closure.

`tools/ime/zh_CN/build_dictionary.py` accepts font coverage and source files and outputs the binary and license directory. Authoring-only dependencies are pypinyin 0.55.0 and msgpack 1.1.2, plus the SHA256-verified wordfreq 3.1.1 release; none are firmware dependencies. The accompanying `pinyin-licenses/sources.json` identifies the sources. Binary format version is 2; record counts, character counts and modifications are listed in `dictionary-report.json`. Font glyphs are not redistributed in the dictionary.

## Attribution and licensing

Mapping and phrase pronunciations come from pypinyin 0.55.0 and its upstream pinyin data (MIT). Frequency-derived ranking comes from wordfreq 3.1.1 (CC BY-SA 4.0), by Robyn Speer and its data contributors, including SUBTLEX authors; SUBTLEX is freely available. The derived dictionary expands character coverage and readings and adds phrases, phrase-boundary prefixes and English completions. It is distributed under CC BY-SA 4.0 with the mapping’s MIT notices retained. Complete notices are included in the package’s `pinyin-licenses`; preserve them when redistributing the dictionary. The data license does not relicense all firmware code.

## Code organization

Language-specific implementations use `ime/<language_code>`: `main/infra/ime/zh_CN` handles composition and dictionary lookup, `main/ui/common/widgets/ime/zh_CN` provides the Chinese input UI, and `tools/ime/zh_CN` contains the dictionary generator. The shared keyboard remains in the common widget layer. SD resources use `fonts_ime/zh-CN`.
