// ============================================================================
// QueryFS — single-file build
// C++ file search engine: indexes a directory tree ONCE into an in-memory
// singly linked list, then runs searches against that list through
// interchangeable, polymorphic strategies. No disk access between queries.
//
// Compile:  g++ -std=c++17 QueryFS.cpp -o queryfs        (Linux)
//           g++ -std=c++17 QueryFS.cpp -o queryfs.exe    (Windows/MinGW)
// Run:      ./queryfs  (or .\queryfs.exe) — then enter a root path
// ============================================================================

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <string>

namespace fs = std::filesystem;

// ============================================================================
// FileNode — one node of the linked list, one file's metadata.
// The constructor auto-fills every field from a path ("zero manual data
// entry"): the caller hands over a path, the constructor does the rest.
// All fields are private (encapsulation); outside code can only read them.
// ============================================================================
class FileNode {
private:
    std::string name;       // filename with extension, e.g. "report.pdf"
    std::string extension;  // extension without dot, e.g. "pdf"; "" if none
    std::string fullPath;   // path as discovered
    std::size_t size;       // bytes (0 for directories)

    FileNode* next;         // singly linked list link — only FileList touches it

public:
    // Primary ctor: auto-fill from a real filesystem path.
    // .string() is called explicitly: fs::path converts implicitly to
    // std::string on Linux but NOT on Windows (native type is wstring there).
    explicit FileNode(const std::string& path)
        : fullPath(path), size(0), next(nullptr) {
        fs::path p(path);
        name = p.filename().string();

        std::string ext = p.extension().string();
        if (!ext.empty() && ext.front() == '.') ext.erase(0, 1);
        extension = ext;

        std::error_code ec;
        if (fs::is_regular_file(p, ec)) {
            auto sz = fs::file_size(p, ec);
            if (!ec) size = static_cast<std::size_t>(sz);
        }
    }

    // Secondary ctor: explicit fields — lets tests build nodes without real
    // files on disk.
    FileNode(const std::string& name, const std::string& extension,
             const std::string& fullPath, std::size_t size)
        : name(name), extension(extension), fullPath(fullPath),
          size(size), next(nullptr) {}

    const std::string& getName() const { return name; }
    const std::string& getExtension() const { return extension; }
    const std::string& getFullPath() const { return fullPath; }
    std::size_t getSize() const { return size; }
    FileNode* getNext() const { return next; }

    // `next` stays private with FileList as friend: no public setter means
    // no outside code can splice the chain; friendship grants exactly one
    // class (the list) the right to rewire links.
    friend class FileList;
};

// ============================================================================
// FileList — the in-memory index. A singly linked list that OWNS every node
// (composition). One list == one directory scan; searches read it, never
// mutate it.
// ============================================================================
class FileList {
private:
    FileNode* head;   // first node, nullptr when empty
    FileNode* tail;   // last node, cached so append() is O(1) — no end-walk
    int count;        // maintained on append, read via getCount()

public:
    FileList() : head(nullptr), tail(nullptr), count(0) {}

    // Frees every node: the list is the single owner, so cleanup lives here.
    ~FileList() {
        FileNode* cur = head;
        while (cur != nullptr) {
            FileNode* dead = cur;
            cur = cur->getNext();
            delete dead;
        }
        head = tail = nullptr;
        count = 0;
    }

    // Copy disabled: a shallow copy would leave two lists pointing at the
    // same nodes = double free. Turning that crash into a compile error.
    FileList(const FileList&) = delete;
    FileList& operator=(const FileList&) = delete;

    // O(1) append: tail is cached, so we never walk the list.
    FileNode* append(const std::string& fullPath) {
        FileNode* node = new FileNode(fullPath);
        node->next = nullptr;
        if (tail == nullptr) head = tail = node;   // empty list
        else { tail->next = node; tail = node; }
        ++count;
        return node;
    }

    // Test append: node built from explicit fields.
    FileNode* append(const std::string& name, const std::string& extension,
                     const std::string& fullPath, std::size_t size) {
        FileNode* node = new FileNode(name, extension, fullPath, size);
        node->next = nullptr;
        if (tail == nullptr) head = tail = node;
        else { tail->next = node; tail = node; }
        ++count;
        return node;
    }

    // Visitor traversal: strategies see every node READ-ONLY. Nobody gets
    // ownership of the chain, so nobody can break it.
    template <typename Visitor>
    void traverse(Visitor&& visit) const {
        for (FileNode* cur = head; cur != nullptr; cur = cur->getNext())
            visit(cur);
    }

