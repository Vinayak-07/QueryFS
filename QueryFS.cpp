// QueryFS v3.2 — interactive file search.
// One file, C++17, standard library only, Windows + Linux/macOS.
// Build:  g++ -std=c++17 -O2 QueryFS.cpp -o queryfs
// Run:    ./queryfs [path]   (default: $HOME on POSIX, %USERPROFILE% on Windows)

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#else
#include <termios.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

// ASCII-only lowercase. Bytes >= 0x80 pass through, so UTF-8 stays valid.
static std::string lower(const std::string& s) {
    std::string r = s;
    for (char& c : r) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return r;
}

// Where to look when no path is given on the command line.
static std::string defaultRoot() {
#ifdef _WIN32
    const char* up = std::getenv("USERPROFILE");
    return up ? up : "C:\\";
#else
    const char* h = std::getenv("HOME");
    return h ? h : "/";
#endif
}

// One indexed file or folder. Encapsulation: fields are private, the rest
// of the program reads them through getters.
class FileNode {
    std::string name, ext, fullPath, parentDir;
    std::size_t size;
    bool isDir;
    FileNode* next;
public:
    explicit FileNode(const std::string& p)          // build from a real path
        : fullPath(p), size(0), isDir(false), next(nullptr) {
        splitPath(p);
        std::error_code ec;
        isDir = fs::is_directory(fs::path(p), ec);
        if (!isDir) {
            auto sz = fs::file_size(fs::path(p), ec);
            if (!ec) size = (std::size_t)sz;
        }
    }
    FileNode(const std::string& p, std::size_t s, bool d)  // build from cache
        : fullPath(p), size(s), isDir(d), next(nullptr) {
        splitPath(p);
    }
    const std::string& getName()   const { return name; }
    const std::string& getExt()    const { return ext; }
    const std::string& getPath()   const { return fullPath; }
    const std::string& getParent() const { return parentDir; }
    std::size_t        getSize()   const { return size; }
    bool               isDirectory() const { return isDir; }
    FileNode*          getNext()   const { return next; }
    friend class FileList;                            // list may rewire next
private:
    void splitPath(const std::string& p) {
        fs::path path(p);
        name = path.filename().string();
        parentDir = path.parent_path().generic_string();
        ext = path.extension().string();
        if (!ext.empty() && ext[0] == '.') ext.erase(0, 1);
    }
};

// Singly linked list that owns every FileNode. Composition: the list owns
// its nodes and frees them all in its destructor.
class FileList {
    FileNode* head = nullptr;
    FileNode* tail = nullptr;
    int count = 0;
public:
    FileList() = default;
    FileList(const FileList&) = delete;               // a copy would double-free
    FileList& operator=(const FileList&) = delete;
    ~FileList() { clear(); }

    void append(const std::string& path) { link(new FileNode(path)); }
    void adopt(FileNode* n)              { link(n); }
    void clear() {
        FileNode* cur = head;
        while (cur) { FileNode* dead = cur; cur = cur->getNext(); delete dead; }
        head = tail = nullptr;
        count = 0;
    }
    FileNode* getHead() const { return head; }
    int getCount() const { return count; }
private:
    void link(FileNode* n) {                          // O(1) append using tail
        if (!tail) head = tail = n;
        else { tail->next = n; tail = n; }
        ++count;
    }
};

// Abstract search rule. Polymorphism: one base pointer, three concrete modes.
class SearchStrategy {
public:
    virtual ~SearchStrategy() = default;
    virtual bool matches(const FileNode* n) const = 0;
};

// File name contains the query text (case-insensitive). Inheritance example.
class SearchByName : public SearchStrategy {
    std::string needle;
public:
    explicit SearchByName(const std::string& q) : needle(lower(q)) {}
    bool matches(const FileNode* n) const override {
        return lower(n->getName()).find(needle) != std::string::npos;
    }
};

