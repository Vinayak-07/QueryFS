// =====================================================================
//  QueryFS - File Search Engine                     (OOP CA-4 project)
// =====================================================================
//  Classes:
//    FileNode       - one file or folder (private data + getters)
//    FileList       - linked list that holds all the FileNodes
//    Scanner        - scans all drives and fills the list
//    Writer         - saves the list to a text file (ofstream)
//                     and reads it back (ifstream)
//    SearchStrategy - abstract class for searching
//      NameSearch, ExtensionSearch, FolderSearch - the 3 searches
//    Display        - everything the user sees: clear screen, menu,
//                     results, search history
//
//  How it works:
//    1. On start the program tries to open the cache file (ifstream)
//         - file found     -> read it into the list (no scanning, instant)
//         - file not found -> scan all drives, then create the file (ofstream)
//    2. Menu option "Rescan" scans the drives again and rewrites the file,
//       so that is how changes on the disk get into the cache
//    3. Search results are sorted with BUBBLE SORT (by name)
//
//  OOP concepts (for the viva):
//    Encapsulation : FileNode / FileList / Writer keep data private
//    Abstraction   : SearchStrategy is abstract (pure virtual functions)
//    Inheritance   : the 3 search classes inherit SearchStrategy
//    Polymorphism  : one SearchStrategy pointer, 3 different behaviours
//    Exceptions    : try/catch around disk and file work
//
//  Build:  g++ -std=c++17 QueryFS.cpp -o queryfs
//  (Windows version: it looks for drives C:\ D:\ E:\ ...)
// =====================================================================

#include <iostream>
#include <fstream>
#include <string>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <stdexcept>
using namespace std;
namespace fs = std::filesystem;

const string CACHE_FILE = "queryfs_index.txt";   // the saved index
const int PAGE_SIZE = 15;                        // results per page
const int SORT_LIMIT = 10000;                    // bubble sort is slow, so no sorting above this
const int MAX_DEPTH = 40;                        // stops endless folder loops
const int HISTORY_SIZE = 10;                     // how many searches we remember

// Make a string lowercase so searching is not case sensitive.
string toLower(string s) {
    for (size_t i = 0; i < s.size(); i++)
        s[i] = tolower((unsigned char)s[i]);
    return s;
}

// ---------------------------------------------------------------------
//  FileNode: one file or folder.  (ENCAPSULATION: data is private)
// ---------------------------------------------------------------------
class FileNode {
private:
    string path;        // full path, e.g. C:\Users\me\notes.txt
    string name;        // "notes.txt" in lowercase
    string ext;         // "txt" in lowercase (empty for folders)
    long long size;     // size in bytes (0 for folders)
    bool folder;        // true if it is a folder
    FileNode* next;     // next node in the linked list

public:
    // Constructor: finds the name and extension from the path.
    FileNode(string p, bool isFolder, long long bytes) {
        path = p;
        folder = isFolder;
        size = bytes;
        next = nullptr;

        size_t slash = p.find_last_of("/\\");
        if (slash == string::npos) name = toLower(p);
        else name = toLower(p.substr(slash + 1));

        ext = "";
        if (!folder) {
            size_t dot = name.find_last_of('.');
            if (dot != string::npos && dot != 0)
                ext = name.substr(dot + 1);
        }
    }

    // Getters (the & means "give the string without copying it")
    const string& getPath() { return path; }
    const string& getName() { return name; }
    const string& getExt() { return ext; }
    long long getSize() { return size; }
    bool isFolder() { return folder; }
    FileNode* getNext() { return next; }

    // Setter (used by the list to link nodes)
    void setNext(FileNode* n) { next = n; }
};

// ---------------------------------------------------------------------
//  FileList: singly linked list of FileNodes.
//  The destructor deletes every node (no memory leak).
// ---------------------------------------------------------------------
class FileList {
private:
    FileNode* head;
    FileNode* tail;
    int fileCount;
    int folderCount;

public:
    FileList() {
        head = nullptr;
        tail = nullptr;
        fileCount = 0;
        folderCount = 0;
    }

    ~FileList() { clear(); }

    // Add a new node at the end of the list.
    void add(string path, bool isFolder, long long size) {
        FileNode* node = new FileNode(path, isFolder, size);
        if (head == nullptr) {
            head = node;
            tail = node;
        } else {
            tail->setNext(node);
            tail = node;
        }
        if (isFolder) folderCount++;
        else fileCount++;
    }

    // Delete all nodes.
    void clear() {
        FileNode* current = head;
        while (current != nullptr) {
            FileNode* old = current;
            current = current->getNext();
            delete old;
        }
        head = nullptr;
        tail = nullptr;
        fileCount = 0;
        folderCount = 0;
    }

