# QueryFS - File Search Engine

A C++ console program that scans all drives of a Windows computer, remembers every file and folder in a linked list, and lets the user search them instantly by name, by extension or by folder.

- Course: Object Oriented Programming (ITY23PCL203), CA-4 project
- Author: Vinayak, Roll No. 2506033
- Institute: IICT, MGM University

---

## Features

- Scans all drives (C:, D:, E:, ...) automatically
- Saves the result in a text file (cache), so the next start is instant
- Three searches: normal (name), by extension, by folder
- Search results are sorted A-Z using bubble sort
- Results are shown page by page (15 per page)
- Keeps a history of the last 10 searches
- Skips locked or unreadable folders instead of crashing

---

## How it works

### On start

1. The program tries to open `queryfs_index.txt` with `ifstream`.
2. If the file exists, every line is read and added to the linked list. No scanning is needed, so the program is ready immediately.
3. If the file does not exist (or is empty), the Scanner scans all drives and `ofstream` creates the file.

### Rescan

Menu option 5 scans all drives again and rewrites the cache file. Use it after files have been added, deleted or renamed, because the cache only changes when a rescan is done.

### Searching

1. The user chooses a search type and types the text.
2. A `SearchStrategy` object for that type is created.
3. The linked list is walked once to count the matches, and a second time to put them in an array of exactly that size.
4. The array is sorted with bubble sort (by file name, A-Z).
5. The results are shown 15 at a time.

---

## Classes

| Class | Job |
|-------|-----|
| `FileNode` | One file or folder: path, lowercase name, extension, size, folder flag, next pointer |
| `FileList` | Singly linked list that owns all the nodes. Its destructor deletes every node |
| `Scanner` | Walks through all drives with `std::filesystem` and adds every entry to the list |
| `Writer` | `save()` writes the list to the cache file (`ofstream`), `load()` reads it back (`ifstream`) |
| `SearchStrategy` | Abstract base class with two pure virtual functions: `matches()` and `describe()` |
| `NameSearch` | Matches files and folders whose name contains the text |
| `ExtensionSearch` | Matches files with exactly the given extension (`pdf` or `.pdf`) |
| `FolderSearch` | Matches only folders whose name contains the text |
| `Display` | Everything shown on screen: clear screen, header, menu, results, search history |

---

## OOP concepts used

| Concept | Where |
|---------|-------|
| Encapsulation | `FileNode`, `FileList`, `Scanner`, `Writer` and `Display` keep their data private and use getters or public functions |
| Abstraction | `SearchStrategy` is abstract. It says what a search must do, not how |
| Inheritance | `NameSearch`, `ExtensionSearch` and `FolderSearch` inherit from `SearchStrategy` |
| Polymorphism | One `SearchStrategy*` pointer holds any of the three searches, and `strategy->matches()` runs the correct version |
| Constructors and destructors | `FileNode` works out its name and extension in the constructor. `FileList` frees all nodes in the destructor |
| Exception handling | `try/catch` in the Scanner (locked folders), in `scanAndSave()` (cache write errors) and in `main()` |
| File handling | `ifstream` and `ofstream` in the `Writer` class |

---

## Build and run

Needs a C++17 compiler (tested with g++ on MSYS2).

```
g++ -std=c++17 QueryFS.cpp -o queryfs
./queryfs
```

On first run it scans every drive and creates `queryfs_index.txt` in the folder you ran it from. This can take a few minutes on a big disk. Every run after that loads the file and starts at once.

---

## Menu

```
==============================================================
   QueryFS - File Search Engine
==============================================================
 Indexed: 175530 files, 18707 folders
--------------------------------------------------------------
  1. Normal search   (name of any file or folder)
  2. Search by extension
  3. Search folders
  4. Show search history
  5. Rescan drives   (updates the cache file)
  6. Exit
--------------------------------------------------------------
```

While looking at results:

- Enter - next page
- p - previous page
- q - back to the menu

Searching is not case sensitive.

---

## Cache file format

`queryfs_index.txt` is a plain text file with one line per file or folder. The three parts of a line are separated by a tab:

```
F	2048	C:\Users\me\notes.txt
D	0	C:\Users\me\Documents
```

- First part: `F` for a file, `D` for a folder
- Second part: size in bytes (0 for folders)
- Third part: full path

Lines that are damaged are skipped when the file is loaded.

---

## Bubble sort

After a search, the results array is sorted by file name using bubble sort. Neighbouring items are compared and swapped if they are in the wrong order. After every pass the largest name moves to the end. If a pass makes no swap, the array is already sorted and the loop stops early.

Bubble sort needs about n x n comparisons, so it becomes slow for very large result lists. For that reason results are only sorted when there are 10000 or fewer (`SORT_LIMIT`). Above that they are shown in the order they were found, and the screen says so.

---

## Settings in the code

| Constant | Value | Meaning |
|----------|-------|---------|
| `CACHE_FILE` | `queryfs_index.txt` | Name of the cache file |
| `PAGE_SIZE` | 15 | Results shown per page |
| `SORT_LIMIT` | 10000 | Most results that will be bubble sorted |
| `MAX_DEPTH` | 40 | Deepest folder level the scanner enters (stops endless loops) |
| `HISTORY_SIZE` | 10 | Number of searches remembered |

---

## Test cases

| Test | Expected result |
|------|-----------------|
| First run, no cache file | Scans all drives, prints "Saved to queryfs_index.txt" |
| Second run | Prints "Loaded N items", no scanning |
| Add a new file, search for it | Not found until Rescan (option 5) is used |
| Rescan, then search again | File is found |
| Delete the cache file, run again | Scans again and creates a new file |
| Empty search text | "Please type something to search." |
| Menu choice that is not 1 to 6 | "Invalid choice" message |
| Search with no matches | "Nothing found." |
| Locked system folders | Skipped, count shown after the scan |
| Search with more than 10000 results | Shown unsorted with a note on screen |

---

## Limitations

- Drive detection works on Windows only (it checks the letters A: to Z:).
- The cache can be out of date until a rescan is done.
- Shortcuts (symlinks) are not entered, and folders deeper than 40 levels are skipped.
- File names with unusual characters may be skipped on Windows.
- Search history is kept in memory only and is lost when the program closes.
- The screen is cleared with an ANSI escape code. If the terminal prints strange characters instead of clearing, change the line in `Display::clearScreen()` to `system("cls");`.

---

## Project files

```
QueryFS.cpp        the whole program (single file)
README.md          this file
queryfs_index.txt  created by the program on first run
```
