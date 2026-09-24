#include "SearchByType.hpp"
#include <algorithm>
#include <iostream>
#include <map>

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}

} // namespace

SearchByType::SearchByType(const std::string& extension)
    : oneExtension(lower(extension)) {}

void SearchByType::search(FileList* src) {
    if (src == nullptr) return;

    if (oneExtension.empty()) {
        // --- Group + count mode: tally every distinct extension. ---
        std::map<std::string, int> tally;
        src->traverse([&](const FileNode* node) {
            std::string ext = lower(node->getExtension());
            if (ext.empty()) ext = "(no extension)";
            ++tally[ext];
        });

        std::cout << "\n--- Search by Type: summary of " << src->getCount()
                  << " indexed files ---\n";
        if (tally.empty()) {
            std::cout << "  Index is empty.\n";
            return;
        }
        for (const auto& [ext, n] : tally) {
            std::cout << "  " << ext << " : " << n << "\n";
        }
    } else {
        // --- Filter mode: only files whose extension matches. ---
        int hits = 0;
        std::cout << "\n--- Search by Type: *." << oneExtension << " ---\n";
        src->traverse([&](const FileNode* node) {
            if (lower(node->getExtension()) == oneExtension) {
                std::cout << "  " << node->getFullPath()
                          << "  (" << node->getSize() << " bytes)\n";
                ++hits;
            }
        });
        std::cout << "  " << hits << " file(s) with extension \""
                  << oneExtension << "\".\n";
    }
}
