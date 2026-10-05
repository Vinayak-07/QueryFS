#ifndef FILELIST_TPP
#define FILELIST_TPP

template <typename Visitor>
void FileList::traverse(Visitor&& visit) const {
    for (FileNode* cur = head; cur != nullptr; cur = cur->getNext()) {
        visit(cur);
    }
}

#endif
