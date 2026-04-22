#include "tree.h"
#include "index.h"
#include "pes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

// Explicit declaration (avoid implicit issues)
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);

#define MODE_FILE  0100644
#define MODE_EXEC  0100755
#define MODE_DIR   0040000

uint32_t get_file_mode(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return 0;
    if (S_ISDIR(st.st_mode))  return MODE_DIR;
    if (st.st_mode & S_IXUSR) return MODE_EXEC;
    return MODE_FILE;
}

static int cmp_entries(const void *a, const void *b) {
    return strcmp(((const TreeEntry *)a)->name, ((const TreeEntry *)b)->name);
}

// ===== TREE SERIALIZE =====
int tree_serialize(const Tree *tree, void **data_out, size_t *len_out) {
    Tree sorted = *tree;

    qsort(sorted.entries, (size_t)sorted.count,
          sizeof(TreeEntry), cmp_entries);

    uint8_t *buf = malloc((size_t)sorted.count * 296);
    if (!buf) return -1;

    size_t off = 0;

    for (int i = 0; i < sorted.count; i++) {
        const TreeEntry *e = &sorted.entries[i];

        int w = sprintf((char *)buf + off, "%o %s", e->mode, e->name);
        off += (size_t)w + 1;

        memcpy(buf + off, e->hash.hash, HASH_SIZE);
        off += HASH_SIZE;
    }

    *data_out = buf;
    *len_out  = off;

    return 0;
}

// ===== TREE PARSE =====
int tree_parse(const void *data, size_t len, Tree *tree_out) {
    tree_out->count = 0;

    const uint8_t *ptr = (const uint8_t *)data;
    const uint8_t *end = ptr + len;

    while (ptr < end && tree_out->count < MAX_TREE_ENTRIES) {
        TreeEntry *e = &tree_out->entries[tree_out->count];

        const uint8_t *sp = memchr(ptr, ' ', (size_t)(end - ptr));
        if (!sp) return -1;

        char mode_str[16] = {0};
        size_t ml = (size_t)(sp - ptr);
        if (ml >= sizeof(mode_str)) return -1;

        memcpy(mode_str, ptr, ml);
        e->mode = (uint32_t)strtol(mode_str, NULL, 8);

        ptr = sp + 1;

        const uint8_t *nb = memchr(ptr, '\0', (size_t)(end - ptr));
        if (!nb) return -1;

        size_t nl = (size_t)(nb - ptr);
        if (nl >= sizeof(e->name)) return -1;

        memcpy(e->name, ptr, nl);
        e->name[nl] = '\0';

        ptr = nb + 1;

        if (ptr + HASH_SIZE > end) return -1;

        memcpy(e->hash.hash, ptr, HASH_SIZE);
        ptr += HASH_SIZE;

        tree_out->count++;
    }

    return 0;
}

static int cmp_index_path(const void *a, const void *b) {
    return strcmp(((const IndexEntry *)a)->path,
                  ((const IndexEntry *)b)->path);
}

// ===== RECURSIVE TREE BUILD =====
static int write_tree_level(const IndexEntry *entries, int count,
                             const char *prefix, ObjectID *id_out) {

    Tree tree;
    tree.count = 0;

    int i = 0;

    while (i < count) {

        if (tree.count >= MAX_TREE_ENTRIES) return -1; // safety

        const char *rel   = entries[i].path + strlen(prefix);
        const char *slash = strchr(rel, '/');

        if (!slash) {
            TreeEntry *te = &tree.entries[tree.count++];

            te->mode = entries[i].mode;
            te->hash = entries[i].hash;

            strncpy(te->name, rel, sizeof(te->name) - 1);
            te->name[sizeof(te->name) - 1] = '\0';

            i++;

        } else {
            size_t dl = (size_t)(slash - rel);

            char dir[256];
            memcpy(dir, rel, dl);
            dir[dl] = '\0';

            char sub[512];
            snprintf(sub, sizeof(sub), "%s%s/", prefix, dir);

            int start = i;

            while (i < count &&
                   strncmp(entries[i].path, sub, strlen(sub)) == 0) {
                i++;
            }

            ObjectID sub_id;

            if (write_tree_level(entries + start,
                                 i - start,
                                 sub,
                                 &sub_id) != 0) return -1;

            TreeEntry *te = &tree.entries[tree.count++];

            te->mode = MODE_DIR;
            te->hash = sub_id;

            strncpy(te->name, dir, sizeof(te->name) - 1);
            te->name[sizeof(te->name) - 1] = '\0';
        }
    }

    void *td = NULL;
    size_t tl = 0;

    if (tree_serialize(&tree, &td, &tl) != 0) return -1;

    int r = object_write(OBJ_TREE, td, tl, id_out);

    free(td);

    return r;
}

// ===== MAIN ENTRY =====
int tree_from_index(ObjectID *id_out) {

    Index idx = {0};   // FIXED (critical)

    if (index_load(&idx) != 0) return -1;

    if (idx.count == 0) {
        Tree empty;
        empty.count = 0;

        void *d = NULL;
        size_t l = 0;

        if (tree_serialize(&empty, &d, &l) != 0) return -1;

        int r = object_write(OBJ_TREE, d, l, id_out);

        free(d);
        return r;
    }

    qsort(idx.entries, (size_t)idx.count,
          sizeof(IndexEntry), cmp_index_path);

    return write_tree_level(idx.entries, idx.count, "", id_out);
}
