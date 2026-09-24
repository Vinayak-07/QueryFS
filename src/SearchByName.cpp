#include "SearchByName.hpp"
#include <algorithm>
#include <iostream>

namespace {

// Case-insensitive comparison helper: lowercases copies, then compares.
std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}

} // namespace

SearchByName::SearchByName(const std::string& query,
                           bool exactMatch,
                           bool caseInsensitive)
    : query(query), exactMatch(exactMatch), caseInsensitive(caseInsensitive) {}

void SearchByName::search(FileList* src) {
    if (src == nullptr) return;

    const std::string needle = caseInsensitive ? lower(query) : query;
    int hits = 0;

    std::cout << "\n--- Search by Name: \"" << query << "\" ("
              << (exactMatch ? "exact" : "substring") << ", "
              << (caseInsensitive ? "case-insensitive" : "case-sensitive")
              << ") ---\n";

    src->traverse([&](const FileNode* node) {
        const std::string& name = node->getName();
        const std::string hay = caseInsensitive ? lower(name) : name;

        bool match = exactMatch ? (hay == needle) : (hay.find(needle) != std::string::npos);
        if (match) {
            std::cout << "  " << node->getFullPath()
                      << "  (" << node->getSize() << " bytes)\n";
            ++hits;
        }
    });

    std::cout << "  " << hits << " match(es) found.\n";
}
