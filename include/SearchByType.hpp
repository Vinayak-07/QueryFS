#ifndef SEARCHBYTYPE_HPP
#define SEARCHBYTYPE_HPP

#include "SearchStrategy.hpp"
#include <string>
#include <vector>
#include <utility>

// SearchByType: filters the index by file extension, then reports a count per
// extension (the "group + count" part of the spec). Two behaviours:
//  - oneExtension = "" (default) : group the ENTIRE index — every distinct
//    extension found is printed with its count. No query needed.
//  - oneExtension set            : list only the files whose extension matches
//    it, with a single total.
// Extensions are compared case-insensitively ("PDF" == "pdf") and stored
// without the leading dot.
class SearchByType : public SearchStrategy {
private:
    std::string oneExtension;  // "" = group/count all, otherwise filter to this

public:
    explicit SearchByType(const std::string& extension = "");

    void search(FileList* src) override;
};

#endif
