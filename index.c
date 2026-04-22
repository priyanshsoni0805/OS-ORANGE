// index.c — Staging area implementation
//
// Text format of .pes/index (one entry per line, sorted by path):
//
//   <mode-octal> <64-char-hex-hash> <mtime-seconds> <size> <path>
//
// Example:
//   100644 a1b2c3d4e5f6...  1699900000 42 README.md
//   100644 f7e8d9c0b1a2...  1699900100 128 src/main.c
//
// This is intentionally a simple text format. No magic numbers, no
// binary parsing. The focus is on the staging area CONCEPT (tracking
// what will go into the next commit) and ATOMIC WRITES (temp+rename).
//
// PROVIDED functions: index_find, index_remove, index_status

#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>

int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);
// ─── PROVIDED ────────────────────────────────────────────────────────────────

// Find an index entry by path (linear scan).
IndexEntry* index_find(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0)
            return &index->entries[i];
    }
    return NULL;
}

// Remove a file from the index.
// Returns 0 on success, -1 if path not in index.
int index_remove(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0) {
            int remaining = index->count - i - 1;
            if (remaining > 0)
                memmove(&index->entries[i], &index->entries[i + 1],
                        remaining * sizeof(IndexEntry));
            index->count--;
            return index_save(index);
        }
    }
    fprintf(stderr, "error: '%s' is not in the index\n", path);
    return -1;
}

