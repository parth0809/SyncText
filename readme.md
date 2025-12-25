# SyncText Collaborative Editor

**SyncText** is a lightweight terminal-based collaborative text editor that allows multiple users to edit a shared document in real-time. It uses **POSIX shared memory**, **FIFO pipes**, and **CRDT-inspired last-writer-wins (LWW) merge logic** to synchronize changes between users.

---

## Features

- **Multi-user support:** Up to 5 concurrent users.
- **Real-time updates:** Changes are synchronized automatically.
- **Conflict resolution:** Uses Last-Writer-Wins CRDT approach for line-level updates.
- **Terminal UI:** Displays the document, active users, and recent changes.
- **Local file persistence:** Each user edits their own file that is kept in sync.

---

## Requirements

- Linux or POSIX-compliant system.
- C++17 compiler (tested with `g++`).
- POSIX libraries: `pthread` and `rt` (`librt`).

---

## Build Instructions


make clean
make
