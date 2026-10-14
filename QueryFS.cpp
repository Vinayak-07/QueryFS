// =====================================================================
//  QueryFS - Simple File Search Engine            (OOP CA-4 project)
// =====================================================================
//  What it does:
//    1. Scans ALL drives automatically (C:\, D:\, ... on Windows, "/" on Linux)
//    2. Keeps every file/folder in memory, so searching is instant
//    3. Three searches: normal (name), by extension, by folder
//    4. Saves the index to a text file (fstream) so the next start is instant
//
//  OOP concepts used (point these out in your viva):
//    - Encapsulation : FileNode / FileList keep data private, use getters
//    - Abstraction   : SearchStrategy is an abstract class (pure virtual)
//    - Inheritance   : NameSearch, ExtensionSearch, FolderSearch inherit it
//    - Polymorphism  : one SearchStrategy pointer, three different behaviours
//    - Exceptions    : custom IndexError + try/catch around file/disk work
//
//  Build:  g++ -std=c++17 -O2 QueryFS.cpp -o queryfs
//  Run:    queryfs              (scan all drives, or load saved index)
//          queryfs C:\Users     (optional: scan only one folder)
// =====================================================================

#include <algorithm>   // std::find
#include <chrono>      // timing the search
#include <cstdint>     // std::uintmax_t
#include <cstdlib>     // std::strtoull
#include <filesystem>  // walking folders
#include <fstream>     // ifstream / ofstream (cache file)
#include <iomanip>     // setw, setprecision (neat columns)
#include <iostream>    // cin / cout
#include <memory>      // unique_ptr
#include <sstream>     // istringstream / ostringstream
#include <stdexcept>   // runtime_error
#include <string>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>   // only used to list drives + fix the console
#endif

namespace fs = std::filesystem;

const std::string CACHE_FILE = "queryfs_index.txt";  // saved index
const std::size_t PAGE_SIZE  = 15;                   // results per screen
const std::size_t MAX_PATH_SHOWN = 70;               // keeps lines on screen

// ---------------------------------------------------------------------
//  Small helper functions
// ---------------------------------------------------------------------

// Lowercase A-Z only, so UTF-8 file names stay valid.
std::string toLower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}

// Remove spaces/tabs/newlines from both ends of a string.
std::string trim(const std::string& s) {
    const std::string spaces = " \t\r\n";
    std::size_t first = s.find_first_not_of(spaces);
    if (first == std::string::npos) return "";
    std::size_t last = s.find_last_not_of(spaces);
    return s.substr(first, last - first + 1);
}

// Turn a byte count into "1.5 MB" style text.
std::string formatSize(std::uintmax_t bytes) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double value = static_cast<double>(bytes);
    int u = 0;
    while (value >= 1024 && u < 4) { value /= 1024; ++u; }
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(u == 0 ? 0 : 1) << value << ' ' << units[u];
    return ss.str();
}

// Convert a filesystem path to a UTF-8 std::string (works on every OS).
std::string toUtf8(const fs::path& p) {
    auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

// ---------------------------------------------------------------------
//  Terminal handling
// ---------------------------------------------------------------------

// One-time console setup. Windows needs two small fixes:
//   - UTF-8 output so non-English file names display properly
//   - ANSI escape codes ON, so we can clear the screen without system("cls")
void setupTerminal() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetErrorMode(SEM_FAILCRITICALERRORS);   // no "insert disk" pop-ups on empty drives
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (GetConsoleMode(out, &mode))
        SetConsoleMode(out, mode | 0x0004);  // ENABLE_VIRTUAL_TERMINAL_PROCESSING
#endif
}

// Clear the screen and move the cursor to the top-left corner.
void clearScreen() {
    std::cout << "\033[2J\033[3J\033[H" << std::flush;
}

// Print a prompt and read one line. Returns false if input has ended (Ctrl+D / Ctrl+Z).
bool readLine(const std::string& prompt, std::string& line) {
    std::cout << prompt << std::flush;
    return static_cast<bool>(std::getline(std::cin, line));
}

// Wait for the user to press Enter.
void pause() {
    std::string dummy;
    readLine("\nPress Enter to continue...", dummy);
}