    int getCount() const { return count; }
    FileNode* getHead() const { return head; }
    FileNode* getTail() const { return tail; }
};

// ============================================================================
// SearchStrategy — the abstract interface. One pure-virtual rule: "given this
// index, produce your results". Pure virtual = cannot instantiate "a search"
// in the abstract, only concrete modes. The index arrives as a parameter, so
// strategies hold no reference to any particular list — that's what makes
// them swappable from the menu through one base pointer.
// ============================================================================
class SearchStrategy {
public:
    virtual void search(FileList* src) = 0;
    virtual ~SearchStrategy() = default;   // virtual: safe delete via base ptr
};

// ============================================================================
// SearchByName — exact match or substring (std::string::find), with a
// case-insensitive toggle (both sides lowercased before comparing).
// ============================================================================
class SearchByName : public SearchStrategy {
private:
    std::string query;
    bool exactMatch;
    bool caseInsensitive;

    static std::string lower(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        return s;
    }

public:
    SearchByName(const std::string& query, bool exactMatch = false,
                 bool caseInsensitive = true)
        : query(query), exactMatch(exactMatch), caseInsensitive(caseInsensitive) {}

    void search(FileList* src) override {
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

            bool match = exactMatch ? (hay == needle)
                                    : (hay.find(needle) != std::string::npos);
            if (match) {
                std::cout << "  " << node->getFullPath()
                          << "  (" << node->getSize() << " bytes)\n";
                ++hits;
            }
        });

        std::cout << "  " << hits << " match(es) found.\n";
    }
};

// ============================================================================
// SearchByType — filter by extension, or group + count every extension when
// no filter is given. One class, two views. Comparisons are
// case-insensitive; extensions stored without the dot.
// ============================================================================
class SearchByType : public SearchStrategy {
private:
    std::string oneExtension;   // "" = group+count all, else filter to this

    static std::string lower(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        return s;
    }

public:
    explicit SearchByType(const std::string& extension = "")
        : oneExtension(lower(extension)) {}

