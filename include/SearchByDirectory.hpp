#ifndef SEARCHBYDIRECTORY_HPP
#define SEARCHBYDIRECTORY_HPP

#include "SearchStrategy.hpp"
#include <string>

// SearchByDirectory: keeps files whose fullPath begins with a given directory
// path. Two scoping modes:
//  - recursive = false : immediate children only — the path prefix must match
//    AND the remainder must contain no further '/' (i.e. the file sits
//    directly inside the target directory).
//  - recursive = true  : full descendants — prefix match alone is enough.
// The target directory is compared case-insensitively on Windows semantics
// only if we ever port; on this build comparison is byte-exact (POSIX paths
// are case-sensitive), which the viva answer should state as a deliberate
// platform choice.
class SearchByDirectory : public SearchStrategy {
private:
    std::string dirPath;
    bool recursive;

public:
    explicit SearchByDirectory(const std::string& dirPath,
                               bool recursive = false);

    void search(FileList* src) override;
};

#endif