    FileNode* getHead() { return head; }
    int getFileCount() { return fileCount; }
    int getFolderCount() { return folderCount; }
    int getTotal() { return fileCount + folderCount; }
};

// ---------------------------------------------------------------------
//  Scanner: scans all drives and fills the FileList.
//  try/catch is used so a locked folder does not stop the scan.
// ---------------------------------------------------------------------
class Scanner {
private:
    FileList* list;
    int skipped;        // entries we could not read

    // Add everything inside one folder (calls itself for sub-folders).
    void scanFolder(fs::path dir, int depth) {
        if (depth > MAX_DEPTH) return;

        try {
            for (fs::directory_entry entry : fs::directory_iterator(dir, fs::directory_options::skip_permission_denied)) {
                try {
                    bool isFolder = entry.is_directory();
                    long long size = 0;
                    if (!isFolder) size = (long long)entry.file_size();

                    list->add(entry.path().string(), isFolder, size);

                    if (list->getTotal() % 5000 == 0)
                        cout << "\r  Scanning... " << list->getTotal() << " items" << flush;

                    // go inside folders, but not shortcuts (symlinks)
                    if (isFolder && !entry.is_symlink())
                        scanFolder(entry.path(), depth + 1);
                }
                catch (exception&) {
                    skipped++;      // this one entry could not be read
                }
            }
        }
        catch (exception&) {
            skipped++;              // the whole folder could not be opened
        }
    }

public:
    Scanner(FileList* target) {
        list = target;
        skipped = 0;
    }

    // Try every drive letter A: to Z: and scan the ones that exist.
    void scanAll() {
        skipped = 0;
        bool found = false;

        for (char letter = 'A'; letter <= 'Z'; letter++) {
            string drive = string(1, letter) + ":\\";
            error_code ec;
            if (fs::exists(drive, ec)) {
                found = true;
                cout << "Scanning drive " << drive << "\n";
                scanFolder(fs::path(drive), 0);
                cout << "\r  " << list->getTotal() << " items so far          \n";
            }
        }

        if (!found) throw runtime_error("No drives found.");
    }

    int getSkipped() { return skipped; }
};

// ---------------------------------------------------------------------
//  Writer: saves the list in a text file and reads it back.
//  One line per entry:   F<TAB>size<TAB>path      (D for folders)
// ---------------------------------------------------------------------
class Writer {
private:
    string fileName;

public:
    Writer(string name) { fileName = name; }

    // Write the whole list into the file with ofstream.
    // This creates the file if it is not there.
    void save(FileList& list) {
        ofstream out(fileName);
        if (!out) throw runtime_error("Could not write " + fileName);

        for (FileNode* n = list.getHead(); n != nullptr; n = n->getNext()) {
            out << (n->isFolder() ? 'D' : 'F') << '\t'
                << n->getSize() << '\t'
                << n->getPath() << '\n';
        }
    }

    // Read the file with ifstream and add every line to the list.
    // Returns false if the file does not exist.
    bool load(FileList& list) {
        ifstream in(fileName);
        if (!in) return false;

        string line;
        while (getline(in, line)) {
            size_t tab1 = line.find('\t');
            size_t tab2 = line.find('\t', tab1 + 1);
            if (tab1 == string::npos || tab2 == string::npos)
                continue;                                   // bad line, skip it

            bool isFolder = (line[0] == 'D');
            long long size = atoll(line.substr(tab1 + 1, tab2 - tab1 - 1).c_str());
            string path = line.substr(tab2 + 1);

            list.add(path, isFolder, size);
        }
        return true;
    }
};

// ---------------------------------------------------------------------
//  SearchStrategy: abstract class (ABSTRACTION).
//  It only says WHAT a search must do. The child classes say HOW.
// ---------------------------------------------------------------------
class SearchStrategy {
public:
    virtual ~SearchStrategy() {}
    virtual bool matches(FileNode* node) = 0;    // pure virtual
    virtual string describe() = 0;               // pure virtual
};

// 1) Normal search: name of any file or folder contains the text.
class NameSearch : public SearchStrategy {
private:
    string keyword;
public:
    NameSearch(string text) { keyword = toLower(text); }
    bool matches(FileNode* node) {
        return node->getName().find(keyword) != string::npos;
    }
    string describe() { return "name contains \"" + keyword + "\""; }
};

