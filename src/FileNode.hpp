#ifndef FILENODE_HPP
#define FILENODE_HPP

#include <string>

// FileNode: one node of the linked list, holding exactly one file's metadata.
// The metadata is captured once, at discovery time, by the constructor —
// nothing in the system writes to a FileNode after construction (except `next`,
// which only FileList is allowed to touch).
class FileNode {
private:
    std::string name;       // filename with extension, e.g. "report.pdf"
    std::string extension;  // extension without the dot, e.g. "pdf"; "" if none
    std::string fullPath;   // absolute path from filesystem root
    std::size_t size;       // file size in bytes (0 for directories)

    FileNode* next;         // singly linked list link. Only FileList touches this.

public:
    // Builds a fully-populated node straight from a filesystem path.
    // This is the "auto-fill / zero manual data entry" guarantee: the caller
    // hands over a path, the constructor does the rest.
    explicit FileNode(const std::string& path);

    // Manual constructor used by tests / phase-2 stub lists, where there is no
    // real file on disk behind the data.
    FileNode(const std::string& name,
             const std::string& extension,
             const std::string& fullPath,
             std::size_t size);

    // --- Read-only accessors (encapsulation: fields stay private) ---
    const std::string& getName() const;
    const std::string& getExtension() const;
    const std::string& getFullPath() const;
    std::size_t getSize() const;
    FileNode* getNext() const;

    // Only FileList may rewire the list. Friendship is deliberate and narrow:
    // it keeps `next` honest (no outside code can splice the list) while
    // avoiding a public setter that would let anyone corrupt the chain.
    friend class FileList;
};

#endif
