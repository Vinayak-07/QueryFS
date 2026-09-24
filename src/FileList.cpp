#include "FileList.hpp"

FileList::FileList() : head(nullptr), tail(nullptr), count(0) {}

// Frees every node it owns. FileList is the single owner (composition), so
// the destructor is where the whole index is reclaimed.
FileList::~FileList() {
    FileNode* cur = head;
    while (cur != nullptr) {
        FileNode* dead = cur;
        cur = cur->getNext();
        delete dead;
    }
    head = tail = nullptr;
    count = 0;
}

// O(1) append: tail is cached, so we never walk the list.
FileNode* FileList::append(const std::string& fullPath) {
    FileNode* node = new FileNode(fullPath);
    node->next = nullptr;

    if (tail == nullptr) {          // empty list
        head = tail = node;
    } else {
        tail->next = node;          // link after current last
        tail = node;                // new node becomes the tail
    }
    ++count;
    return node;
}

// Test/stub append: node built from explicit fields.
FileNode* FileList::append(const std::string& name,
                           const std::string& extension,
                           const std::string& fullPath,
                           std::size_t size) {
    FileNode* node = new FileNode(name, extension, fullPath, size);
    node->next = nullptr;

    if (tail == nullptr) {
        head = tail = node;
    } else {
        tail->next = node;
        tail = node;
    }
    ++count;
    return node;
}

int FileList::getCount() const { return count; }
FileNode* FileList::getHead() const { return head; }
FileNode* FileList::getTail() const { return tail; }