// 2) Extension search: files with exactly this extension.
class ExtensionSearch : public SearchStrategy {
private:
    string wanted;
public:
    ExtensionSearch(string text) {
        wanted = toLower(text);
        if (!wanted.empty() && wanted[0] == '.')
            wanted = wanted.substr(1);           // accept ".pdf" and "pdf"
    }
    bool matches(FileNode* node) {
        return !node->isFolder() && node->getExt() == wanted;
    }
    string describe() { return "extension is \"." + wanted + "\""; }
};

// 3) Folder search: only folders whose name contains the text.
class FolderSearch : public SearchStrategy {
private:
    string keyword;
public:
    FolderSearch(string text) { keyword = toLower(text); }
    bool matches(FileNode* node) {
        return node->isFolder() && node->getName().find(keyword) != string::npos;
    }
    string describe() { return "folder name contains \"" + keyword + "\""; }
};

// ---------------------------------------------------------------------
//  Display: everything the user sees on the terminal.
// ---------------------------------------------------------------------
class Display {
private:
    string history[HISTORY_SIZE];   // last searches
    int historyCount;

public:
    Display() { historyCount = 0; }

    // Clear the terminal and move the cursor to the top.
    void clearScreen() {
        cout << "\033[2J\033[H";
    }

    void showHeader() {
        cout << "==============================================================\n"
             << "   QueryFS - File Search Engine\n"
             << "==============================================================\n";
    }

    void showMessage(string msg) {
        cout << " " << msg << "\n";
    }

    // Wait for the user to press Enter.
    void pause() {
        string dummy;
        cout << "\n Press Enter to continue...";
        getline(cin, dummy);
    }

    // Print a question and read the answer. Returns false if input ended.
    bool ask(string question, string& answer) {
        cout << question;
        return (bool)getline(cin, answer);
    }

    // The main menu.
    void showMenu(int files, int folders) {
        clearScreen();
        showHeader();
        cout << " Indexed: " << files << " files, " << folders << " folders\n"
             << "--------------------------------------------------------------\n"
             << "  1. Normal search   (name of any file or folder)\n"
             << "  2. Search by extension\n"
             << "  3. Search folders\n"
             << "  4. Show search history\n"
             << "  5. Rescan drives   (updates the cache file)\n"
             << "  6. Exit\n"
             << "--------------------------------------------------------------\n";
    }

    // Remember one search. When the list is full, the oldest one is removed.
    void addHistory(string entry) {
        if (historyCount == HISTORY_SIZE) {
            for (int i = 0; i < HISTORY_SIZE - 1; i++)
                history[i] = history[i + 1];
            historyCount--;
        }
        history[historyCount] = entry;
        historyCount++;
    }

    void showHistory() {
        clearScreen();
        showHeader();
        cout << " Search history (oldest first)\n"
             << "--------------------------------------------------------------\n";
        if (historyCount == 0) {
            cout << " No searches yet.\n";
        } else {
            for (int i = 0; i < historyCount; i++)
                cout << "  " << (i + 1) << ". " << history[i] << "\n";
        }
        pause();
    }

    // Show the results page by page.
    void showResults(string searchText, FileNode** results, int count, double ms, bool sorted) {
        int pages = (count + PAGE_SIZE - 1) / PAGE_SIZE;
        int page = 0;

        while (true) {
            clearScreen();
            showHeader();
            cout << " Search : " << searchText << "\n"
                 << " Found  : " << count << " results in " << ms << " ms";
            if (sorted) cout << " (bubble sorted by name)";
            else cout << " (too many results, not sorted)";
            cout << "\n--------------------------------------------------------------\n";

            if (count == 0) {
                cout << " Nothing found.\n";
                pause();
                return;
            }

            int from = page * PAGE_SIZE;
            int to = from + PAGE_SIZE;
            if (to > count) to = count;

            for (int i = from; i < to; i++) {
                FileNode* n = results[i];
                if (n->isFolder())
                    cout << " [DIR]  " << n->getPath() << "\n";
                else
                    cout << " [FILE] " << n->getPath() << "  (" << n->getSize() / 1024 << " KB)\n";
            }

            cout << "--------------------------------------------------------------\n"
                 << " Page " << (page + 1) << " of " << pages
                 << "   [Enter] next   [p] previous   [q] menu\n";

            string cmd;
            if (!ask(" > ", cmd)) return;

            if (cmd == "q") return;
            if (cmd == "p") {
                if (page > 0) page--;
            }
            else if (page + 1 < pages) page++;   // Enter = next page
            else return;                         // Enter on the last page = menu
        }
    }
};

