#include <tree_sitter/api.h>

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

extern const TSLanguage *tree_sitter_typescript(void);

typedef struct {
    char **items;
    size_t count;
    size_t capacity;
} FileList;

typedef enum {
    PARSER_MODE_REUSE,
    PARSER_MODE_PER_FILE,
} ParserMode;

static void *sys_malloc(size_t size) { return malloc(size); }
static void *sys_calloc(size_t count, size_t size) { return calloc(count, size); }
static void *sys_realloc(void *ptr, size_t size) { return realloc(ptr, size); }
static void sys_free(void *ptr) { free(ptr); }

static void free_file_list(FileList *files) {
    if (!files) return;
    for (size_t i = 0; i < files->count; ++i) {
        free(files->items[i]);
    }
    free(files->items);
    files->items = NULL;
    files->count = 0;
    files->capacity = 0;
}

static int push_file(FileList *files, const char *path) {
    if (files->count == files->capacity) {
        size_t next_capacity = files->capacity == 0 ? 128 : files->capacity * 2;
        char **next_items = realloc(files->items, next_capacity * sizeof(*next_items));
        if (!next_items) {
            fprintf(stderr, "Failed to grow file list to %zu entries\n", next_capacity);
            return -1;
        }
        files->items = next_items;
        files->capacity = next_capacity;
    }

    size_t len = strlen(path);
    char *copy = malloc(len + 1);
    if (!copy) {
        fprintf(stderr, "Failed to duplicate path: %s\n", path);
        return -1;
    }
    memcpy(copy, path, len + 1);
    files->items[files->count++] = copy;
    return 0;
}

static bool has_typescript_extension(const char *name) {
    const char *ext = strrchr(name, '.');
    if (!ext) return false;
    return strcmp(ext, ".ts") == 0 ||
           strcmp(ext, ".tsx") == 0 ||
           strcmp(ext, ".mts") == 0 ||
           strcmp(ext, ".cts") == 0;
}

static int collect_typescript_files(const char *dir_path, FileList *files) {
    DIR *dir = opendir(dir_path);
    if (!dir) {
        fprintf(stderr, "opendir failed for %s: %s\n", dir_path, strerror(errno));
        return -1;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        size_t dir_len = strlen(dir_path);
        size_t name_len = strlen(entry->d_name);
        size_t full_len = dir_len + 1 + name_len;
        char *full_path = malloc(full_len + 1);
        if (!full_path) {
            fprintf(stderr, "Failed to allocate path buffer for %s/%s\n", dir_path, entry->d_name);
            closedir(dir);
            return -1;
        }

        memcpy(full_path, dir_path, dir_len);
        full_path[dir_len] = '/';
        memcpy(full_path + dir_len + 1, entry->d_name, name_len + 1);

        struct stat st;
        if (lstat(full_path, &st) != 0) {
            fprintf(stderr, "lstat failed for %s: %s\n", full_path, strerror(errno));
            free(full_path);
            closedir(dir);
            return -1;
        }

        if (S_ISDIR(st.st_mode)) {
            if (collect_typescript_files(full_path, files) != 0) {
                free(full_path);
                closedir(dir);
                return -1;
            }
        } else if (S_ISREG(st.st_mode) && has_typescript_extension(entry->d_name)) {
            if (push_file(files, full_path) != 0) {
                free(full_path);
                closedir(dir);
                return -1;
            }
        }

        free(full_path);
    }

    closedir(dir);
    return 0;
}

static int compare_paths(const void *lhs, const void *rhs) {
    const char *const *left = lhs;
    const char *const *right = rhs;
    return strcmp(*left, *right);
}

static char *read_file(const char *path, size_t *len) {
    FILE *file = fopen(path, "rb");
    if (!file) {
        fprintf(stderr, "fopen failed for %s: %s\n", path, strerror(errno));
        return NULL;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fprintf(stderr, "fseek end failed for %s\n", path);
        fclose(file);
        return NULL;
    }

    long file_size = ftell(file);
    if (file_size < 0) {
        fprintf(stderr, "ftell failed for %s\n", path);
        fclose(file);
        return NULL;
    }

    if (fseek(file, 0, SEEK_SET) != 0) {
        fprintf(stderr, "fseek start failed for %s\n", path);
        fclose(file);
        return NULL;
    }

    char *buffer = malloc((size_t)file_size + 1);
    if (!buffer) {
        fprintf(stderr, "malloc failed for %s (%ld bytes)\n", path, file_size);
        fclose(file);
        return NULL;
    }

    size_t bytes_read = fread(buffer, 1, (size_t)file_size, file);
    if (bytes_read != (size_t)file_size) {
        fprintf(stderr, "fread short read for %s (%zu/%ld)\n", path, bytes_read, file_size);
        free(buffer);
        fclose(file);
        return NULL;
    }

    buffer[file_size] = '\0';
    *len = (size_t)file_size;
    fclose(file);
    return buffer;
}

