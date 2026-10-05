#ifndef SEARCHSTRATEGY_HPP
#define SEARCHSTRATEGY_HPP

#include "FileList.hpp"
#include <string>

// SearchStrategy: the abstract interface every search mode implements.
// One method, one rule — "given this index, produce your results".
// Nothing else about a search is fixed here, which is what lets the three
// modes look nothing alike internally yet be interchangeable from the menu.
class SearchStrategy {
public:
    virtual void search(FileList* src) = 0;
    virtual ~SearchStrategy() = default;
};

#endif
