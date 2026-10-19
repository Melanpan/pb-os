# barryglide

Glide typing, the suggestion strip and autocorrect of Barry Launcher's
keyboards (`sm8550-overlay/usr/lib/barry_launcher/barry_glide.py`) as a plain
C++20 static library: no Qt, no dependencies. Not wired into Barry.Input yet.

| File | What |
|---|---|
| `glide.h/.cpp` | `Decoder` (glides), `Corrector` (correct, near, suggest, complete, known, keep), `Pack` and `Layout` (keyboard.json as shelld reads it) |
| `blob.h/.cpp` | The dictionary blob: builder, cache file, mmap |
| `unicode.*`, `unicode_tables.inc`, `gen_unicode.py` | Python's `isalpha`/`isupper`/`isspace`/`lower`/`upper`/`int()` digits, from tables generated with the device's Python |
| `json.*`, `text.h` | Small ordered JSON reader; Python's text-file line and `split`/`strip` rules |
| `tools/glide_cli.cpp` | The harness's CLI (shelld's `/glide`, `/suggest`, `/type` "correct" answers over JSONL) |
| `tools/glide_blob.cpp` | Builds a pack's blob ahead of time |
| `tests/` | The equivalence harness |

## Memory

A pack's words go into one read-only blob (layout in `blob.h`): words with
their letter keys and levels, the `(first, last)` letter table, the
lower-case lexicon sorted (completions bisect it), contractions, and the
delete index as sorted 64-bit hashes plus lexicon ids. A hash hit is checked
against the word, so collisions change nothing. `Blob::load` keeps it in
`$XDG_CACHE_HOME/barry_launcher/glide/<key>.blob` (key: a hash of words.txt,
English or not, the accent fold, the format version), writes it atomically
and maps it read-only: clean, file-backed pages the kernel drops under
pressure instead of swapping them. No cache dir: it stays in memory. Only
the header is read at load; each reference is bounds-checked where it is
read. The newest 8 blobs are kept.

The decoder's ideal paths sit in a bounded LRU (512 entries, ~0.6 MB),
emptied when the key layout changes, instead of the unbounded `_ideal` dict.

## Equivalence

`tests/run.sh <rootfs> <bin> <repo> <work> [pack.zip ...]` (in the colima VM,
as root for bwrap) runs `ref.py` (barry_glide.py with the rootfs's Python 3.12,
answering as shelld does) and `glide-cli` over `gen_cases.py`'s cases for the
shipped packs (en, en-thumbkey), each zip given (the Languages tab's es), and a
random fuzz pack (Unicode capitals, final sigma, dotted İ, ß, apostrophes,
repeats, odd levels, CRLF). `compare.py` counts exact matches and order
mismatches per pack and operation. Then memory and latency.

Built with `-ffp-contract=off`; distances use CPython's `vector_norm`, sums
use its Neumaier `sum()`, `x ** 2` goes through libm's `pow`.

Intentional deviations (none changed a result in the harness):

- Integral JSON coordinates are Python ints; CPython's `sum()` adds ints
  without compensation. barryglide treats every coordinate as a double, so a
  score can move by an ulp. Qt writes whole-pixel doubles as integers, so the
  harness sends about a quarter of the coordinates that way.
- Levels saturate at 32 bits (Python's are unbounded).
- Invalid UTF-8 or lone surrogates in a word: no answer (Python may answer).
  A personal-words file that is not UTF-8 is ignored (Python raises).
- A glide with no letter keys returns no words (Python raises ValueError,
  shelld answers 500). NaN and infinite coordinates are not handled.
- Unicode tables are Python 3.12.3's (Unicode 15.0). Regenerate them with
  `gen_unicode.py` when the rootfs's Python changes.
