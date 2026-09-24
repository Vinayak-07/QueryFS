#include "FileNode.hpp"
#include <filesystem>

// --- Primary constructor: auto-fill from a real filesystem path. ---
// This is the "zero manual data entry" promise: hand it a path, it fills in
// every field itself. Directories get size 0 (their size is not meaningful
// for a file-search index).
FileNode::FileNode(const std::string& path)
    : fullPath(path), size(0), next(nullptr) {
    std::filesystem::path p(path);

    name = p.filename().string();

    // extension without the leading dot; "" if the file has none
    std::string ext = p.extension().string();
    if (!ext.empty() && ext.front() == '.') ext.erase(0, 1);
    extension = ext;

    std::error_code ec;
    if (std::filesystem::is_regular_file(p, ec)) {
        auto sz = std::filesystem::file_size(p, ec);
        if (!ec) size = static_cast<std::size_t>(sz);
    }
}

// --- Secondary constructor: explicit fields (tests / stub lists). ---
FileNode::FileNode(const std::string& name,
                   const std::string& extension,
                   const std::string& fullPath,
                   std::size_t size)
    : name(name), extension(extension),
      fullPath(fullPath), size(size), next(nullptr) {}

const std::string& FileNode::getName() const { return name; }
const std::string& FileNode::getExtension() const { return extension; }
const std::string& FileNode::getFullPath() const { return fullPath; }
std::size_t FileNode::getSize() const { return size; }
FileNode* FileNode::getNext() const { return next; }
