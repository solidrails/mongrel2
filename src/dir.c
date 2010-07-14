#include <dir.h>
#include <fcntl.h>
#include <dbg.h>
#include <task/task.h>
#include <string.h>
#include <pattern.h>
#include <assert.h>
#include <mime.h>

struct tagbstring default_type = bsStatic ("text/plain");

const char *RESPONSE_FORMAT = "HTTP/1.1 200 OK\r\n"
    "Date: %s\r\n"
    "Content-Type: %s\r\n"
    "Content-Length: %d\r\n"
    "Last-Modified: %s\r\n"
    "ETag: %x-%x\r\n"
    "Connection: %s\r\n\r\n";

const char *RFC_822_TIME = "%a, %d %b %y %T %z";

enum {
    HOG_MAX = 1024
};

FileRecord *Dir_find_file(bstring path)
{
    FileRecord *fr = calloc(sizeof(FileRecord), 1);
    const char *p = bdata(path);

    check(fr, "Failed to make FileRecord memory.");

    // right here, if p ends with / then add index.html
    int rc = stat(p, &fr->sb);
    check(rc == 0, "File stat failed: %s", bdata(path));

    fr->fd = open(p, O_RDONLY);
    check(fr->fd >= 0, "Failed to open file but stat worked: %s", bdata(path));

    fr->loaded = time(NULL);

    fr->date = bStrfTime(RFC_822_TIME, gmtime(&fr->loaded));
    check(fr->date, "Failed to format current date.");

    fr->last_mod = bStrfTime(RFC_822_TIME, gmtime(&fr->sb.st_mtime));
    check(fr->last_mod, "Failed to format last modified time.");

    // TODO: get this from a configuration
    fr->content_type = MIME_match_ext(path, &default_type);
    check(fr->content_type, "Should always get a content type back.");

    // we own this now, not the caller
    fr->full_path = path;

    // don't let people who've received big files linger and hog the show
    const char *conn_close = fr->sb.st_size > HOG_MAX ? "close" : "keep-alive";

    fr->header = bformat(RESPONSE_FORMAT,
        bdata(fr->date),
        bdata(fr->content_type),
        fr->sb.st_size,
        bdata(fr->last_mod),
        fr->sb.st_mtime, fr->sb.st_size,
        conn_close);

    check(fr->header != NULL, "Failed to create response header.");

    return fr;

error:
    FileRecord_destroy(fr);
    return NULL;
}


int Dir_stream_file(FileRecord *file, int sock_fd)
{
    ssize_t sent = 0;
    size_t total = 0;
    off_t offset = 0;
    size_t block_size = MAX_SEND_BUFFER;

    fdwrite(sock_fd, bdata(file->header), blength(file->header));

    for(total = 0; fdwait(sock_fd, 'w') == 0 && total < file->sb.st_size; total += sent) {
        sent = Dir_send(sock_fd, file->fd, &offset, block_size);
        check(sent > 0, "Failed to sendfile on socket: %d from file %d", sock_fd, file->fd);
    }

    check(total <= file->sb.st_size, "Wrote way too much, wrote %d but size was %d", (int)total, (int)file->sb.st_size);

    return sent;

error:
    return -1;
}


Dir *Dir_create(const char *base, const char *prefix, const char *index_file)
{
    Dir *dir = calloc(sizeof(Dir), 1);
    check(dir, "Out of memory error.");

    dir->base = bfromcstr(base);
    check(blength(dir->base) < MAX_DIR_PATH, "Base direcotry is too long, must be less than %d", MAX_DIR_PATH);

    dir->prefix = bfromcstr(prefix);
    check(blength(dir->prefix) < MAX_DIR_PATH, "Prefix is too long, must be less than %d", MAX_DIR_PATH);

    dir->index_file = bfromcstr(index_file);

    return dir;
error:
    return NULL;
}



void Dir_destroy(Dir *dir)
{
    if(dir) {
        bdestroy(dir->base);
        bdestroy(dir->prefix);
        bdestroy(dir->index_file);
        bdestroy(dir->normalized_base);
        free(dir);
    }
}


void FileRecord_destroy(FileRecord *file)
{
    if(file) {
        fdclose(file->fd);
        bdestroy(file->date);
        bdestroy(file->last_mod);
        bdestroy(file->header);
        bdestroy(file->full_path);
        // file->content_type is not owned by us
        free(file);
    }
}


inline int normalize_path(bstring target)
{
    ballocmin(target, PATH_MAX);

    char *normalized = realpath((const char *)target->data, NULL);
    check(normalized, "Failed to normalize path: %s", bdata(target));

    bassigncstr(target, normalized);
    free(normalized);

    return 0;

error:
    return 1;
}

inline int Dir_lazy_normalize_base(Dir *dir)
{
    if(dir->normalized_base == NULL) {
        dir->normalized_base = bstrcpy(dir->base);
        check(normalize_path(dir->normalized_base) == 0, 
                "Failed to normalize base path: %s", bdata(dir->normalized_base));
        debug("Lazy normalized base path %s into %s", 
                bdata(dir->base), bdata(dir->normalized_base));
    }
    return 0;

error:
    return -1;
}


int Dir_serve_file(Dir *dir, bstring path, int fd)
{
    FileRecord *file = NULL;
    bstring target = NULL;

    check(Dir_lazy_normalize_base(dir) == 0, "Failed to normalize base path.");

    check(bstrncmp(path, dir->prefix, blength(dir->prefix)) == 0, 
            "Request for path %s does not start with %s prefix.", 
            bdata(path), bdata(dir->prefix));

    if(bchar(path, blength(path) - 1) == '/') {
        target = bformat("%s%s/%s",
                    bdata(dir->normalized_base),
                    path->data + blength(dir->prefix),
                    bdata(dir->index_file));
    } else {
        target = bformat("%s/%s",
                bdata(dir->normalized_base),
                path->data + blength(dir->prefix));
    }

    check(target, "Couldn't construct target path for %s", bdata(path));

    check(normalize_path(target) == 0, "Failed to normalize target path.");
   
    check(bstrncmp(target, dir->normalized_base, blength(dir->normalized_base)) == 0, 
            "Request for path %s does not start with %s base after normalizing.", 
            bdata(target), bdata(dir->base));

    debug("Looking up target: %s", bdata(target));

    // the FileRecord now owns the target
    file = Dir_find_file(target);
    check(file, "Error opening file: %s", bdata(target));

    int rc = Dir_stream_file(file, fd);
    check(rc == file->sb.st_size, "Didn't send all of the file, sent %d of %s.", rc, bdata(target));

    FileRecord_destroy(file);
    return 0;

error:
    FileRecord_destroy(file);
    return -1;
}