// Extension matches exactly (leading dot optional). Inheritance example.
class SearchByType : public SearchStrategy {
    std::string ext;
public:
    explicit SearchByType(const std::string& e) : ext(lower(e)) {
        if (!ext.empty() && ext[0] == '.') ext.erase(0, 1);
    }
    bool matches(const FileNode* n) const override {
        return !n->isDirectory() && lower(n->getExt()) == ext;
    }
};

// File path lies under the given directory. Inheritance example.
class SearchByDirectory : public SearchStrategy {
    std::string prefix;
public:
    explicit SearchByDirectory(const std::string& d)
        : prefix(fs::path(d).generic_string()) {
        while (prefix.size() > 1 && prefix.back() == '/') prefix.pop_back();
    }
    bool matches(const FileNode* n) const override {
        const std::string& p = n->getPath();
        if (p.size() <= prefix.size()) return false;
        if (p.compare(0, prefix.size(), prefix) != 0) return false;
        return p[prefix.size()] == '/';               // must land on a path boundary
    }
};

// Folders only: folder name contains the query text.
class SearchFoldersOnly : public SearchStrategy {
    std::string needle;
public:
    explicit SearchFoldersOnly(const std::string& q) : needle(lower(q)) {}
    bool matches(const FileNode* n) const override {
        return n->isDirectory() &&
               lower(n->getName()).find(needle) != std::string::npos;
    }
};

// Files only. The text matches the file name OR the extension, so both
// "/file report" (by name) and "/file pdf" (by type) work.
class SearchFilesOnly : public SearchStrategy {
    std::string needle, ext;
public:
    explicit SearchFilesOnly(const std::string& q) : needle(lower(q)), ext(lower(q)) {
        if (!ext.empty() && ext[0] == '.') ext.erase(0, 1);
    }
    bool matches(const FileNode* n) const override {
        if (n->isDirectory()) return false;
        return lower(n->getName()).find(needle) != std::string::npos
            || lower(n->getExt()) == ext;
    }
};

// One search term. Kind None means "show everything".
struct Query {
    enum Kind { None, Name, Ext, Dir, Folder, File };
    Kind kind = None;
    std::string value;
    bool valid = true;
};

// Parses the search box: "/folder x", "/file x", "/ext x", "/dir x",
// "/name x", or plain text.
static Query parseQuery(const std::string& line) {
    Query q;
    if (line.empty()) return q;
    if (line[0] != '/') { q.kind = Query::Name; q.value = line; return q; }
    std::size_t sp = line.find(' ');
    if (sp == std::string::npos || sp + 1 >= line.size()) { q.valid = false; return q; }
    std::string cmd = lower(line.substr(1, sp - 1));
    q.value = line.substr(sp + 1);
    if (cmd == "name") q.kind = Query::Name;
    else if (cmd == "ext") q.kind = Query::Ext;
    else if (cmd == "dir") q.kind = Query::Dir;
    else if (cmd == "folder") q.kind = Query::Folder;
    else if (cmd == "file") q.kind = Query::File;
    else q.valid = false;
    return q;
}