// ---------------------------------------------------------------------
//  Bubble sort (sorts the results A-Z by name)
//  Compare neighbours and swap them if they are in the wrong order.
//  After every pass the biggest name moves to the end.
//  If a pass has no swap, the array is already sorted, so we stop.
// ---------------------------------------------------------------------
void bubbleSort(FileNode** arr, int n) {
    for (int i = 0; i < n - 1; i++) {
        bool swapped = false;
        for (int j = 0; j < n - 1 - i; j++) {
            if (arr[j]->getName() > arr[j + 1]->getName()) {
                FileNode* temp = arr[j];
                arr[j] = arr[j + 1];
                arr[j + 1] = temp;
                swapped = true;
            }
        }
        if (!swapped) break;
    }
}

// ---------------------------------------------------------------------
//  Doing one search: ask text -> search -> bubble sort -> show
// ---------------------------------------------------------------------
void doSearch(FileList& list, Display& display, string choice) {
    string question;
    if (choice == "1") question = " Enter part of a file or folder name: ";
    else if (choice == "2") question = " Enter an extension (e.g. pdf): ";
    else question = " Enter part of a folder name: ";

    string text;
    if (!display.ask(question, text)) return;

    if (text.empty()) {
        display.showMessage("Please type something to search.");
        display.pause();
        return;
    }

    // POLYMORPHISM: one base class pointer can point to ANY of the 3 searches.
    SearchStrategy* strategy = nullptr;
    if (choice == "1") strategy = new NameSearch(text);
    else if (choice == "2") strategy = new ExtensionSearch(text);
    else strategy = new FolderSearch(text);

    clock_t start = clock();

    // Pass 1: count how many nodes match.
    int count = 0;
    for (FileNode* n = list.getHead(); n != nullptr; n = n->getNext())
        if (strategy->matches(n))       // runs the child class version
            count++;

    // Pass 2: put the matching nodes in an array of exactly that size.
    FileNode** results = new FileNode*[count];
    int k = 0;
    for (FileNode* n = list.getHead(); n != nullptr; n = n->getNext())
        if (strategy->matches(n))
            results[k++] = n;

    // Bubble sort (only when there are not too many results).
    bool sorted = false;
    if (count <= SORT_LIMIT) {
        bubbleSort(results, count);
        sorted = true;
    }

    double ms = (clock() - start) * 1000.0 / CLOCKS_PER_SEC;

    display.addHistory(strategy->describe() + "  -> " + to_string(count) + " results");
    display.showResults(strategy->describe(), results, count, ms, sorted);

    delete[] results;
    delete strategy;
}

// ---------------------------------------------------------------------
//  Scan all drives and save the result in the cache file.
// ---------------------------------------------------------------------
void scanAndSave(FileList& list, Scanner& scanner, Writer& writer, Display& display) {
    list.clear();
    scanner.scanAll();

    cout << "\n Done: " << list.getFileCount() << " files, "
         << list.getFolderCount() << " folders";
    if (scanner.getSkipped() > 0)
        cout << " (" << scanner.getSkipped() << " entries skipped)";
    cout << "\n";

    try {
        writer.save(list);
        display.showMessage("Saved to " + CACHE_FILE);
    }
    catch (exception& e) {
        display.showMessage(string("Warning: ") + e.what());
    }
}

// ---------------------------------------------------------------------
//  main
// ---------------------------------------------------------------------
int main() {
    FileList list;
    Display display;
    Scanner scanner(&list);
    Writer writer(CACHE_FILE);

    try {
        display.clearScreen();
        display.showHeader();

        // Cache file there? Read it. Not there (or empty)? Scan and make it.
        if (writer.load(list) && list.getTotal() > 0) {
            display.showMessage("Loaded " + to_string(list.getTotal()) + " items from " + CACHE_FILE);
        } else {
            display.showMessage("No cache file found. Scanning all drives (first run only)...\n");
            scanAndSave(list, scanner, writer, display);
        }
        display.pause();

        // menu loop
        while (true) {
            display.showMenu(list.getFileCount(), list.getFolderCount());

            string choice;
            if (!display.ask(" Choose (1-6): ", choice)) break;

            if (choice == "1" || choice == "2" || choice == "3") {
                doSearch(list, display, choice);
            }
            else if (choice == "4") {
                display.showHistory();
            }
            else if (choice == "5") {
                display.clearScreen();
                display.showHeader();
                scanAndSave(list, scanner, writer, display);
                display.pause();
            }
            else if (choice == "6") {
                break;
            }
            else {
                display.showMessage("Invalid choice. Type a number from 1 to 6.");
                display.pause();
            }
        }
    }
    catch (exception& e) {
        cout << "Error: " << e.what() << "\n";
        return 1;
    }

    cout << "Bye!\n";
    return 0;
}