QueryFS — viva demo script (walk through exactly this)
======================================================

Setup (do this before the viva):

    mkdir -p /tmp/demo/project/src /tmp/demo/project/docs
    echo 'int main(){}'  > /tmp/demo/project/src/main.cpp
    echo 'helper'        > /tmp/demo/project/src/util.cpp
    echo '# readme'      > /tmp/demo/project/docs/readme.md
    echo 'notes here'    > /tmp/demo/project/notes.txt

Build:

    make
    ./queryfs

Demo flow — 5 steps, ~2 minutes:

STEP 1 — Index once
    Root path: /tmp/demo
    Console:   Indexer: indexed 7 entries under "/tmp/demo"
    Say: "The whole tree was walked once and every file went into the linked
    list. From here on we never touch the disk again — that's the speed
    argument of the project."

STEP 2 — Search by Type, group mode
    Choice 2, press Enter (empty = group everything).
    Shows the per-extension tally: cpp:2, md:1, txt:1, (no extension):2
    Say: "One traversal of the list, a map counts extensions as we go."

STEP 3 — Search by Name, case-insensitive substring
    Choice 1, query "MAIN", exact=0, case-insensitive=1.
    Finds src/main.cpp even though we typed MAIN in caps.
    Say: "The strategy lowercases both sides before comparing — the flag
    decides whether that happens."

STEP 4 — Search by Directory, both modes
    Choice 3, path /tmp/demo/project, recursive=1  -> full descendants (6 entries)
    Choice 3 again, recursive=0                    -> immediate children (3)
    Say: "Same interface, one boolean flips the mode. Non-recursive checks
    that the remaining path segment has no further slash."

STEP 5 — Exit
    Choice 4. Clean shutdown, list destroyed by the FileList destructor.
    Say: "One index, many queries, zero re-scans."

If she digs deeper, the four killer questions are in docs/design.md:
tail pointer (O(1) append), deleted copy ctor (double-free guard),
virtual destructor (delete-through-base safety), friend instead of setter
(encapsulation with a narrow escape hatch).

And the exception-handling answer: recursive_directory_iterator throws on
unreadable directories; one locked folder would kill the whole scan, so the
walk uses skip_permission_denied plus a try/catch per entry — skip and keep
going, never crash.