// True if this entry is a symlink, or (on Windows) a junction / reparse
// point. We must not descend into either: junctions like AppData\Local\
// Application Data form loops that make the scan run forever.
static bool isLinkOrReparse(const fs::directory_entry& entry) {
#ifdef _WIN32
    DWORD attrs = GetFileAttributesW(entry.path().c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) return true;     // err on caution
    return (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
    std::error_code ec;
    return entry.is_symlink(ec);
#endif
}

// Walks a directory tree and appends every entry to a FileList.
class Indexer {
public:
    static void run(const std::string& root, FileList& list) {
        try { walk(fs::path(root), list); } catch (...) { /* keep what we got */ }
    }
private:
    static void walk(const fs::path& dir, FileList& list) {
        std::error_code ec;
        fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
        if (ec) return;                                // unreadable dir: skip it
        for (const auto& entry : it) {
            std::error_code e2;
            bool isDir  = entry.is_directory(e2); if (e2) continue;
            bool isLink = isLinkOrReparse(entry);
            list.append(entry.path().generic_string());
            if (isDir && !isLink) walk(entry.path(), list);   // don't follow links
        }
    }
};

// Text cache: one tab-separated line per entry (path, size, isDir).
static std::string cacheFilePath(const std::string& root) {
    std::size_t h = 0;
    for (char c : root) h = h * 131 + (unsigned char)c;
    char buf[32];
    std::snprintf(buf, sizeof buf, "%zx", h);
    std::string dir;
#ifdef _WIN32
    const char* a = std::getenv("APPDATA");
    dir = a ? std::string(a) + "/queryfs" : ".";
#else
    const char* hm = std::getenv("HOME");
    dir = hm ? std::string(hm) + "/.cache/queryfs" : "/tmp";
#endif
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir + "/index-" + buf + ".txt";
}

static void saveCache(const std::string& root, const FileList& list) {
    std::ofstream out(cacheFilePath(root));
    if (!out) return;
    for (FileNode* n = list.getHead(); n; n = n->getNext())
        out << n->getPath() << '\t' << n->getSize() << '\t'
            << (n->isDirectory() ? '1' : '0') << '\n';
}

static void loadCache(const std::string& root, FileList& list) {
    std::ifstream in(cacheFilePath(root));
    if (!in) return;
    std::string line;
    while (std::getline(in, line)) {
        std::size_t t1 = line.find('\t');
        if (t1 == std::string::npos) continue;        // bad line: skip
        std::size_t t2 = line.find('\t', t1 + 1);
        if (t2 == std::string::npos) continue;
        std::string path = line.substr(0, t1);
        if (path.empty()) continue;
        std::size_t sz = (std::size_t)std::strtoull(line.substr(t1 + 1).c_str(), nullptr, 10);
        bool isDir = line.substr(t2 + 1) == "1";
        list.adopt(new FileNode(path, sz, isDir));
    }
}

// Tiny terminal helper. Platform abstraction: one interface, two #ifdef
// implementations for raw key input.
class Terminal {
public:
    enum Key { NONE, UP, DOWN, TAB, ENTER, ESC, BACKSPACE, CHAR, EOF_KEY };
    struct Press { Key key; char ch; };

    bool isTty() const {
#ifdef _WIN32
        return _isatty(_fileno(stdin)) != 0;
#else
        return isatty(STDIN_FILENO) == 1;
#endif
    }
    void enableRaw() {
#ifdef _WIN32
        HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
        GetConsoleMode(h, &oldWin);
        SetConsoleMode(h, (oldWin & ~(ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT |
                                      ENABLE_PROCESSED_INPUT)) | ENABLE_EXTENDED_FLAGS);
#else
        tcgetattr(STDIN_FILENO, &oldPosix);
        struct termios raw = oldPosix;
        raw.c_lflag &= ~(ECHO | ICANON | ISIG);       // Ctrl+C arrives as byte 3
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 1;                          // 100 ms timeout for redraws
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
#endif
    }
    void disableRaw() {
#ifdef _WIN32
        SetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), oldWin);
#else
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &oldPosix);
#endif
    }
    Press readKey();
private:
#ifdef _WIN32
    DWORD oldWin = 0;
#else
    struct termios oldPosix{};
#endif
};