// Print the status of the working directory.
//
// Identifies files that are staged, unstaged (modified/deleted in working dir),
// and untracked (present in working dir but not in index).
// Returns 0.
int index_status(const Index *index) {
    printf("Staged changes:\n");
    int staged_count = 0;
    // Note: A true Git implementation deeply diffs against the HEAD tree here. 
    // For this lab, displaying indexed files represents the staging intent.
    for (int i = 0; i < index->count; i++) {
        printf("  staged:     %s\n", index->entries[i].path);
        staged_count++;
    }
    if (staged_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    printf("Unstaged changes:\n");
    int unstaged_count = 0;
    for (int i = 0; i < index->count; i++) {
        struct stat st;
        if (stat(index->entries[i].path, &st) != 0) {
            printf("  deleted:    %s\n", index->entries[i].path);
            unstaged_count++;
        } else {
            // Fast diff: check metadata instead of re-hashing file content
            if (st.st_mtime != (time_t)index->entries[i].mtime_sec || st.st_size != (off_t)index->entries[i].size) {
                printf("  modified:   %s\n", index->entries[i].path);
                unstaged_count++;
            }
        }
    }
    if (unstaged_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    printf("Untracked files:\n");
    int untracked_count = 0;
    DIR *dir = opendir(".");
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            // Skip hidden directories, parent directories, and build artifacts
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
            if (strcmp(ent->d_name, ".pes") == 0) continue;
            if (strcmp(ent->d_name, "pes") == 0) continue; // compiled executable
            if (strstr(ent->d_name, ".o") != NULL) continue; // object files

            // Check if file is tracked in the index
            int is_tracked = 0;
            for (int i = 0; i < index->count; i++) {
                if (strcmp(index->entries[i].path, ent->d_name) == 0) {
                    is_tracked = 1; 
                    break;
                }
            }
            
            if (!is_tracked) {
                struct stat st;
                stat(ent->d_name, &st);
                if (S_ISREG(st.st_mode)) { // Only list regular files for simplicity
                    printf("  untracked:  %s\n", ent->d_name);
                    untracked_count++;
                }
            }
        }
        closedir(dir);
    }
    if (untracked_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    return 0;
}

int index_load(Index *index) {
    index->count = 0;
 
    FILE *f = fopen(INDEX_FILE, "r");
    if (!f) {
        // file not existing just means no entries staged yet
        return 0;
    }
 
    char hex[HASH_HEX_SIZE + 1];
 
    while (index->count < MAX_INDEX_ENTRIES) {
        IndexEntry *e = &index->entries[index->count];
 
        // parse one line: mode  hex-hash  mtime  size  path
        int n = fscanf(f, "%o %64s %llu %u %511s",
                       &e->mode,
                       hex,
                       (unsigned long long *)&e->mtime_sec,
                       &e->size,
                       e->path);
 
        if (n == EOF || n < 5) break;  // done or malformed — stop either way
 
        // convert the 64-char hex string back to the 32-byte binary ObjectID
        if (hex_to_hash(hex, &e->hash) != 0) {
            fclose(f);
            return -1;
        }
 
        index->count++;
    }
 
    fclose(f);

    return 0;
}

static int compare_entries_by_path(const void *a, const void *b) {
    return strcmp(((const IndexEntry *)a)->path,
                  ((const IndexEntry *)b)->path);
}
 
// Save the index atomically: write to a temp file, fsync, then rename
// over the real index so a crash never leaves a half-written file.
int index_save(const Index *index) {
    // build temp path alongside the real index file
    char tmp_path[512];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp_XXXXXX", INDEX_FILE);
 
    int fd = mkstemp(tmp_path);
    if (fd < 0) return -1;
 
    FILE *f = fdopen(fd, "w");
    if (!f) { close(fd); unlink(tmp_path); return -1; }
 
    // sort a mutable copy so we don't reorder the caller's in-memory index
Index *sorted = malloc(sizeof(Index));
if (!sorted) {
    fclose(f);
    unlink(tmp_path);
    return -1;
}

*sorted = *index;

qsort(sorted->entries, (size_t)sorted->count,
      sizeof(IndexEntry), compare_entries_by_path);
 
    char hex[HASH_HEX_SIZE + 1];
    for (int i = 0; i < sorted->count; i++) {
    const IndexEntry *e = &sorted->entries[i];
        hash_to_hex(&e->hash, hex);
        fprintf(f, "%o %s %llu %u %s\n",
                e->mode,
                hex,
                (unsigned long long)e->mtime_sec,
                e->size,
                e->path);
    }
 
    // flush stdio buffer → kernel buffer → disk before rename
    fflush(f);
    fsync(fileno(f));
    fclose(f);
 
    // atomic replace: no reader ever sees a partial file
    if (rename(tmp_path, INDEX_FILE) != 0) {
        unlink(tmp_path);
        return -1;
    }
    free(sorted);
    return 0;
}
int index_add(Index *index, const char *path) {
    // open and measure the file
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "error: cannot open '%s'\n", path);
        return -1;
    }
 
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (file_size < 0) { fclose(f); return -1; }
 
    // read the entire file into a heap buffer
    uint8_t *buf = malloc((size_t)file_size);
    if (!buf) { fclose(f); return -1; }
 
    size_t bytes_read = fread(buf, 1, (size_t)file_size, f);
    fclose(f);
    if (bytes_read != (size_t)file_size) { free(buf); return -1; }
ObjectID blob_id;
    if (object_write(OBJ_BLOB, buf, bytes_read, &blob_id) != 0) {
        free(buf);
        return -1;
    }
    free(buf);
 
    // get fresh metadata for mtime and size (use lstat to handle symlinks)
    struct stat st;
    if (lstat(path, &st) != 0) return -1;
 
    // upsert: update existing entry if the file is already staged,
    // otherwise append a new entry at the end
    IndexEntry *entry = index_find(index, path);
    if (!entry) {
        if (index->count >= MAX_INDEX_ENTRIES) {
            fprintf(stderr, "error: index is full\n");
            return -1;
        }
        entry = &index->entries[index->count++];
    }
 
    entry->hash      = blob_id;
    entry->mtime_sec = (uint64_t)st.st_mtime;
    entry->size      = (uint32_t)st.st_size;
    entry->mode      = S_ISREG(st.st_mode)
                         ? ((st.st_mode & S_IXUSR) ? 0100755 : 0100644)
                         : 0100644;
    strncpy(entry->path, path, sizeof(entry->path) - 1);
    entry->path[sizeof(entry->path) - 1] = '\0';
 return index_save(index);
}
