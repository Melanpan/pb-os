// barryglide: the few Unicode facts barry_glide.py takes from Python's str
// methods, from tables generated with the device's Python (gen_unicode.py),
// so words are matched, lowered and capitalized exactly as there.
#pragma once

#include <string>
#include <string_view>

namespace barryglide::uni {

// Strict UTF-8 (as Python's decoder: no overlongs, surrogates or > U+10FFFF).
bool decode(std::string_view in, std::u32string &out);
std::u32string decode(std::string_view in);  // invalid input: empty
void append(std::string &out, char32_t c);
std::string encode(std::u32string_view s);

bool isAlpha(char32_t c);  // str.isalpha() of one character
bool isUpper(char32_t c);  // str.isupper() of one character
bool isSpace(char32_t c);  // str.isspace() of one character
int decimal(char32_t c);   // the digit int() reads, else -1

bool isAlpha(std::u32string_view s);  // str.isalpha(): nonempty, all letters
bool isAscii(std::u32string_view s);
std::u32string lower(std::u32string_view s);  // str.lower(), final sigma included
std::u32string upper(char32_t c);             // str.upper() of one character

}  // namespace barryglide::uni
