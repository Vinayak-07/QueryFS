#include "SearchByDirectory.hpp"
#include <iostream>

SearchByDirectory::SearchByDirectory(const std::string& dirPath, bool recursive)
    : dirPath(dirPath), recursive(recursive) {}

void SearchByDirectory::search(FileList* src) {
    if (src == nullptr) return;

    std::cout << "\n--- Search by Directory: \"" << dirPath << "\" ("
              << (recursive ? "full descendants" : "immediate children only")
              << ") ---\n";

    // Normalize: strip trailing '/' so "dir/" and "dir" behave the same,
    // and the prefix math below stays consistent.
    std::string prefix = dirPath;
    while (prefix.size() > 1 && prefix.back() == '/') prefix.pop_back();
    if (prefix == ".") prefix = "";

    int hits = 0;

    src->traverse([&](const FileNode* node) {
        const std::string& fp = node->getFullPath();

        // Prefix match must land on a PATH BOUNDARY, not mid-name:
        // "/a/project" must NOT match "/a/project-link". So everything
        // after the prefix has to start with '/' (or be empty).
        if (fp.compare(0, prefix.size(), prefix) != 0) return; // wrong prefix
        std::string rest = fp.substr(prefix.size());  // e.g. "/src/main.cpp"
        if (!rest.empty() && rest[0] != '/') return;  // boundary violated

        if (rest.empty()) return;  // the target directory itself, not a child

        if (!recursive) {
            // Immediate children only: strip the leading '/' then require
            // no further '/' — i.e. the file sits directly inside the dir.
            std::string leaf = rest.substr(1);
            if (leaf.find('/') != std::string::npos) return;
        }

        std::cout << "  " << fp << "  (" << node->getSize() << " bytes)\n";
        ++hits;
    });

    std::cout << "  " << hits << " file(s) found.\n";
}
