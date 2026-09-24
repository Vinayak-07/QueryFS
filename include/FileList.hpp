#ifndef FILELIST_HPP
#define FILELIST_HPP

#include "FileNode.hpp"

// FileList: the in-memory index. A singly linked list of FileNodes that owns
// every node it contains (composition — the nodes are part of the list, not
// borrowed from elsewhere). One list == one directory scan; searches read it,
// they never mutate it.
class FileList {
private:
    FileNode* head;   // first node, nullptr when the list is empty
    FileNode* tail;   // last node, kept so append() is O(1) — no end-of-list walk
    int count;        // number of nodes; maintained on append, read via getCount()

public:
    FileList();
    ~FileList();

    // Disable copy/assignment: two FileLists sharing/owning the same nodes
    // would mean a double delete. We never need to copy an index — if it's
    // needed someday, it should be a deep copy written deliberately, not an
    // accidental shallow one from the compiler.
    FileList(const FileList&) = delete;
    FileList& operator=(const FileList&) = delete;

    // Creates a node for `fullPath` (constructor auto-fills metadata) and
    // links it at the tail. O(1) because tail is cached. Returns the node.
    // The list owns every node it appends; nothing else frees them.
    FileNode* append(const std::string& fullPath);

    // Builds a node from pre-fetched values (test lists / phase-2 stubs).
    FileNode* append(const std::string& name,
                     const std::string& extension,
                     const std::string& fullPath,
                     std::size_t size);

    // Walks head -> tail, calling visit(node) for each. Order = discovery order.
    template <typename Visitor>
    void traverse(Visitor&& visit) const;

    int getCount() const;
    FileNode* getHead() const;
    FileNode* getTail() const;
};

#include "FileList.tpp"
#endif