// Banner shown at the top of every screen.
void printHeader() {
    std::cout << "==============================================================\n"
              << "   QueryFS - File Search Engine\n"
              << "==============================================================\n";
}

// ---------------------------------------------------------------------
//  Custom exception (inherits from a standard exception)
// ---------------------------------------------------------------------
class IndexError : public std::runtime_error {
public:
    explicit IndexError(const std::string& message) : std::runtime_error(message) {}
};

// ---------------------------------------------------------------------
//  ENCAPSULATION: FileNode = one file or folder.
//  All data is private. The outside world can only use the getters.
// ---------------------------------------------------------------------
class FileNode {
private:
    std::string path;        // full path, e.g. C:\Users\me\notes.txt
    std::string nameLower;   // "notes.txt" in lowercase (makes searching fast)
    std::string ext;         // "txt" in lowercase (empty for folders)
    std::uintmax_t size;     // bytes (0 for folders)
    bool folder;             // true if this is a directory
    FileNode* next;          // next node in the linked list

public:
    // Constructor: works out the name and extension from the path by itself.
    FileNode(const std::string& fullPath, bool isFolder, std::uintmax_t bytes)
        : path(fullPath), size(bytes), folder(isFolder), next(nullptr) {
        std::size_t slash = path.find_last_of("/\\");
        std::string name = (slash == std::string::npos) ? path : path.substr(slash + 1);
        nameLower = toLower(name);

        if (!folder) {
            std::size_t dot = nameLower.find_last_of('.');
            if (dot != std::string::npos && dot != 0)   // ".gitignore" has no extension
                ext = nameLower.substr(dot + 1);
        }
    }

    // Getters (read-only access)
    const std::string& getPath() const { return path; }
    const std::string& getNameLower() const { return nameLower; }
    const std::string& getExt() const { return ext; }
    std::uintmax_t getSize() const { return size; }
    bool isFolder() const { return folder; }
    FileNode* getNext() const { return next; }

    // Setter (only the list uses this to link nodes together)
    void setNext(FileNode* n) { next = n; }
};

// ---------------------------------------------------------------------
//  FileList = singly linked list that OWNS all FileNodes.
//  The destructor frees every node, so there are no memory leaks.
// ---------------------------------------------------------------------
class FileList {
private:
    FileNode* head;
    FileNode* tail;          // keeping the tail makes add() O(1)
    std::size_t fileCount;
    std::size_t folderCount;

public:
    FileList() : head(nullptr), tail(nullptr), fileCount(0), folderCount(0) {}
    ~FileList() { clear(); }

    // A copy would delete the same nodes twice, so copying is forbidden.
    FileList(const FileList&) = delete;
    FileList& operator=(const FileList&) = delete;

    // Add one entry at the end of the list.
    void add(const std::string& path, bool isFolder, std::uintmax_t size) {
        FileNode* node = new FileNode(path, isFolder, size);
        if (head == nullptr) head = tail = node;
        else { tail->setNext(node); tail = node; }
        if (isFolder) ++folderCount; else ++fileCount;
    }

    // Delete every node and reset the list.
    void clear() {
        FileNode* current = head;
        while (current != nullptr) {
            FileNode* old = current;
            current = current->getNext();
            delete old;
        }
        head = tail = nullptr;
        fileCount = folderCount = 0;
    }

    FileNode* getHead() const { return head; }
    std::size_t getFileCount() const { return fileCount; }
    std::size_t getFolderCount() const { return folderCount; }
    std::size_t getTotal() const { return fileCount + folderCount; }
};

// ---------------------------------------------------------------------
//  ABSTRACTION: SearchStrategy is an abstract class.
//  It says WHAT a search must do (matches), not HOW. You cannot create
//  an object of it directly because it has pure virtual functions.
// ---------------------------------------------------------------------
class SearchStrategy {
public:
    virtual ~SearchStrategy() {}                            // virtual destructor
    virtual bool matches(const FileNode& node) const = 0;   // pure virtual
    virtual std::string describe() const = 0;               // pure virtual
};

// ---------------------------------------------------------------------
//  INHERITANCE: three child classes, each with its own matching rule.
// ---------------------------------------------------------------------

