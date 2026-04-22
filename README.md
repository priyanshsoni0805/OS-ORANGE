# PES-VCS Lab Report  
## Building a Version Control System from Scratch  

**Name:** priyansh soni
**SRN:** PES1UG24CS344

---

## Phase 1: Object Storage Foundation  

### Overview  
Implemented content-addressable storage using SHA-256 hashing. Objects are stored with a type header (`blob <size>\0`) and sharded into subdirectories by the first 2 hex characters of their hash.  

### Key Implementation Details  
- **`object_write`**: Prepends type header, computes SHA-256, writes atomically using temp-file-then-rename pattern  
- **`object_read`**: Reads object, parses header, verifies integrity by recomputing hash  
- **Deduplication**: Identical content produces identical hashes, stored only once  

---

## Phase 2: Tree Objects  

### Overview  
Implemented tree serialization to represent directory structures. Trees contain entries with mode, hash, and filename, supporting nested paths.  

### Key Implementation Details  
- **`tree_from_index`**: Builds hierarchical tree structure from flat index  
- Handles nested paths (e.g., `src/main.c` creates `src` subtree)  
- Entries sorted deterministically for reproducible hashes  

---

## Phase 3: The Index (Staging Area)  

### Overview  
Implemented the staging area as a text-based file format. The index tracks which files are prepared for the next commit.  

### Key Implementation Details  
- **Index format**: `<mode> <hash> <mtime> <size> <path>` per line  
- **`index_load`**: Parses text file into Index struct  
- **`index_save`**: Atomic write with fsync() before rename  
- **`index_add`**: Stages file by computing blob hash and updating index  

---

## Phase 4: Commits and History  

### Overview  
Implemented commit creation and history traversal. Commits tie together trees with metadata (author, timestamp, message, parent).  

### Key Implementation Details  
- **`commit_create`**: Builds commit object, updates branch reference  
- **`commit_walk`**: Traverses parent chain to display history  
- **Reference chain**: HEAD → refs/heads/main → commit hash  

---

## Phase 5: Branching and Checkout (Analysis)  

### Q5.1: Implementing `pes checkout <branch>`  

**Files that need to change in `.pes/`:**  
1. **`.pes/HEAD`**: Update to point to the target branch reference file  
2. **Working directory files**: Must be updated to match the target branch's snapshot  

**What makes this operation complex:**  
- Synchronizing working directory with target state  
- Handling uncommitted changes safely  
- Ensuring atomic updates  
- Managing path conflicts  

---

### Q5.2: Detecting "Dirty Working Directory" Conflicts  

**Approach:**  
- Compare working directory files with index hashes  
- Compare index entries with target branch tree  
- If both differ → conflict detected  

---

### Q5.3: Detached HEAD State  

- Commits are created but not referenced by any branch  
- Become unreachable when switching branches  
- Can be recovered using commit hash or reflog  

---

## Phase 6: Garbage Collection and Space Reclamation (Analysis)  

### Q6.1: Finding Unreachable Objects  

**Algorithm:**  
- Start from all branch references  
- Traverse commits and trees  
- Mark reachable objects  
- Delete all unmarked objects  

**Efficient structure:**  
- Hash set for O(1) lookup  

---

### Q6.2: Race Condition with Concurrent GC  

**Problem:**  
GC may delete objects while a commit is being created  

**Solution (Git approach):**  
- Use grace period before deletion  
- Perform operations in phases  
- Ensure atomic updates  

---