static int parse_files(const FileList *files, ParserMode mode) {
    const char *mode_name = mode == PARSER_MODE_REUSE ? "reuse" : "per-file";
    const TSLanguage *language = tree_sitter_typescript();
    if (!language) {
        fprintf(stderr, "tree_sitter_typescript() returned NULL\n");
        return 1;
    }

    TSParser *shared_parser = NULL;
    if (mode == PARSER_MODE_REUSE) {
        shared_parser = ts_parser_new();
        if (!shared_parser) {
            fprintf(stderr, "[%s] ts_parser_new() failed\n", mode_name);
            return 1;
        }
        if (!ts_parser_set_language(shared_parser, language)) {
            fprintf(stderr, "[%s] ts_parser_set_language() failed\n", mode_name);
            ts_parser_delete(shared_parser);
            return 1;
        }
    }

    printf("[%s] parsing %zu files\n", mode_name, files->count);
    fflush(stdout);

    for (size_t i = 0; i < files->count; ++i) {
        const char *path = files->items[i];
        size_t len = 0;
        char *source = read_file(path, &len);
        if (!source) {
            if (shared_parser) ts_parser_delete(shared_parser);
            return 1;
        }

        TSParser *parser = shared_parser;
        if (mode == PARSER_MODE_PER_FILE) {
            parser = ts_parser_new();
            if (!parser) {
                fprintf(stderr, "[%s] ts_parser_new() failed at #%zu: %s\n", mode_name, i + 1, path);
                free(source);
                return 1;
            }
            if (!ts_parser_set_language(parser, language)) {
                fprintf(stderr, "[%s] ts_parser_set_language() failed at #%zu: %s\n", mode_name, i + 1, path);
                ts_parser_delete(parser);
                free(source);
                return 1;
            }
        }

        printf("[%s] #%zu/%zu %s\n", mode_name, i + 1, files->count, path);
        fflush(stdout);

        TSTree *tree = ts_parser_parse_string(parser, NULL, source, (uint32_t)len);
        if (!tree) {
            fprintf(stderr, "[%s] ts_parser_parse_string() returned NULL at #%zu: %s\n", mode_name, i + 1, path);
            if (mode == PARSER_MODE_PER_FILE) ts_parser_delete(parser);
            free(source);
            if (shared_parser) ts_parser_delete(shared_parser);
            return 1;
        }

        TSNode root = ts_tree_root_node(tree);
        if (ts_node_is_null(root)) {
            fprintf(stderr, "[%s] root node is NULL at #%zu: %s\n", mode_name, i + 1, path);
            ts_tree_delete(tree);
            if (mode == PARSER_MODE_PER_FILE) ts_parser_delete(parser);
            free(source);
            if (shared_parser) ts_parser_delete(shared_parser);
            return 1;
        }

        ts_tree_delete(tree);
        if (mode == PARSER_MODE_PER_FILE) {
            ts_parser_delete(parser);
        }
        free(source);
    }

    if (shared_parser) {
        ts_parser_delete(shared_parser);
    }

    printf("[%s] completed successfully\n", mode_name);
    fflush(stdout);
    return 0;
}

static void print_usage(const char *argv0) {
    fprintf(stderr,
            "Usage: %s <directory> [--mode=reuse|per-file|both]\n"
            "  reuse    Reuse one TSParser across all files.\n"
            "  per-file Create and delete one TSParser per file.\n"
            "  both     Run reuse, then per-file.\n",
            argv0);
}

int main(int argc, char **argv) {
    if (argc < 2 || argc > 3) {
        print_usage(argv[0]);
        return 2;
    }

    const char *directory = argv[1];
    const char *mode_arg = argc == 3 ? argv[2] : "--mode=both";
    bool run_reuse = false;
    bool run_per_file = false;

    if (strcmp(mode_arg, "--mode=reuse") == 0) {
        run_reuse = true;
    } else if (strcmp(mode_arg, "--mode=per-file") == 0) {
        run_per_file = true;
    } else if (strcmp(mode_arg, "--mode=both") == 0) {
        run_reuse = true;
        run_per_file = true;
    } else {
        print_usage(argv[0]);
        return 2;
    }

    struct stat st;
    if (stat(directory, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "Input path is not a readable directory: %s\n", directory);
        return 2;
    }

    ts_set_allocator(sys_malloc, sys_calloc, sys_realloc, sys_free);

    FileList files = {0};
    if (collect_typescript_files(directory, &files) != 0) {
        free_file_list(&files);
        return 1;
    }

    qsort(files.items, files.count, sizeof(*files.items), compare_paths);

    printf("Collected %zu TypeScript files under %s\n", files.count, directory);
    fflush(stdout);

    int status = 0;
    if (run_reuse) {
        status = parse_files(&files, PARSER_MODE_REUSE);
    }
    if (status == 0 && run_per_file) {
        status = parse_files(&files, PARSER_MODE_PER_FILE);
    }

    free_file_list(&files);
    return status;
}