// 1) Normal search: the name of any file OR folder contains the text.
class NameSearch : public SearchStrategy {
private:
    std::string keyword;
public:
    explicit NameSearch(const std::string& text) : keyword(toLower(text)) {}
    bool matches(const FileNode& node) const override {
        return node.getNameLower().find(keyword) != std::string::npos;
    }
    std::string describe() const override { return "name contains \"" + keyword + "\""; }
};

// 2) Extension search: files whose extension is exactly the given one.
class ExtensionSearch : public SearchStrategy {
private:
    std::string wanted;
public:
    explicit ExtensionSearch(const std::string& text) : wanted(toLower(text)) {
        if (!wanted.empty() && wanted[0] == '.') wanted.erase(0, 1);  // accept ".pdf" too
    }
    bool matches(const FileNode& node) const override {
        return !node.isFolder() && node.getExt() == wanted;
    }
    std::string describe() const override { return "extension is \"." + wanted + "\""; }
};

// 3) Folder search: only folders whose name contains the text.
class FolderSearch : public SearchStrategy {
private:
    std::string keyword;
public:
    explicit FolderSearch(const std::string& text) : keyword(toLower(text)) {}
    bool matches(const FileNode& node) const override {
        return node.isFolder() && node.getNameLower().find(keyword) != std::string::npos;
    }
    std::string describe() const override { return "folder name contains \"" + keyword + "\""; }
};

// ---------------------------------------------------------------------
//  Scanner: walks the disk and fills the FileList.
//  Exceptions are caught per folder, so one locked folder never stops the scan.
// ---------------------------------------------------------------------
class Scanner {
private:
    FileList& list;          // the list we are filling
    std::size_t skipped;     // folders we could not open

    // Symlinks and Windows junctions can loop forever, so we never enter them.
    static bool isLink(const fs::directory_entry& entry) {
        if (entry.is_symlink()) return true;
#ifdef _WIN32
        DWORD attrs = GetFileAttributesW(entry.path().c_str());
        return attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_REPARSE_POINT);
#else
        return false;
#endif
    }

    // Find every drive on the machine.
    static std::vector<std::string> findDrives() {
        std::vector<std::string> drives;
#ifdef _WIN32
        DWORD mask = GetLogicalDrives();          // bit 0 = A:, bit 1 = B:, ...
        for (int i = 0; i < 26; ++i)
            if (mask & (1u << i))
                drives.push_back(std::string(1, char('A' + i)) + ":\\");
#else
        drives.push_back("/");                    // Linux/macOS have one root
#endif
        return drives;
    }

    // Recursively add everything inside one folder.
    void scanFolder(const fs::path& dir) {
#ifndef _WIN32
        std::string p = dir.string();             // fake system folders: skip them
        if (p == "/proc" || p == "/sys" || p == "/dev") return;
#endif
        try {
            for (const auto& entry :
                 fs::directory_iterator(dir, fs::directory_options::skip_permission_denied)) {
                try {
                    bool isFolder = entry.is_directory();
                    std::uintmax_t size = isFolder ? 0 : entry.file_size();
                    list.add(toUtf8(entry.path()), isFolder, size);

                    // live progress, updated on the same line
                    if (list.getTotal() % 5000 == 0)
                        std::cout << "\r  Scanning... " << list.getTotal() << " items found" << std::flush;

                    if (isFolder && !isLink(entry))
                        scanFolder(entry.path());
                } catch (const std::exception&) {
                    ++skipped;                    // this one entry is unreadable
                }
            }
        } catch (const fs::filesystem_error&) {
            ++skipped;                            // whole folder is unreadable
        }
    }

public:
    explicit Scanner(FileList& target) : list(target), skipped(0) {}

    // root empty  -> scan every drive.  root given -> scan just that folder.
    void scan(const std::string& root) {
        if (!root.empty()) {
            std::error_code ec;
            if (!fs::is_directory(fs::path(root), ec))
                throw IndexError("Folder not found: " + root);
            std::cout << "Scanning " << root << "\n";
            scanFolder(fs::path(root));
        } else {
            std::vector<std::string> drives = findDrives();
            if (drives.empty()) throw IndexError("No drives found.");
            for (const std::string& d : drives) {
                std::cout << "Scanning drive " << d << "\n";
                scanFolder(fs::path(d));
                std::cout << "\r  " << list.getTotal() << " items so far          \n";
            }
        }
    }

    std::size_t getSkipped() const { return skipped; }
};