    void search(FileList* src) override {
        if (src == nullptr) return;

        if (oneExtension.empty()) {
            // Group + count mode: tally every distinct extension in one walk.
            std::map<std::string, int> tally;
            src->traverse([&](const FileNode* node) {
                std::string ext = lower(node->getExtension());
                if (ext.empty()) ext = "(no extension)";
                ++tally[ext];
            });

            std::cout << "\n--- Search by Type: summary of " << src->getCount()
                      << " indexed files ---\n";
            if (tally.empty()) { std::cout << "  Index is empty.\n"; return; }
            for (const auto& [ext, n] : tally)
                std::cout << "  " << ext << " : " << n << "\n";
        } else {
            // Filter mode: only files whose extension matches.
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
};

// ============================================================================
// SearchByDirectory — keeps files whose fullPath starts with a directory path.
// recursive=false: immediate children only (remainder after the prefix holds
// no further '/'). recursive=true: full descendants. The prefix must land on
// a PATH BOUNDARY: "/a/project" must not match "/a/project-link".
// Comparison is byte-exact — POSIX paths are case-sensitive (deliberate).
// ============================================================================
class SearchByDirectory : public SearchStrategy {
private:
    std::string dirPath;
    bool recursive;

public:
    SearchByDirectory(const std::string& dirPath, bool recursive = false)
        : dirPath(dirPath), recursive(recursive) {}

    void search(FileList* src) override {
        if (src == nullptr) return;

        std::cout << "\n--- Search by Directory: \"" << dirPath << "\" ("
                  << (recursive ? "full descendants" : "immediate children only")
                  << ") ---\n";

        // Normalize: strip trailing '/' so "dir/" and "dir" behave the same.
        std::string prefix = dirPath;
        while (prefix.size() > 1 && prefix.back() == '/') prefix.pop_back();
        if (prefix == ".") prefix = "";

        int hits = 0;

        src->traverse([&](const FileNode* node) {
            const std::string& fp = node->getFullPath();

            if (fp.compare(0, prefix.size(), prefix) != 0) return; // wrong prefix
            std::string rest = fp.substr(prefix.size());  // e.g. "/src/main.cpp"
            if (rest.empty()) return;    // the target dir itself, not a child
            if (rest[0] != '/') return;  // boundary violated (project-link case)

            if (!recursive) {
                // Immediate children only: no further '/' in the remainder.
                if (rest.substr(1).find('/') != std::string::npos) return;
            }

            std::cout << "  " << fp << "  (" << node->getSize() << " bytes)\n";
            ++hits;
        });

        std::cout << "  " << hits << " file(s) found.\n";
    }
};

// ============================================================================
// Indexer — turns a directory tree into a FileList. Pure discovery: every
// discovered path is handed to FileList::append(), and the FileNode
// constructor auto-fills the metadata.
//
// Why try/catch lives HERE: recursive_directory_iterator throws on directories
// it cannot read. Without the catch, ONE locked folder kills the whole scan;
// with it, we skip the bad entry and keep going. skip_permission_denied
// handles the common case at the iterator level; the per-entry try/catch
// covers everything else (files deleted mid-scan, etc.).
// ============================================================================
class Indexer {
public:
    static void buildIndex(const std::string& rootPath, FileList& list) {
        std::error_code ec;
        fs::path root(rootPath);

        if (!fs::exists(root, ec) || ec) {
            std::cout << "Indexer: path does not exist: " << rootPath << "\n";
            return;
        }

        // Single-file root: nothing to recurse, index the file itself.
        if (fs::is_regular_file(root, ec) && !ec) {
            list.append(root.string());
            std::cout << "Indexer: indexed 1 entry (single file): " << rootPath
                      << "\n";
            return;
        }

        int skipped = 0;

        fs::recursive_directory_iterator it(
            root, fs::directory_options::skip_permission_denied, ec);
        if (ec) {
            std::cout << "Indexer: cannot open " << rootPath << ": "
                      << ec.message() << "\n";
            return;
        }

        for (auto end = fs::end(it); it != end; it.increment(ec)) {
            if (ec) {                    // increment failed: skip the subtree
                ++skipped;
                ec.clear();
                it.pop();
                continue;
            }
            try {
                const fs::directory_entry& entry = *it;

                std::error_code e2;
                if (!entry.exists(e2) || e2) { ++skipped; continue; }

                list.append(entry.path().string());
            } catch (const std::exception&) {
                ++skipped;               // unreadable entry: skip, never crash
            }
        }

        std::cout << "Indexer: indexed " << list.getCount() << " entr"
                  << (list.getCount() == 1 ? "y" : "ies") << " under \""
                  << rootPath << "\""
                  << (skipped > 0
                          ? ("; skipped " + std::to_string(skipped)
                             + " unreadable entr" + (skipped == 1 ? "y" : "ies")
                             + ".")
                          : ".")
                  << "\n";
    }
};

// ============================================================================
// main — index once, query many.
//
// Polymorphic dispatch, and why there is NO switch-case on strategy type:
// each menu branch constructs the right subclass and hands it to the base
// pointer `strategy`; the virtual call strategy->search(&index) resolves to
// the subclass at RUNTIME. The loop never needs to know which subclass it
// holds — add a fourth strategy someday and this loop doesn't change.
//
// All input is read LINE-BY-LINE (one getline per prompt). Mixing >> and
// getline is a classic bug: >> leaves the '\n' behind and the next getline
// returns an empty string. Line-based input avoids that entirely.
// ============================================================================

namespace {

bool readLine(const std::string& label, std::string& out) {
    std::cout << label;
    return static_cast<bool>(std::getline(std::cin, out));
}

int readInt(const std::string& label, int def) {
    std::string line;
    if (!readLine(label, line)) return def;
    try { return std::stoi(line); }
    catch (const std::exception&) { return def; }
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

std::unique_ptr<SearchStrategy> runNameSearch() {
    std::string q;
    readLine("Name to search for: ", q);
    int exact = readInt("Exact match? (1=yes, 0=substring): ", 0);
    int ci    = readInt("Case-insensitive? (1=yes, 0=no): ", 1);
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
    int rec = readInt(
        "Recursive (full descendants)? (1=yes, 0=immediate children only): ", 0);
    return std::make_unique<SearchByDirectory>(q, rec == 1);
}

} // namespace

int main() {
    std::string root;
    if (!readLine("Enter root path to index: ", root)) return 0;

    // Built ONCE. Every search below reads this list from memory — no
    // re-scanning between queries, that is the whole point of the design.
    FileList index;
    Indexer::buildIndex(root, index);

    if (index.getCount() == 0) {
        std::cout << "Nothing indexed. Exiting.\n";
        return 0;
    }

    bool running = true;
    while (running) {
        switch (menu()) {
            case 1: { auto s = runNameSearch();      s->search(&index); break; }
            case 2: { auto s = runTypeSearch();      s->search(&index); break; }
            case 3: { auto s = runDirectorySearch(); s->search(&index); break; }
            case 4: running = false; break;
            default: std::cout << "Invalid choice.\n";
        }
    }

    std::cout << "Goodbye.\n";
    return 0;
}
