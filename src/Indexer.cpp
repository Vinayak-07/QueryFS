#include "Indexer.hpp"
#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

// Builds the index: walks `rootPath` recursively and appends every discovered
// entry to the list. The FileNode constructor auto-fills all metadata from
// the path — the indexer's only job is discovery + hand-off.
//
// Exception handling, and why it sits HERE specifically:
// recursive_directory_iterator throws (std::filesystem::filesystem_error) the
// moment it hits a directory it cannot read — a permission-denied folder, a
// file deleted mid-scan, a broken mount. Without the catch, ONE locked
// directory kills the entire scan. With it, we skip the bad entry and keep
// going. The `error_code` overloads below never throw, but the iterator's
// increment itself can — so the try/catch is still required around the loop.
void Indexer::buildIndex(const std::string& rootPath, FileList& list) {
    std::error_code ec;
    fs::path root(rootPath);

    if (!fs::exists(root, ec) || ec) {
        std::cout << "Indexer: path does not exist: " << rootPath << "\n";
        return;
    }

    // Single-file root: there is nothing to recurse, just index the file.
    // .string() is explicit because fs::path only converts implicitly to
    // std::string on POSIX — on Windows its native type is std::wstring.
    if (fs::is_regular_file(root, ec) && !ec) {
        list.append(root.string());
        std::cout << "Indexer: indexed 1 entry (single file): " << rootPath
                  << "\n";
        return;
    }

    int skipped = 0;

    // skip_permission_denied: the iterator itself skips unreadable dirs
    // instead of throwing at the constructor/increment level. The per-entry
    // try/catch below covers everything else (mid-scan deletes, etc.).
    fs::recursive_directory_iterator it(
        root, fs::directory_options::skip_permission_denied, ec);
    if (ec) {
        std::cout << "Indexer: cannot open " << rootPath << ": "
                  << ec.message() << "\n";
        return;
    }

    for (auto end = fs::end(it); it != end; it.increment(ec)) {
        if (ec) {                    // increment failed: skip this subtree
            ++skipped;
            ec.clear();
            it.pop();
            continue;
        }
        try {
            const fs::directory_entry& entry = *it;

            std::error_code e2;
            if (!entry.exists(e2) || e2) { ++skipped; continue; }

            list.append(entry.path().string());
        } catch (const std::exception&) {
            ++skipped;               // unreadable entry: skip, never crash
        }
    }

    std::cout << "Indexer: indexed " << list.getCount() << " entr"
              << (list.getCount() == 1 ? "y" : "ies") << " under \""
              << rootPath << "\""
              << (skipped > 0 ? ("; skipped " + std::to_string(skipped)
                                + " unreadable entr"
                                   + (skipped == 1 ? "y" : "ies") + ".")
                              : ".")
              << "\n";
}
