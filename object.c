// object.c — Content-addressable object store
//
// Every piece of data (file contents, directory listings, commits) is stored
// as an "object" named by its SHA-256 hash. Objects are stored under
// .pes/objects/XX/YYYYYY... where XX is the first two hex characters of the
// hash (directory sharding).
//
// PROVIDED functions: compute_hash, object_path, object_exists, hash_to_hex, hex_to_hash
// TODO functions:     object_write, object_read

#include "pes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <openssl/evp.h>

// ─── PROVIDED ────────────────────────────────────────────────────────────────

void hash_to_hex(const ObjectID *id, char *hex_out) {
    for (int i = 0; i < HASH_SIZE; i++) {
        sprintf(hex_out + i * 2, "%02x", id->hash[i]);
    }
    hex_out[HASH_HEX_SIZE] = '\0';
}

int hex_to_hash(const char *hex, ObjectID *id_out) {
    if (strlen(hex) < HASH_HEX_SIZE) return -1;
    for (int i = 0; i < HASH_SIZE; i++) {
        unsigned int byte;
        if (sscanf(hex + i * 2, "%2x", &byte) != 1) return -1;
        id_out->hash[i] = (uint8_t)byte;
    }
    return 0;
}

void compute_hash(const void *data, size_t len, ObjectID *id_out) {
    unsigned int hash_len;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
    EVP_DigestUpdate(ctx, data, len);
    EVP_DigestFinal_ex(ctx, id_out->hash, &hash_len);
    EVP_MD_CTX_free(ctx);
}

// Get the filesystem path where an object should be stored.
// Format: .pes/objects/XX/YYYYYYYY...
// The first 2 hex chars form the shard directory; the rest is the filename.
void object_path(const ObjectID *id, char *path_out, size_t path_size) {
    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(id, hex);
    snprintf(path_out, path_size, "%s/%.2s/%s", OBJECTS_DIR, hex, hex + 2);
}

int object_exists(const ObjectID *id) {
    char path[512];
    object_path(id, path, sizeof(path));
    return access(path, F_OK) == 0;
}

static const char *object_type_string(ObjectType type) {
    switch (type) {
        case OBJ_BLOB:   return "blob";
        case OBJ_TREE:   return "tree";
        case OBJ_COMMIT: return "commit";
        default:         return "unknown";
    }
}
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out) {
    // --- Commit 2: Build header, assemble full object buffer, compute hash ---
 
    const char *type_str = object_type_string(type);
 
    // Build the header: "<type> <size>\0"
    char header[64];
    int header_len = snprintf(header, sizeof(header), "%s %zu", type_str, len) + 1;
    // +1 to include the '\0' terminator that snprintf doesn't count
 
    // Allocate buffer for header + data
    size_t total_len = (size_t)header_len + len;
    uint8_t *full_obj = malloc(total_len);
    if (!full_obj) return -1;
 
    memcpy(full_obj, header, (size_t)header_len);
    memcpy(full_obj + header_len, data, len);
 
    // Compute SHA-256 of the entire object (header + data)
    ObjectID id;
    compute_hash(full_obj, total_len, &id);
    if (object_exists(&id)) {
        *id_out = id;
        free(full_obj);
        return 0;
    }
 
    // Build shard directory path: .pes/objects/XX/
    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(&id, hex);
 
    char shard_dir[512];
    snprintf(shard_dir, sizeof(shard_dir), "%s/%.2s", OBJECTS_DIR, hex);
 
    // Create shard directory if it doesn't exist (ignore EEXIST)
    mkdir(shard_dir, 0755);
 
    // Build final object path
    char final_path[512];
    object_path(&id, final_path, sizeof(final_path));
 
    // Write to a temp file in the same shard directory (so rename is atomic)
    char tmp_path[512];
    snprintf(tmp_path, sizeof(tmp_path), "%s/tmp_XXXXXX", shard_dir);
 
    int fd = mkstemp(tmp_path);
    if (fd < 0) {
        free(full_obj);
        return -1;
    }
 
    // Write the full object contents
    ssize_t written = write(fd, full_obj, total_len);
    free(full_obj);
    if (written < 0 || (size_t)written != total_len) {
        close(fd);
        unlink(tmp_path);
        return -1;
    }
 
    // fsync the temp file to ensure data reaches disk before the rename
    if (fsync(fd) != 0) {
        close(fd);
        unlink(tmp_path);
        return -1;
    }
    close(fd);
 
    // Atomically rename temp file to the final path
    if (rename(tmp_path, final_path) != 0) {
        unlink(tmp_path);
        return -1;
    }
 
    // fsync the shard directory to persist the directory entry for the rename
    int dir_fd = open(shard_dir, O_RDONLY);
    if (dir_fd >= 0) {
        fsync(dir_fd);
        close(dir_fd);
    }
 
    // Store the computed hash in the output parameter
    *id_out = id;
    return 0;
}

int object_read(const ObjectID *id, ObjectType *type_out, void **data_out, size_t *len_out) {
    
 
    char path[512];
    object_path(id, path, sizeof(path));
 
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
 
    // Determine file size
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
 
    if (file_size <= 0) {
        fclose(f);
        return -1;
    }
 
    // Read entire file into memory
    uint8_t *buf = malloc((size_t)file_size);
    if (!buf) {
        fclose(f);
        return -1;
    }
 
    size_t bytes_read = fread(buf, 1, (size_t)file_size, f);
    fclose(f);
 
    if (bytes_read != (size_t)file_size) {
        free(buf);
        return -1;
    }
 
    // Integrity check: recompute the hash and compare with the expected id
    ObjectID computed;
    compute_hash(buf, bytes_read, &computed);
    if (memcmp(computed.hash, id->hash, HASH_SIZE) != 0) {
        free(buf);
        return -1;  // Corrupted object
    }


uint8_t *null_ptr = memchr(buf, '\0', bytes_read);
    if (!null_ptr) {
        free(buf);
        return -1;
    }
 
    // The header is everything before '\0'
    char *header = (char *)buf;
    size_t header_len = (size_t)(null_ptr - buf);  // length without '\0'
 
    // Parse the type string (everything before the first space)
    // Format: "<type> <size>"
    char *space = memchr(header, ' ', header_len);
    if (!space) {
        free(buf);
        return -1;
    }
 
    // Compare type prefix
    size_t type_len = (size_t)(space - header);
    ObjectType parsed_type;
    if (strncmp(header, "blob", type_len) == 0 && type_len == 4) {
        parsed_type = OBJ_BLOB;
    } else if (strncmp(header, "tree", type_len) == 0 && type_len == 4) {
        parsed_type = OBJ_TREE;
    } else if (strncmp(header, "commit", type_len) == 0 && type_len == 6) {
        parsed_type = OBJ_COMMIT;
    } else {
        free(buf);
        return -1;
    }
 
    // Parse the declared size from header (after the space)
    size_t declared_size = (size_t)atol(space + 1);
 
    // The data payload starts immediately after the '\0'
    uint8_t *data_start = null_ptr + 1;
    size_t data_len = bytes_read - (header_len + 1);  // total - header - '\0'
 
    // Sanity check: declared size must match actual data length
    if (declared_size != data_len) {
        free(buf);
        return -1;
    }
 
    // Allocate output buffer and copy data payload
    void *out = malloc(data_len);
    if (!out) {
        free(buf);
        return -1;
    }
    memcpy(out, data_start, data_len);
 
    free(buf);
 
    // Set all output parameters
    *type_out = parsed_type;
    *data_out = out;
    *len_out  = data_len;
 
    return 0;
}
