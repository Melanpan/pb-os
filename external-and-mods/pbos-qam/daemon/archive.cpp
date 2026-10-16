#include "archive.h"

#include <archive.h>
#include <archive_entry.h>
#include <dlfcn.h>

#include <mutex>

namespace pq {

namespace {

struct Lib {
    bool ok = false;
#define FN(name) decltype(&::name) name = nullptr;
    FN(archive_read_new)
    FN(archive_read_support_format_zip)
    FN(archive_read_support_format_tar)
    FN(archive_read_support_filter_all)
    FN(archive_read_open_filename)
    FN(archive_read_next_header)
    FN(archive_read_data_skip)
    FN(archive_read_extract2)
    FN(archive_read_free)
    FN(archive_error_string)
    FN(archive_format)
    FN(archive_write_disk_new)
    FN(archive_write_disk_set_options)
    FN(archive_write_free)
    FN(archive_entry_pathname)
    FN(archive_entry_set_pathname)
    FN(archive_entry_filetype)
    FN(archive_entry_size)
    FN(archive_entry_hardlink)
    FN(archive_entry_symlink)
    FN(archive_entry_set_perm)
#undef FN
};

Lib& lib() {
    static Lib l;
    static std::once_flag once;
    std::call_once(once, [] {
        void* h = dlopen("libarchive.so.13", RTLD_NOW | RTLD_LOCAL);
        if (!h) return;
        bool all = true;
#define LOAD(name)                                                      \
    l.name = reinterpret_cast<decltype(l.name)>(dlsym(h, #name));       \
    all = all && l.name;
        LOAD(archive_read_new)
        LOAD(archive_read_support_format_zip)
        LOAD(archive_read_support_format_tar)
        LOAD(archive_read_support_filter_all)
        LOAD(archive_read_open_filename)
        LOAD(archive_read_next_header)
        LOAD(archive_read_data_skip)
        LOAD(archive_read_extract2)
        LOAD(archive_read_free)
        LOAD(archive_error_string)
        LOAD(archive_format)
        LOAD(archive_write_disk_new)
        LOAD(archive_write_disk_set_options)
        LOAD(archive_write_free)
        LOAD(archive_entry_pathname)
        LOAD(archive_entry_set_pathname)
        LOAD(archive_entry_filetype)
        LOAD(archive_entry_size)
        LOAD(archive_entry_hardlink)
        LOAD(archive_entry_symlink)
        LOAD(archive_entry_set_perm)
#undef LOAD
        l.ok = all;
    });
    return l;
}

archive* open_reader(Lib& L, const std::string& path) {
    archive* a = L.archive_read_new();
    L.archive_read_support_format_zip(a);
    L.archive_read_support_format_tar(a);
    L.archive_read_support_filter_all(a);
    if (L.archive_read_open_filename(a, path.c_str(), 65536) != ARCHIVE_OK) {
        L.archive_read_free(a);
        return nullptr;
    }
    return a;
}

bool unsafe_name(const std::string& name) {
    if (!name.empty() && name[0] == '/') return true;
    size_t start = 0;
    while (start <= name.size()) {
        size_t slash = name.find('/', start);
        if (slash == std::string::npos) slash = name.size();
        if (name.compare(start, slash - start, "..") == 0 && slash - start == 2) return true;
        start = slash + 1;
    }
    return false;
}

}  // namespace

Unpack unpack_archive(const std::string& path, const std::string& dir, const std::string& noun, size_t max_files,
                      uint64_t max_bytes, std::string& why) {
    Lib& L = lib();
    if (!L.ok) {
        why = "libarchive is missing";
        return Unpack::Failed;
    }
    // First every entry, checked; nothing is written yet.
    archive* a = open_reader(L, path);
    if (!a) return Unpack::NotAnArchive;
    archive_entry* e = nullptr;
    size_t files = 0;
    uint64_t total = 0;
    bool zip = false;
    int r;
    while ((r = L.archive_read_next_header(a, &e)) == ARCHIVE_OK || r == ARCHIVE_WARN) {
        zip = (L.archive_format(a) & ARCHIVE_FORMAT_BASE_MASK) == ARCHIVE_FORMAT_ZIP;
        const char* name_c = L.archive_entry_pathname(e);
        std::string name = name_c ? name_c : "";
        if (unsafe_name(name)) {
            why = "unsafe path in the archive: " + name;
            L.archive_read_free(a);
            return Unpack::Refused;
        }
        auto type = L.archive_entry_filetype(e);
        bool link = type == AE_IFLNK || L.archive_entry_hardlink(e) != nullptr;
        if (zip ? link : (type != AE_IFREG && type != AE_IFDIR) || link) {
            why = zip ? "the archive has links in it" : "the archive has links or special files in it";
            L.archive_read_free(a);
            return Unpack::Refused;
        }
        auto size = L.archive_entry_size(e);
        total += size > 0 ? static_cast<uint64_t>(size) : 0;
        files += 1;
        if (total > max_bytes || files > max_files) {
            why = "the archive is too big for a " + noun;
            L.archive_read_free(a);
            return Unpack::Refused;
        }
        L.archive_read_data_skip(a);
    }
    if (r != ARCHIVE_EOF) {
        // libarchive found no zip or tar in it at all.
        bool none = files == 0;
        why = L.archive_error_string(a) ? L.archive_error_string(a) : "unreadable archive";
        L.archive_read_free(a);
        return none ? Unpack::NotAnArchive : Unpack::Failed;
    }
    L.archive_read_free(a);
    if (files == 0) {
        // An empty file reads as an empty archive; Python saw no archive.
        return Unpack::NotAnArchive;
    }

    // Then unpacked under dir, refusing anything that would leave it.
    a = open_reader(L, path);
    if (!a) {
        why = "the archive went away";
        return Unpack::Failed;
    }
    archive* disk = L.archive_write_disk_new();
    // The entry names were checked above; their targets are made absolute
    // (under dir) here, so only .. and links through links are refused.
    L.archive_write_disk_set_options(disk, ARCHIVE_EXTRACT_SECURE_NODOTDOT | ARCHIVE_EXTRACT_SECURE_SYMLINKS |
                                               ARCHIVE_EXTRACT_TIME);
    Unpack result = Unpack::Ok;
    while ((r = L.archive_read_next_header(a, &e)) == ARCHIVE_OK || r == ARCHIVE_WARN) {
        std::string target = dir + "/" + L.archive_entry_pathname(e);
        L.archive_entry_set_pathname(e, target.c_str());
        L.archive_entry_set_perm(e, L.archive_entry_filetype(e) == AE_IFDIR ? 0755 : 0644);
        if (L.archive_read_extract2(a, e, disk) < ARCHIVE_WARN) {
            why = L.archive_error_string(a) ? L.archive_error_string(a) : "cannot unpack";
            result = Unpack::Failed;
            break;
        }
    }
    if (result == Unpack::Ok && r != ARCHIVE_EOF) {
        why = L.archive_error_string(a) ? L.archive_error_string(a) : "cannot unpack";
        result = Unpack::Failed;
    }
    L.archive_write_free(disk);
    L.archive_read_free(a);
    return result;
}

}  // namespace pq
