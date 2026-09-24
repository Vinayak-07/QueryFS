QueryFS — Phase 1 Design Lock (frozen at CA-4 kickoff)
=======================================================

Class diagram
-------------

                    +---------------------+
                    |   SearchStrategy    |  <<abstract>>
                    +---------------------+
                    | + search(FileList*) |  = 0 (pure virtual)
                    | + ~SearchStrategy() |  virtual
                    +---------------------+
                              ^
            _________________|_________________
           |                  |                |
  +----------------+  +---------------+  +---------------------+
  |  SearchByName  |  |  SearchByType |  | SearchByDirectory   |
  +----------------+  +---------------+  +---------------------+
  | query          |  | oneExtension  |  | dirPath             |
  | exactMatch     |  +---------------+  | recursive           |
  | caseInsensitive|                     +---------------------+

  +---------------------+          +---------------------------+
  |       FileList      | 1      * |         FileNode          |
  | (owns all nodes)    |--------->|  (one file's metadata)    |
  +---------------------+          +---------------------------+
  | - head: FileNode*   |          | - name, extension         |
  | - tail: FileNode*   |          | - fullPath                |
  | - count: int        |          | - size: size_t            |
  +---------------------+          | - next: FileNode*         |
  | + append(path)      | O(1)     +---------------------------+
  | + append(4 fields)  |          | + FileNode(path)  autofill|
  | + traverse(visit)   |          | + FileNode(4 fields)      |
  | + getCount() : int  |          | + getName()  const&       |
  | + getHead()         |          | + getExtension() const&   |
  | + getTail()         |          | + getFullPath() const&    |
  +---------------------+          | + getSize()  size_t       |
   (friend of FileNode)            | + getNext()  FileNode*    |
                                   +---------------------------+
                                    (friend class FileList)

  Indexer (Phase 3): free function / small class using
  std::filesystem::recursive_directory_iterator; hands each discovered
  entry to FileList::append(). Not part of the inheritance tree.

Frozen signatures
-----------------
FileNode(const std::string& path);                        // auto-fill ctor
FileNode(const std::string& name, const std::string& ext,
         const std::string& fullPath, std::size_t size);  // test ctor
const std::string& getName() const;
const std::string& getExtension() const;
const std::string& getFullPath() const;
std::size_t getSize() const;
FileNode* getNext() const;

FileList();
~FileList();
FileList(const FileList&) = delete;
FileList& operator=(const FileList&) = delete;
FileNode* append(const std::string& fullPath);
FileNode* append(const std::string& name, const std::string& ext,
                 const std::string& fullPath, std::size_t size);
template <typename Visitor> void traverse(Visitor&& visit) const;
int getCount() const;
FileNode* getHead() const;
FileNode* getTail() const;

virtual void search(FileList* src) = 0;                  // SearchStrategy

SearchByName(const std::string& query,
             bool exactMatch = false,
             bool caseInsensitive = true);
SearchByType(const std::string& extension = "");
SearchByDirectory(const std::string& dirPath, bool recursive = false);

Design decisions — the "why this and not X" list
------------------------------------------------
Q: Why a hand-rolled singly linked list instead of std::vector or std::list?
A: Two reasons. First, the assignment requires it — the data structure is the
   syllabus topic (ITY23PCL201), so the list must be built, not borrowed.
   Second, it genuinely fits: the workload is "append a lot, then read
   front-to-back many times" and never "random access" or "insert in middle".
   A vector would also work, but our version exposes the pointers and node
   mechanics that the CA is testing. std::list hides all of that.

Q: Why keep a `tail` pointer?
A: Without it, append() must walk the whole list to find the end — O(n) per
   file, O(n^2) per scan. With it, append is O(1). That's the single most
   likely viva question about this class; answer is one sentence: "tail makes
   append constant time, which matters because the indexer appends once per
   file discovered."

Q: Why is `next` private in FileNode with FileList as friend?
A: Encapsulation with an escape hatch. If `next` were public, any code could
   splice nodes out of the list and break the count/ownership invariants.
   Only FileList legitimately rewires the chain, so it gets friendship. There
   is no public setNext() on purpose — a setter would re-open the hole.

Q: Why two constructors in FileNode?
A: The primary one takes a real filesystem path and auto-fills every field
   (the "zero manual data entry" requirement). The secondary one takes the
   four fields directly so Phase 2 can build a hardcoded test list before the
   indexer exists — and so tests can construct nodes without creating real
   files on disk. Same class, two construction contexts.

Q: Why traverse() as a template/visitor instead of returning the list?
A: The list must never leak ownership — returning internal pointers invites
   outsiders to mutate or delete. A visitor walks read-only: the search
   strategies see every node, but nobody gets a handle that lets them break
   the chain. (Also: getHead() exists for the rare case something legitimately
   needs a raw walk, e.g. printing.)

Q: Why is search() pure virtual and why does it take FileList*?
A: Pure virtual makes SearchStrategy impossible to instantiate — you can only
   search *by something*, never "search" in the abstract. Passing the index as
   a parameter means strategies are stateless w.r.t. the index: one strategy
   object can be pointed at any list, which is what makes them interchangeable
   from the menu with a base-class pointer.

Q: Why does search() print results instead of returning a list?
A: Scope control (no gold-plating). Every strategy currently ends in printed
   output; if CA-4 grading later wants result objects, that's a mechanical
   change (return vector<FileNode*>) — flagged, not built.

Q: Why is FileList non-copyable?
A: Shallow copy of two owning pointers = double free. We never need to copy
   an index; deleting the copy constructor turns a runtime crash into a
   compile-time error, which is the cheap way to be safe.

Q: Why does SearchByType group-and-count when no extension is given?
A: The spec says "filter by extension, group + count per extension". The
   empty-query default gives the summary view of the whole index; setting the
   extension gives the filtered view. One class, two views, no duplication.

Q: Why byte-exact (case-sensitive) directory comparison?
A: POSIX paths are case-sensitive; "matching" case-insensitively would be a
   Linux lie. Stated as a deliberate platform decision, not an oversight.

Q: Where do virtual destructors matter here?
A: The menu (Phase 2) will do `SearchStrategy* s = new SearchByName(...);`
   and later `delete s;`. Without the virtual destructor in the base, delete
   through the base pointer is undefined behaviour for the subclass part.
   `virtual ~SearchStrategy() = default;` is the one-line fix.
