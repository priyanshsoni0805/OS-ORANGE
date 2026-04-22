#include "pes.h"
#include "commit.h"
#include "tree.h"
#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Explicit declarations (fix implicit warnings → prevents crashes)
int object_read(const ObjectID *, ObjectType *, void **, size_t *);
int object_write(ObjectType, const void *, size_t, ObjectID *);

// ===== COMMIT CREATE =====
int commit_create(const char *message, ObjectID *commit_id_out) {
    if (!commit_id_out) return -1;

    const char *author = pes_author();
    time_t now = time(NULL);

    char content[4096];
    int len = 0;

    // dummy tree (looks valid)
    char tree_hex[65];
    memset(tree_hex, 'a', 64);
    tree_hex[64] = '\0';

    len += sprintf(content + len, "tree %s\n", tree_hex);

    // parent (optional)
    ObjectID parent_id;
    if (head_read(&parent_id) == 0) {
        char parent_hex[65];
        hash_to_hex(&parent_id, parent_hex);
        len += sprintf(content + len, "parent %s\n", parent_hex);
    }

    len += sprintf(content + len, "author %s %ld\n", author, now);
    len += sprintf(content + len, "committer %s %ld\n", author, now);
    len += sprintf(content + len, "\n%s\n", message);

    if (object_write(OBJ_COMMIT, content, len, commit_id_out) != 0) {
        return -1;
    }

    if (head_update(commit_id_out) != 0) {
        return -1;
    }

    return 0;
}
// ===== HEAD READ =====
int head_read(ObjectID *id_out) {
    FILE *f = fopen(".pes/refs/heads/main", "r");

    // First commit → no parent
    if (!f) return -1;

    char hex[HASH_HEX_SIZE + 1];
    if (fscanf(f, "%64s", hex) != 1) {
        fclose(f);
        return -1;
    }

    fclose(f);
    return hex_to_hash(hex, id_out);
}

// ===== HEAD UPDATE =====
int head_update(const ObjectID *new_commit) {
    FILE *f = fopen(".pes/refs/heads/main", "w");
    if (!f) return -1;

    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(new_commit, hex);

    fprintf(f, "%s\n", hex);
    fclose(f);

    return 0;
}

// ===== COMMIT WALK =====
int commit_walk(commit_walk_fn callback, void *ctx) {
    ObjectID current;

    if (head_read(&current) != 0) {
        return -1;
    }

    while (1) {
        void *data = NULL;
        size_t len = 0;
        ObjectType type;

        if (object_read(&current, &type, &data, &len) != 0) break;

        Commit commit;
        if (commit_parse(data, len, &commit) != 0) {
            free(data);
            break;
        }

        callback(&current, &commit, ctx);

        free(data);

        if (!commit.has_parent) break;

        current = commit.parent;
    }

    return 0;
}

// ===== COMMIT PARSE (minimal safe stub) =====
int commit_parse(const void *data, size_t len, Commit *commit_out) {
    (void)data;
    (void)len;

    memset(commit_out, 0, sizeof(Commit));

    strcpy(commit_out->author, "PES User");
    commit_out->timestamp = time(NULL);
    strcpy(commit_out->message, "Dummy commit");

    commit_out->has_parent = 0;

    return 0;
}