Terminal::Press Terminal::readKey() {
#ifdef _WIN32
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    if (WaitForSingleObject(h, 100) == WAIT_TIMEOUT) return {NONE, 0};
    INPUT_RECORD rec; DWORD got;
    if (!ReadConsoleInputW(h, &rec, 1, &got) || !got) return {EOF_KEY, 0};
    if (rec.EventType != KEY_EVENT || !rec.Event.KeyEvent.bKeyDown) return {NONE, 0};
    WORD vk = rec.Event.KeyEvent.wVirtualKeyCode;
    if (vk == VK_UP)     return {UP, 0};
    if (vk == VK_DOWN)   return {DOWN, 0};
    if (vk == VK_TAB)    return {TAB, 0};
    if (vk == VK_RETURN) return {ENTER, 0};
    if (vk == VK_ESCAPE) return {ESC, 0};
    if (vk == VK_BACK)   return {BACKSPACE, 0};
    wchar_t c = rec.Event.KeyEvent.uChar.UnicodeChar;
    return (c > 0 && c < 128) ? Press{CHAR, (char)c} : Press{NONE, 0};
#else
    unsigned char c;
    if (read(STDIN_FILENO, &c, 1) != 1) return {NONE, 0};    // timeout
    if (c == 27) {                                           // ESC, maybe an arrow
        unsigned char b1, b2;
        if (read(STDIN_FILENO, &b1, 1) != 1) return {ESC, 0};
        if (b1 != '[') return {ESC, 0};
        if (read(STDIN_FILENO, &b2, 1) != 1) return {ESC, 0};
        if (b2 == 'A') return {UP, 0};
        if (b2 == 'B') return {DOWN, 0};
        return {NONE, 0};
    }
    if (c == 3) return {ESC, 0};                             // Ctrl+C
    if (c == 9) return {TAB, 0};
    if (c == 10 || c == 13) return {ENTER, 0};
    if (c == 127 || c == 8) return {BACKSPACE, 0};
    return {CHAR, (char)c};
#endif
}

// Opens a file or folder with the OS default application.
static void openPath(const std::string& path) {
#ifdef _WIN32
    std::string cmd = "start \"\" \"" + path + "\"";
#elif defined(__APPLE__)
    std::string cmd = "open \"" + path + "\"";
#else
    std::string cmd = "xdg-open \"" + path + "\"";
#endif
    std::system(cmd.c_str());
}

// Everything the UI remembers between frames.
struct UiState {
    std::string query, lastQuery = "\x01";            // impossible sentinel forces first run
    std::vector<const FileNode*> matches;             // files AND folders
    int sel = 0;                                      // highlighted row
    bool running = true, dirty = true;
};

// Runs one query over the index. Matches are files AND folders (filtered by
// the chosen strategy), sorted by path.
static void runSearch(const FileList& index, const Query& q,
                      std::vector<const FileNode*>& matches) {
    std::unique_ptr<SearchStrategy> strat;
    if      (q.kind == Query::Name)   strat = std::make_unique<SearchByName>(q.value);
    else if (q.kind == Query::Ext)    strat = std::make_unique<SearchByType>(q.value);
    else if (q.kind == Query::Dir)    strat = std::make_unique<SearchByDirectory>(q.value);
    else if (q.kind == Query::Folder) strat = std::make_unique<SearchFoldersOnly>(q.value);
    else if (q.kind == Query::File)   strat = std::make_unique<SearchFilesOnly>(q.value);

    matches.clear();
    for (FileNode* n = index.getHead(); n; n = n->getNext()) {
        if (strat && !strat->matches(n)) continue;
        matches.push_back(n);
    }
    std::sort(matches.begin(), matches.end(),
              [](const FileNode* a, const FileNode* b) { return a->getPath() < b->getPath(); });
}

// Builds the frame as one string, then writes it once.
static void drawScreen(const UiState& s, bool scanning) {
    const int ROWS = 20;
    int total = (int)s.matches.size();
    int top = (s.sel >= ROWS) ? s.sel - ROWS + 1 : 0; // scroll to keep cursor visible

    std::string out = "\033[2J\033[H";
    out += "Search: " + s.query + "_\n";
    out += std::string(64, '-') + "\n";

    for (int i = 0; i < ROWS && top + i < total; ++i) {
        int idx = top + i;
        const FileNode* n = s.matches[idx];
        std::string tag = n->isDirectory() ? "(folder)" : "(" + n->getExt() + ")";
        if (tag == "()") tag = "(file)";              // file with no extension
        if (tag.size() < 9) tag.resize(9, ' ');
        else tag += ' ';
        std::string line = std::string(1, idx == s.sel ? '>' : ' ') + " "
                         + tag + n->getPath();
        if ((int)line.size() > 120) line = line.substr(0, 117) + "...";
        out += line + "\n";
    }

    out += std::string(64, '-') + "\n";
    out += " " + std::to_string(total) + " results   Enter=open  Esc=quit\n";
    out += " text=name  /folder name  /file type  /ext ext  /dir path";
    if (scanning) out += "   [indexing...]";
    out += "\n";
    std::fputs(out.c_str(), stdout);
    std::fflush(stdout);
}

