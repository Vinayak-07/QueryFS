#ifndef SEARCHBYNAME_HPP
#define SEARCHBYNAME_HPP

#include "SearchStrategy.hpp"
#include <string>

// SearchByName: finds files by filename.
//  - mode A: exact match (the whole name equals the query, case-insensitively)
//  - mode B: substring match (query appears anywhere in the name)
// A caseInsensitive flag governs both. Defaults are set in the ctor so the
// menu only has to know: construct, maybe set fields, call search().
class SearchByName : public SearchStrategy {
private:
    std::string query;
    bool exactMatch;       // true = whole-name equality, false = substring
    bool caseInsensitive;  // true = "REPORT" matches "report"

public:
    explicit SearchByName(const std::string& query,
                          bool exactMatch = false,
                          bool caseInsensitive = true);

    void search(FileList* src) override;  // prints matching files to stdout
};

#endif
