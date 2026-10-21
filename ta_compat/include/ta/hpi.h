#pragma once
// Reader and writer for TA's HAPI archives (.hpi, .ufo, .ccx, .gp3) and the
// layered virtual file system the game builds from them.
//
// Format (see docs/RE_MEMORY.md, "HPI archives"):
//   header (20 bytes): "HAPI", u32 version 0x00010000, u32 directory_size,
//                      u8 key (+3 pad), u32 root_node_offset
//   bytes [20, directory_size) hold the directory tree, XOR-obscured when key != 0.
//   node:  u32 entry_count, u32 entries_offset
//   entry (9 bytes): u32 name_offset, u32 data_offset, u8 flags (bit0 = directory)
//   file record (9 bytes): u32 data_offset, u32 size, u8 compression
//   compressed files: u32 chunk sizes for each 64 KiB block, then SQSH chunks.

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ta {

enum class HpiError {
    None,
    OpenFailed,
    BadHeader,
    BadTrailer,
    BadDirectory,
    ReadFailed,
    BadChunk,
    BadChunkChecksum,
    BadChunkSize,
    BadCompression,
};

const char* to_string(HpiError e);

// Decode one SQSH chunk into `out` (appended). Exposed for tests.
HpiError unpack_sqsh(std::vector<std::uint8_t>& chunk, std::vector<std::uint8_t>& out);

// Number of SQSH chunks decoded so far, per compression method (0-3). Diagnostics only.
const std::uint64_t* sqsh_method_counts();

// LZ77 variant with a 4 KiB window. Exposed for tests.
void decompress_lz77(const std::uint8_t* in, std::size_t in_size, std::vector<std::uint8_t>& out);

struct HpiEntry {
    std::string name;
    bool is_dir = false;
    // Set by Vfs::refresh_shadows when an earlier source (a loose file or an earlier
    // archive) provides the same file path; the finder skips such entries.
    bool shadowed = false;
    // Directory
    std::vector<HpiEntry> children;
    // File
    std::uint32_t data_offset = 0;
    std::uint32_t size = 0;
    std::uint8_t compression = 0;
};

class HpiArchive {
public:
    // `keep_open` mirrors the game's flag: archives mounted with it are never pruned
    // by a later scan (rev31.gp3 and *.ccx); the others are dropped when their file
    // can no longer be opened.
    static std::unique_ptr<HpiArchive> open(const std::filesystem::path& path, HpiError& err, bool keep_open = false);

    const std::filesystem::path& path() const { return path_; }
    const HpiEntry& root() const { return root_; }
    HpiEntry& root() { return root_; }
    bool keep_open() const { return keep_open_; }

    // Path lookup using the game's rules: components split on '\\' only (a '/'
    // is an ordinary name character), case-insensitive, entries searched from last
    // to first within a directory, and the first hit decides (a file met where a
    // directory is needed ends the search).
    const HpiEntry* find(std::string_view path) const;
    HpiEntry* find(std::string_view path);

    // The directory node a finder pattern's directory part names: the components
    // before the last '\\' (none: the root), looked up as find() does.
    const HpiEntry* find_dir_of_pattern(std::string_view pattern) const;

    HpiError read(const HpiEntry& file, std::vector<std::uint8_t>& out);

    // The entry's stored bytes, unobscured (compressed entries: the chunk size
    // table and the chunks). For copying entries between archives.
    HpiError read_stored(const HpiEntry& file, std::vector<std::uint8_t>& out);

private:
    HpiArchive() = default;
    bool read_raw(std::uint32_t offset, std::uint8_t* dst, std::size_t size);
    // Closes the file after a read unless the archive is kept open.
    struct CloseAfter {
        HpiArchive& a;
        explicit CloseAfter(HpiArchive& archive) : a(archive) {}
        ~CloseAfter() {
            if (!a.keep_open_ && a.file_.is_open()) a.file_.close();
        }
    };

    std::filesystem::path path_;
    std::ifstream file_;
    std::uint8_t key_ = 0;
    bool keep_open_ = false;
    HpiEntry root_;
};

// Writes HAPI archives (for tests and tools). Directories are created from the
// paths; a file path whose name already exists in its directory adds a second
// entry with that name (archives can hold such duplicates).
struct HpiWriteFile {
    std::string path;                 // "dir\\sub\\name"
    std::vector<std::uint8_t> data;   // the file's bytes (ignored when `stored` is set)
    std::uint8_t compression = 0;     // 0 stored, 1 LZ77, 2 zlib
    bool scramble_chunks = false;     // set the chunks' scrambled flag
    // Copy of an entry from another archive (HpiArchive::read_stored): written as is.
    bool copy_stored = false;
    std::vector<std::uint8_t> stored;
    std::uint32_t stored_size = 0;    // the entry's file size
    std::uint8_t stored_compression = 0;
};
std::vector<std::uint8_t> hpi_build(const std::vector<HpiWriteFile>& files, std::uint8_t key);

// The game's wildcard matcher (case-insensitive '*' and '?').
bool wildcard_match(std::string_view name, std::string_view pattern);

// One result of the game's finder (Vfs::find_files).
struct VfsFound {
    std::string name;
    std::uint32_t attrib = 0;   // disk: the find data's attributes (0x80 alone reads 0); archives: 0x11 directory, 1 file
    std::uint32_t size = 0;
    int source = -1;            // -1 disk, else the archive's mount index
    bool is_dir() const { return (attrib & 0x10) != 0; }
};

// The game's layered file system (docs/RE_MEMORY.md, "File system and archives"):
// loose files under the game directory first, then archives in mount order.
class Vfs {
public:
    // Sets the game directory (the original's working directory) and runs the
    // first scan, as the game does at start-up.
    void mount_game_dir(const std::filesystem::path& game_dir, std::vector<std::string>* log = nullptr);

    // One archive scan exactly as the game runs it (at start-up and again on several
    // menu actions): archives whose file vanished are dropped (not rev31.gp3 / *.ccx),
    // then rev31.GP3, *.CCX, *.UFO and *.HPI (at most 10 newly mounted per scan) are
    // mounted in finder order, then *.hpi in the root of every CD-ROM drive; finally
    // the shadow flags are recomputed. Already mounted archives keep their place.
    void scan(std::vector<std::string>* log = nullptr);

    // Mounts one archive unless one with the same full path (case-insensitive) is
    // mounted; returns true when newly mounted. Does not touch the shadow flags
    // (call refresh_shadows; scan() does).
    bool mount(const std::filesystem::path& archive, HpiError& err, bool keep_open = false);

    // Recomputes every archive entry's shadow flag from the current sources.
    void refresh_shadows();

    // The CD-ROM drive roots a scan searches ("D:\\" ...). By default (unset) the
    // system's CD-ROM drives on Windows, none elsewhere.
    void set_cd_drives(std::vector<std::string> roots) { cd_drives_ = std::move(roots); cd_drives_set_ = true; }

    const std::vector<std::unique_ptr<HpiArchive>>& archives() const { return archives_; }
    const std::filesystem::path& game_dir() const { return root_; }
    // The disk file a game path names inside the game directory (the original's working
    // directory: what fopen / DeleteFileA of the relative path reach).
    std::filesystem::path disk_file(std::string_view path) const;

    // Where a path resolves: a loose file (source -1, entry null), the first archive
    // in mount order whose lookup gives a file (source = mount index), or nothing
    // (source -2). Shadow flags play no part here.
    struct Located {
        int source = -2;
        const HpiEntry* entry = nullptr;
        explicit operator bool() const { return source != -2; }
    };
    Located locate(std::string_view path);

    // Read a file by game path (e.g. "units\\armcom.fbi"). Returns false if absent.
    bool read(std::string_view path, std::vector<std::uint8_t>& out, std::string* source = nullptr);

    // Size of the file the game would open, or 0 if absent (the game treats an
    // empty file the same as a missing one).
    std::uint32_t size_or_zero(std::string_view path);

    // True if the path resolves to a loose file on disk (not archive-backed).
    bool is_loose(std::string_view path);

    // The game's finder: `pattern` = "dir\\...\\name-pattern". With source -1 the disk
    // listing comes first (the operating system's own matching and order); when it
    // ends (or finds nothing) and `all` is set, each archive in mount order follows
    // with its entries in stored order, matched by wildcard_match ("*.*" = "*"),
    // skipping shadowed entries. With source >= 0 the search starts at that archive;
    // without `all` it ends after the first archive whose directory exists.
    std::vector<VfsFound> find_files(std::string_view pattern, int source = -1, bool all = true);

    // Names matching `pattern` (e.g. "units\\*.FBI") in the game's enumeration
    // order (find_files over all sources, "." and ".." left out); directories are
    // included, or only directories with `dirs_only`. A name can repeat (a
    // directory present in several sources, or duplicates inside one archive).
    std::vector<std::string> enumerate(std::string_view pattern, bool dirs_only = false);

    // Full paths ("dir\\sub\\name") of files under `dir` whose name matches
    // `file_pattern`, walked the way the game's recursive finder does: "dir\\*" over
    // the sources, each subdirectory recursed within the source it was found in only.
    std::vector<std::string> enumerate_tree(std::string_view dir, std::string_view file_pattern, int source = -1, bool all = true);

    // The game's localized path builder: when the language is set, "dir-<language>\\name"
    // (extension replaced by `ext` when given) is used if it opens; otherwise "dir\\name".
    std::string virtual_path(std::string_view dir, std::string_view name, std::string_view ext);
    std::string language = game_language();  // the program's language (below) when the Vfs is made

    // The program's language (the original's 0x51FB50, set once at start-up, 0x49E830): the
    // command line's bare word, else the registry value "language", else "english".
    static const std::string& game_language();
    static void set_game_language(const std::string& language);

private:
    class Finder;
    std::vector<VfsFound> list_disk(std::string_view pattern) const;
    void mount_glob(std::string_view pattern, bool keep_open, int limit, std::vector<std::string>* log);
    void mark_shadows(const std::string& dir, int source, bool all);
    std::vector<std::string> cd_drive_roots() const;
    void tree_walk(const std::string& dir, std::string_view file_pattern, int source, bool all, std::vector<std::string>& out);

    std::filesystem::path root_;
    std::vector<std::unique_ptr<HpiArchive>> archives_;
    std::vector<std::string> cd_drives_;
    bool cd_drives_set_ = false;
};

} // namespace ta
