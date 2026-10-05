#ifndef INDEXER_HPP
#define INDEXER_HPP

#include <string>
#include "FileList.hpp"

// Indexer: turns a directory tree into a FileList. Kept as a tiny namespace-
// style class — one public static entry point, no state of its own. All the
// interesting behaviour (auto-fill metadata, ownership) lives in FileNode /
// FileList; the indexer is pure discovery.
class Indexer {
public:
    static void buildIndex(const std::string& rootPath, FileList& list);
};

#endif
