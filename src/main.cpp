#include <iostream>
#include <string>
#include <memory>

#include "FileList.hpp"
#include "SearchStrategy.hpp"
#include "SearchByName.hpp"
#include "SearchByType.hpp"
#include "SearchByDirectory.hpp"
#include "Indexer.hpp"

// QueryFS main: index once, query many.
//
// Polymorphic dispatch, and why there is no switch-case on strategy type:
// the menu collects the user's choices, constructs the matching subclass, and
// then hands it to the base pointer `strategy` — from there the virtual call
// `strategy->search(&index)` resolves to the right subclass at RUNTIME. The
// menu code never needs to know which subclass it is holding; add a fourth
// strategy someday and this main loop doesn't change.

// All input is read LINE-BY-LINE (one getline per prompt). Mixing >> and
// getline is a classic bug: >> leaves the '\n' behind and the next getline
// returns an empty string. Line-based input avoids that entirely, and it
// makes "press Enter to submit an empty answer" work naturally.

namespace {

// Reads one full line; returns false only on EOF.
bool readLine(const std::string& label, std::string& out) {
    std::cout << label;
    if (!std::getline(std::cin, out)) return false;
    return true;
}

// Reads a line and parses an int; falls back to `def` on junk input.
int readInt(const std::string& label, int def) {
    std::string line;
    if (!readLine(label, line)) return def;
    try {
        return std::stoi(line);
    } catch (const std::exception&) {
        return def;
    }
}

int menu() {
    std::cout << "\n================ QueryFS ================\n"
              << " 1. Search by Name\n"
              << " 2. Search by Type\n"
              << " 3. Search by Directory\n"
              << " 4. Exit\n"
              << "=========================================\n";
    return readInt("Choice: ", 4);   // EOF/junk -> exit cleanly
}

// ---------- the three search menus, each returning a fresh strategy --------

std::unique_ptr<SearchStrategy> runNameSearch() {
    std::string q;
    readLine("Name to search for: ", q);
    int exact = readInt("Exact match? (1=yes, 0=substring): ", 0);
    int ci = readInt("Case-insensitive? (1=yes, 0=no): ", 1);
    return std::make_unique<SearchByName>(q, exact == 1, ci == 1);
}

std::unique_ptr<SearchStrategy> runTypeSearch() {
    std::string q;
    readLine("Extension to filter by (press Enter for full group+count): ", q);
    return std::make_unique<SearchByType>(q);
}

std::unique_ptr<SearchStrategy> runDirectorySearch() {
    std::string q;
    readLine("Directory path to search under: ", q);
    int rec = readInt("Recursive (full descendants)? (1=yes, 0=immediate children only): ", 0);
    return std::make_unique<SearchByDirectory>(q, rec == 1);
}

} // namespace

int main() {
    std::string root;
    if (!readLine("Enter root path to index: ", root)) return 0;

    // The index is built ONCE here. Every search below reads this list from
    // memory — no re-scanning between queries, that is the whole point.
    FileList index;
    Indexer::buildIndex(root, index);

    if (index.getCount() == 0) {
        std::cout << "Nothing indexed. Exiting.\n";
        return 0;
    }

    bool running = true;
    while (running) {
        switch (menu()) {
            case 1: {
                auto strategy = runNameSearch();
                strategy->search(&index);   // virtual dispatch via base ptr
                break;
            }
            case 2: {
                auto strategy = runTypeSearch();
                strategy->search(&index);
                break;
            }
            case 3: {
                auto strategy = runDirectorySearch();
                strategy->search(&index);
                break;
            }
            case 4:
                running = false;
                break;
            default:
                std::cout << "Invalid choice.\n";
        }
    }

    std::cout << "Goodbye.\n";
    return 0;
}