// ---------------------------------------------------------------------
//  Cache file (fstream). One line per entry:   F<TAB>size<TAB>path
// ---------------------------------------------------------------------
void saveCache(const FileList& list) {
    std::ofstream out(CACHE_FILE);
    if (!out) throw IndexError("Could not write cache file: " + CACHE_FILE);

    for (const FileNode* n = list.getHead(); n != nullptr; n = n->getNext())
        out << (n->isFolder() ? 'D' : 'F') << '\t' << n->getSize() << '\t' << n->getPath() << '\n';

    if (!out) throw IndexError("Error while writing cache file (disk full?)");
}

// Returns how many entries were loaded. Throws IndexError if there is no cache.
std::size_t loadCache(FileList& list) {
    std::ifstream in(CACHE_FILE);
    if (!in) throw IndexError("No cache file yet.");

    std::string line;
    while (std::getline(in, line)) {
        std::size_t t1 = line.find('\t');
        if (t1 == std::string::npos) continue;                    // damaged line: skip
        std::size_t t2 = line.find('\t', t1 + 1);
        if (t2 == std::string::npos) continue;                    // damaged line: skip

        std::istringstream sizeText(line.substr(t1 + 1, t2 - t1 - 1));
        std::uintmax_t size = 0;
        if (!(sizeText >> size)) continue;                        // size is not a number

        list.add(line.substr(t2 + 1), line[0] == 'D', size);
    }
    return list.getTotal();
}

// Scan the disk, then save the result. A failed save is only a warning.
void buildIndex(FileList& list, const std::string& root) {
    list.clear();
    Scanner scanner(list);

    auto start = std::chrono::steady_clock::now();
    scanner.scan(root);
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    std::cout << "\nDone: " << list.getFileCount() << " files, " << list.getFolderCount()
              << " folders in " << std::fixed << std::setprecision(1) << secs << " s";
    if (scanner.getSkipped() > 0)
        std::cout << " (" << scanner.getSkipped() << " locked folders skipped)";
    std::cout << "\n";

    if (root.empty()) {                       // only cache full-drive scans
        try { saveCache(list); }
        catch (const IndexError& e) { std::cout << "Warning: " << e.what() << "\n"; }
    }
}

// ---------------------------------------------------------------------
//  Searching + showing results
// ---------------------------------------------------------------------

// POLYMORPHISM: the strategy is passed by BASE reference. The call to
// strategy.matches() runs NameSearch, ExtensionSearch or FolderSearch code
// depending on the real object behind it.
std::vector<const FileNode*> runSearch(const FileList& list, const SearchStrategy& strategy) {
    std::vector<const FileNode*> results;
    for (const FileNode* n = list.getHead(); n != nullptr; n = n->getNext())
        if (strategy.matches(*n))
            results.push_back(n);
    return results;
}

// Cut very long paths so every result stays on a single terminal line.
std::string shortenPath(const std::string& path) {
    if (path.size() <= MAX_PATH_SHOWN) return path;
    std::size_t start = path.size() - (MAX_PATH_SHOWN - 3);
    while (start < path.size() && (path[start] & 0xC0) == 0x80) ++start;  // don't cut a UTF-8 char
    return "..." + path.substr(start);
}

// Show results one page at a time (like the "more" command).
void showResults(const SearchStrategy& strategy, const std::vector<const FileNode*>& results, double ms) {
    std::size_t pages = (results.size() + PAGE_SIZE - 1) / PAGE_SIZE;
    std::size_t page = 0;

    while (true) {
        clearScreen();
        printHeader();
        std::cout << " Search : " << strategy.describe() << "\n"
                  << " Found  : " << results.size() << " results in "
                  << std::fixed << std::setprecision(2) << ms << " ms\n"
                  << "--------------------------------------------------------------\n";

        if (results.empty()) {
            std::cout << " Nothing found.\n";
            pause();
            return;
        }

        std::size_t from = page * PAGE_SIZE;
        std::size_t to = std::min(from + PAGE_SIZE, results.size());
        for (std::size_t i = from; i < to; ++i) {
            const FileNode* n = results[i];
            std::string tag = n->isFolder() ? "<DIR>" : (n->getExt().empty() ? "file" : n->getExt());
            std::string size = n->isFolder() ? "" : formatSize(n->getSize());
            std::cout << ' ' << std::left << std::setw(7) << tag.substr(0, 6)
                      << std::right << std::setw(9) << size << "  "
                      << shortenPath(n->getPath()) << "\n";
        }

        std::cout << "--------------------------------------------------------------\n"
                  << " Page " << (page + 1) << " of " << pages
                  << "   [Enter] next   [p] previous   [q] menu\n";

        std::string cmd;
        if (!readLine(" > ", cmd)) return;
        cmd = toLower(trim(cmd));

        if (cmd == "q") return;
        if (cmd == "p") { if (page > 0) --page; }
        else if (page + 1 < pages) ++page;    // Enter / anything else = next
        else return;                          // Enter on last page = back to menu
    }
}