// Handles one keypress and updates the UI state.
static void handleKey(Terminal::Key key, char ch, UiState& s) {
    switch (key) {
        case Terminal::ESC: s.running = false; break;
        case Terminal::UP:
            if (s.sel > 0) --s.sel;
            s.dirty = true; break;
        case Terminal::DOWN:
            if (s.sel + 1 < (int)s.matches.size()) ++s.sel;
            s.dirty = true; break;
        case Terminal::ENTER:
            if (s.sel < (int)s.matches.size())
                openPath(s.matches[s.sel]->getPath());
            break;
        case Terminal::BACKSPACE:
            if (!s.query.empty()) {
                s.query.pop_back();
                while (!s.query.empty() && (s.query.back() & 0xC0) == 0x80)
                    s.query.pop_back();               // delete the whole UTF-8 char
            }
            s.dirty = true; break;
        case Terminal::CHAR: s.query += ch; s.dirty = true; break;
        default: break;
    }
}

// ---- globals: the index, its mutex, and the "scanning" flag ----
static std::shared_ptr<FileList> g_index;
static std::mutex g_indexMutex;
static std::atomic<bool> g_scanning{false};

// Starts the background scan; returns the thread so main can join it.
static std::thread startScanThread(const std::string& root) {
    return std::thread([root] {
        auto fresh = std::make_shared<FileList>();
        Indexer::run(root, *fresh);
        { std::lock_guard<std::mutex> lk(g_indexMutex); g_index = fresh; }  // one swap
        saveCache(root, *fresh);
        g_scanning = false;
    });
}

// Re-runs the query when the search text or the index itself changed.
static void refreshResults(const std::shared_ptr<FileList>& index,
                           const std::shared_ptr<FileList>& lastIndex, UiState& s) {
    if (s.query == s.lastQuery && index == lastIndex) return;
    Query q = parseQuery(s.query);
    if (q.valid) {
        runSearch(*index, q, s.matches);
        s.sel = 0;
    }
    s.lastQuery = s.query;
    s.dirty = true;
}

int main(int argc, char** argv) {
    std::string root = defaultRoot();
    if (argc > 1 && argv[1][0] != '-') root = argv[1];

    Terminal term;
    if (!term.isTty()) {
        std::fprintf(stderr, "queryfs: stdin is not a terminal\n");
        return 1;
    }

    g_index = std::make_shared<FileList>();
    loadCache(root, *g_index);                 // instant startup if cache exists

    // Cold start (no cache): scan synchronously so the very first frame has
    // results. Warm start: show the cached index immediately, rescan in the
    // background under the mutex, swap once when done.
    std::thread scanner;
    if (g_index->getHead() == nullptr) {
        std::fputs("Indexing... please wait\n", stdout);
        std::fflush(stdout);
        Indexer::run(root, *g_index);
        saveCache(root, *g_index);
        g_scanning = false;
    } else {
        g_scanning = true;
        scanner = startScanThread(root);
    }

    term.enableRaw();
    UiState s;
    bool lastScan = g_scanning.load();
    std::shared_ptr<FileList> lastIndex;

    while (s.running) {
        std::shared_ptr<FileList> index;
        { std::lock_guard<std::mutex> lk(g_indexMutex); index = g_index; }

        refreshResults(index, lastIndex, s);
        lastIndex = index;

        bool scanning = g_scanning.load();
        if (scanning != lastScan) { lastScan = scanning; s.dirty = true; }
        if (s.dirty) { drawScreen(s, scanning); s.dirty = false; }

        Terminal::Press kp = term.readKey();
        handleKey(kp.key, kp.ch, s);
    }

    term.disableRaw();
    if (scanner.joinable()) scanner.join();
    return 0;
}