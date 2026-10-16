// Unpacking a theme's or a language pack's archive (.zip, .tar, .tar.gz,
// .tar.xz, .tar.bz2) as Python's zipfile/tarfile did for the Barry Launcher
// plugin: every entry checked first, then unpacked. libarchive is loaded
// only for this, so the daemon does not keep it mapped the rest of the time.
#pragma once

#include <cstdint>
#include <string>

namespace pq {

enum class Unpack { Ok, NotAnArchive, Refused, Failed };

// Into dir (which exists). Refused: an absolute path or "..", a link or a
// special file, more than max_files entries or max_bytes in all; why says
// which, in the plugin's words.
Unpack unpack_archive(const std::string& path, const std::string& dir, const std::string& noun, size_t max_files,
                      uint64_t max_bytes, std::string& why);

}  // namespace pq
