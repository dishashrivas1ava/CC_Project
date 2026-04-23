#define FUSE_USE_VERSION 31
#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <limits.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>

struct mini_unionfs_state {
    char *lower_dir;
    char *upper_dir;
};

#define UNIONFS_DATA ((struct mini_unionfs_state *) fuse_get_context()->private_data)

// --- Helper Functions ---

static void get_upper_path(char *fpath, const char *path) {
    snprintf(fpath, PATH_MAX, "%s%s", UNIONFS_DATA->upper_dir, path);
}

static void get_lower_path(char *fpath, const char *path) {
    snprintf(fpath, PATH_MAX, "%s%s", UNIONFS_DATA->lower_dir, path);
}

static void get_whiteout_path(char *wh_path, const char *path) {
    char path_copy[PATH_MAX];
    strncpy(path_copy, path, PATH_MAX);
    
    char *base = strrchr(path_copy, '/');
    if (base) {
        *base = '\0'; // Split directory from filename
        char *filename = (char *)path + (base - path_copy) + 1;
        snprintf(wh_path, PATH_MAX, "%s%s/.wh.%s", UNIONFS_DATA->upper_dir, path_copy, filename);
    } else {
        snprintf(wh_path, PATH_MAX, "%s/.wh.%s", UNIONFS_DATA->upper_dir, path);
    }
}

static int is_whiteouted(const char *path) {
    char wh_path[PATH_MAX];
    get_whiteout_path(wh_path, path);
    return (access(wh_path, F_OK) == 0);
}

static int copy_on_write(const char *path) {
    char l_path[PATH_MAX], u_path[PATH_MAX];
    get_lower_path(l_path, path);
    get_upper_path(u_path, path);

    int src = open(l_path, O_RDONLY);
    if (src < 0) return -errno;

    int dst = open(u_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (dst < 0) {
        close(src);
        return -errno;
    }

    char buf[4096];
    ssize_t n;
    while ((n = read(src, buf, sizeof(buf))) > 0) {
        if (write(dst, buf, n) != n) {
            close(src);
            close(dst);
            return -EIO;
        }
    }

    close(src);
    close(dst);
    return 0;
}

// --- FUSE Operations ---

static int unionfs_getattr(const char *path, struct stat *stbuf, struct fuse_file_info *fi) {
    char fpath[PATH_MAX];

    if (is_whiteouted(path)) return -ENOENT;

    get_upper_path(fpath, path);
    if (lstat(fpath, stbuf) == 0) return 0;

    get_lower_path(fpath, path);
    if (lstat(fpath, stbuf) == 0) return 0;

    return -ENOENT;
}

static int unionfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi, enum fuse_readdir_flags flags) {
    char upath[PATH_MAX], lpath[PATH_MAX];
    get_upper_path(upath, path);
    get_lower_path(lpath, path);

    filler(buf, ".", NULL, 0, 0);
    filler(buf, "..", NULL, 0, 0);

    // Scan Upper
    DIR *dp = opendir(upath);
    if (dp) {
        struct dirent *de;
        while ((de = readdir(dp)) != NULL) {
            if (strncmp(de->d_name, ".wh.", 4) == 0) continue;
            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
            filler(buf, de->d_name, NULL, 0, 0);
        }
        closedir(dp);
    }

    // Scan Lower
    dp = opendir(lpath);
    if (dp) {
        struct dirent *de;
        while ((de = readdir(dp)) != NULL) {
            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
            
            char wh_check[PATH_MAX];
            snprintf(wh_check, sizeof(wh_check), "%s/.wh.%s", upath, de->d_name);
            char up_check[PATH_MAX];
            snprintf(up_check, sizeof(up_check), "%s/%s", upath, de->d_name);

            // Only show if it's NOT in upper and NOT whiteouted
            if (access(wh_check, F_OK) != 0 && access(up_check, F_OK) != 0) {
                filler(buf, de->d_name, NULL, 0, 0);
            }
        }
        closedir(dp);
    }
    return 0;
}

static int unionfs_open(const char *path, struct fuse_file_info *fi) {
    char upath[PATH_MAX];
    get_upper_path(upath, path);

    if (is_whiteouted(path)) return -ENOENT;

    // Trigger CoW if writing to a file that only exists in lower
    if ((fi->flags & (O_WRONLY | O_RDWR)) && (access(upath, F_OK) != 0)) {
        int res = copy_on_write(path);
        if (res < 0) return res;
    }

    return 0;
}

static int unionfs_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
    char fpath[PATH_MAX];
    get_upper_path(fpath, path);
    if (access(fpath, F_OK) != 0) get_lower_path(fpath, path);

    int fd = open(fpath, O_RDONLY);
    if (fd == -1) return -errno;
    int res = pread(fd, buf, size, offset);
    if (res == -1) res = -errno;
    close(fd);
    return res;
}

static int unionfs_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
    char fpath[PATH_MAX];
    get_upper_path(fpath, path);

    int fd = open(fpath, O_WRONLY);
    if (fd == -1) return -errno;
    int res = pwrite(fd, buf, size, offset);
    if (res == -1) res = -errno;
    close(fd);
    return res;
}

static int unionfs_unlink(const char *path) {
    char upath[PATH_MAX], wh_path[PATH_MAX];
    get_upper_path(upath, path);
    get_whiteout_path(wh_path, path);

    // If it's in the upper layer, delete it physically
    if (access(upath, F_OK) == 0) {
        unlink(upath);
    }
    
    // Create whiteout to hide it from the lower layer
    int fd = open(wh_path, O_CREAT | O_WRONLY, 0644);
    if (fd >= 0) close(fd);

    return 0;
}

static struct fuse_operations unionfs_oper = {
    .getattr = unionfs_getattr,
    .readdir = unionfs_readdir,
    .open    = unionfs_open,
    .read    = unionfs_read,
    .write   = unionfs_write,
    .unlink  = unionfs_unlink,
};

int main(int argc, char *argv[]) {
    if (argc < 4) {
        fprintf(stderr, "Usage: %s <lower> <upper> <mountpoint>\n", argv[0]);
        return 1;
    }
    struct mini_unionfs_state *state = malloc(sizeof(struct mini_unionfs_state));
    state->lower_dir = realpath(argv[1], NULL);
    state->upper_dir = realpath(argv[2], NULL);

    if (!state->lower_dir || !state->upper_dir) {
        perror("realpath");
        return 1;
    }

    char *fuse_argv[argc];
    fuse_argv[0] = argv[0];
    fuse_argv[1] = argv[3]; // The mountpoint
    for (int i = 4; i < argc; i++) fuse_argv[i - 2] = argv[i];

    return fuse_main(argc - 2, fuse_argv, &unionfs_oper, state);
}