// Ask for the search text, pick the right strategy, run it, show it.
void searchFlow(const FileList& index, const std::string& choice) {
    std::string prompt;
    if (choice == "1") prompt = "\nEnter part of a file or folder name: ";
    else if (choice == "2") prompt = "\nEnter an extension (e.g. pdf or .pdf): ";
    else prompt = "\nEnter part of a folder name: ";

    std::string text;
    if (!readLine(prompt, text)) return;
    text = trim(text);

    if (text.empty()) {                       // input validation
        std::cout << "Please type something to search for.\n";
        pause();
        return;
    }

    // A base-class pointer that can hold ANY of the three search types.
    std::unique_ptr<SearchStrategy> strategy;
    if (choice == "1") strategy = std::make_unique<NameSearch>(text);
    else if (choice == "2") strategy = std::make_unique<ExtensionSearch>(text);
    else strategy = std::make_unique<FolderSearch>(text);

    auto start = std::chrono::steady_clock::now();
    std::vector<const FileNode*> results = runSearch(index, *strategy);
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();

    showResults(*strategy, results, ms);
}

// ---------------------------------------------------------------------
//  main: menu loop
// ---------------------------------------------------------------------
int main(int argc, char* argv[]) {
    setupTerminal();
    std::string root = (argc > 1) ? argv[1] : "";   // empty = all drives

    try {
        FileList index;
        std::string status;

        // Step 1: build the index (cache first, scan if there is no cache).
        clearScreen();
        printHeader();
        bool loaded = false;
        if (root.empty()) {
            try {
                std::size_t n = loadCache(index);
                if (n > 0) {
                    status = "Loaded " + std::to_string(n) + " items from saved index. Choose 4 to rescan.";
                    loaded = true;
                }
            } catch (const IndexError&) {
                std::cout << "No saved index found. Scanning all drives (first run only)...\n\n";
            }
        }
        if (!loaded) {
            buildIndex(index, root);
            status = "Index ready.";
            pause();
        }

        // Step 2: menu loop.
        while (true) {
            clearScreen();
            printHeader();
            std::cout << " " << status << "\n"
                      << " Indexed: " << index.getFileCount() << " files, "
                      << index.getFolderCount() << " folders\n"
                      << "--------------------------------------------------------------\n"
                      << "  1. Normal search   (name of any file or folder)\n"
                      << "  2. Search by extension\n"
                      << "  3. Search folders\n"
                      << "  4. Rescan drives\n"
                      << "  5. Exit\n"
                      << "--------------------------------------------------------------\n";

            std::string choice;
            if (!readLine(" Choose (1-5): ", choice)) break;   // input closed
            choice = trim(choice);

            if (choice == "1" || choice == "2" || choice == "3") {
                searchFlow(index, choice);
            } else if (choice == "4") {
                clearScreen();
                printHeader();
                buildIndex(index, root);
                status = "Rescan finished.";
                pause();
            } else if (choice == "5") {
                break;
            } else {
                std::cout << " Invalid choice. Type a number from 1 to 5.\n";
                pause();
            }
        }
    }
    catch (const IndexError& e) {                 // our own errors
        std::cerr << "QueryFS error: " << e.what() << "\n";
        return 1;
    }
    catch (const std::exception& e) {             // anything else (e.g. out of memory)
        std::cerr << "Unexpected error: " << e.what() << "\n";
        return 1;
    }

    std::cout << "Bye!\n";
    return 0;
}