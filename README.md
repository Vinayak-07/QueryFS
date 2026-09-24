# QueryFS

A C++17 file search engine. Indexes a directory tree once into an in-memory
singly linked list, then answers searches against that list — no re-scanning
between queries. Search modes are interchangeable polymorphic strategies
behind one abstract interface.

Built for OOP CA-4 (IICT, MGM University). STL only — no boost, no third-party
libraries. Data structure is a hand-written singly linked list, not std::list.

## Build & run

    make
    ./queryfs

Requires g++ (or any C++17 compiler). std::filesystem needs no extra flags
on g++ 9+.

## How it works

1. You give it a root path. `Indexer` walks the tree with
   `std::filesystem::recursive_directory_iterator`, skipping unreadable
   entries instead of crashing (skip_permission_denied + per-entry try/catch).
2. Every discovered file becomes a `FileNode` — the constructor auto-fills
   name, extension, full path and size from the path itself. Zero manual data
   entry.
3. Nodes are appended to a `FileList` (O(1) via a cached tail pointer).
   That list is the index. It is built ONCE per run.
4. The main menu loops. Each choice constructs the matching search strategy
   and calls it through a `SearchStrategy*` base pointer — virtual dispatch
   picks the subclass at runtime. No switch-case on strategy type.

## The classes

    FileNode        one file's metadata + next pointer (list node)
    FileList        the index: head, tail, count; owns all nodes
    SearchStrategy  abstract: virtual void search(FileList*) = 0
      SearchByName       exact / substring, case toggle
      SearchByType       filter by extension, or group+count all
      SearchByDirectory  path-prefix match, children-only or full-descendants
    Indexer         directory walk -> FileList::append, per entry

## Edge cases handled (tested)

- empty directory root        -> "indexed 0 entries", clean exit
- nonexistent root            -> error message, clean exit
- all-inaccessible root       -> skip message, clean exit
- single file as root         -> indexed as one entry
- permission-denied subdirs   -> skipped, scan continues
- symlink loops               -> std::filesystem detects them, no infinite loop
- unicode filenames           -> works (UTF-8 passthrough)
- prefix traps                -> "/a/project" does NOT match "/a/project-link"
                                 (path-boundary check in SearchByDirectory)

## OOP concept map

    FileNode / FileList          classes & objects
    private fields + getters     encapsulation
    FileNode(path) ctor          constructor (auto-fill at discovery)
    3 subclasses of strategy     inheritance
    search() via base pointer    polymorphism (virtual dispatch)
    try/catch in Indexer         exception handling
    recursive_directory_iterator STL

See docs/design.md for the frozen class diagram and the "why this and not X"
decision list (why tail pointer, why deleted copy ctor, why virtual dtor,
why friend instead of a setter, why linked list at all).

## Files

    include/  FileNode.hpp FileList.hpp FileList.tpp SearchStrategy.hpp
              SearchByName.hpp SearchByType.hpp SearchByDirectory.hpp
              Indexer.hpp
    src/      matching .cpp files + main.cpp (menu + dispatch)
    docs/     design.md (class diagram + viva Q&A), demo.md (scripted demo)
