#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/vfs.h>

#include <cutils/properties.h>
#include <cutils/log.h>

#include <selinux/label.h>

#include "roots.h"

#include "bu.h"

#define PATHNAME_RC "/tmp/burc"

using namespace android;

struct selabel_handle *sehandle;

int sockfd;
TAR* tar;
gzFile gzf;

char* hash_name;
size_t hash_datalen;
SHA1_CTX sha1_ctx;
MD5_CTX md5_ctx;

void
ui_print(const char* format, ...) {
    char buffer[256];

    va_list ap;
    va_start(ap, format);
    vsnprintf(buffer, sizeof(buffer), format, ap);
    va_end(ap);

    fputs(buffer, stdout);
}

void logmsg(const char *fmt, ...)
{
    char msg[1024];
    FILE* fp;
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    fp = fopen("/tmp/bu.log", "a");
    if (fp) {
        fprintf(fp, "[%d] %s", getpid(), msg);
        fclose(fp);
    }
}

static int tar_cb_open(const char* path, int mode, ...)
{
    errno = EINVAL;
    return -1;
}

static int tar_cb_close(int fd)
{
    return 0;
}

static ssize_t tar_cb_read(int fd, void* buf, size_t len)
{
    ssize_t nread;
    nread = ::read(fd, buf, len);
    if (nread > 0 && hash_name) {
        SHA1Update(&sha1_ctx, (u_char*)buf, nread);
        MD5_Update(&md5_ctx, buf, nread);
        hash_datalen += nread;
    }
    return nread;
}

static ssize_t tar_cb_write(int fd, const void* buf, size_t len)
{
    ssize_t written = 0;

    if (hash_name) {
        SHA1Update(&sha1_ctx, (u_char*)buf, len);
        MD5_Update(&md5_ctx, buf, len);
        hash_datalen += len;
    }

    while (len > 0) {
        ssize_t n = ::write(fd, buf, len);
        if (n < 0) {
            logmsg("tar_cb_write: error: n=%d\n", n);
            return n;
        }
        if (n == 0)
            break;
        buf = (const char *)buf + n;
        len -= n;
        written += n;
    }
    return written;
}

static tartype_t tar_io = {
    tar_cb_open,
    tar_cb_close,
    tar_cb_read,
    tar_cb_write
};

static ssize_t tar_gz_cb_read(int fd, void* buf, size_t len)
{
    int nread;
    nread = gzread(gzf, buf, len);
    if (nread > 0 && hash_name) {
        SHA1Update(&sha1_ctx, (u_char*)buf, nread);
        MD5_Update(&md5_ctx, buf, nread);
        hash_datalen += nread;
    }
    return nread;
}

static ssize_t tar_gz_cb_write(int fd, const void* buf, size_t len)
{
    ssize_t written = 0;

    if (hash_name) {
        SHA1Update(&sha1_ctx, (u_char*)buf, len);
        MD5_Update(&md5_ctx, buf, len);
        hash_datalen += len;
    }

    while (len > 0) {
        ssize_t n = gzwrite(gzf, buf, len);
        if (n < 0) {
            logmsg("tar_gz_cb_write: error: n=%d\n", n);
            return n;
        }
        if (n == 0)
            break;
        buf = (const char *)buf + n;
        len -= n;
        written += n;
    }
    return written;
}

static tartype_t tar_io_gz = {
    tar_cb_open,
    tar_cb_close,
    tar_gz_cb_read,
    tar_gz_cb_write
};

int create_tar(const char* compress, const char* mode)
{
    int rc = -1;

    SHA1Init(&sha1_ctx);
    MD5_Init(&md5_ctx);

    if (!compress || strcasecmp(compress, "none") == 0) {
        rc = tar_fdopen(&tar, sockfd, "foobar", &tar_io,
                0, /* oflags: unused */
                0, /* mode: unused */
                TAR_GNU | TAR_STORE_SELINUX /* options */);
    }
    else if (strcasecmp(compress, "gzip") == 0) {
        gzf = gzdopen(sockfd, mode);
        if (gzf != NULL) {
            rc = tar_fdopen(&tar, 0, "foobar", &tar_io_gz,
                    0, /* oflags: unused */
                    0, /* mode: unused */
                    TAR_GNU | TAR_STORE_SELINUX /* options */);
        }
    }
    return rc;
}

static void do_exit(int rc)
{
    char rcstr[80];
    int len;
    len = sprintf(rcstr, "%d\n", rc);

    unlink(PATHNAME_RC);
    int fd = open(PATHNAME_RC, O_RDWR|O_CREAT, 0644);
    write(fd, rcstr, len);
    close(fd);
    exit(rc);
}

int main(int argc, char **argv)
{
    int n;
    int rc = 1;

    logmsg("bu: invoked with %d args\n", argc);

    if (argc < 3) {
        logmsg("Not enough args (%d)\n", argc);
        do_exit(1);
    }

    // progname sockfd args...
    int optidx = 1;
    sockfd = atoi(argv[optidx++]);
    const char* opname = argv[optidx++];

    struct selinux_opt seopts[] = {
      { SELABEL_OPT_PATH, "/file_contexts" }
    };
    sehandle = selabel_open(SELABEL_CTX_FILE, seopts, 1);

    load_volume_table();
//    vold_client_start(&v_callbacks, 1);

    if (!strcmp(opname, "backup")) {
        rc = do_backup(argc-optidx, &argv[optidx]);
    }
    else if (!strcmp(opname, "restore")) {
        rc = do_restore(argc-optidx, &argv[optidx]);
    }
    else {
        logmsg("Unknown operation %s\n", opname);
        do_exit(1);
    }

    sleep(1);
    close(sockfd);

    logmsg("bu exiting\n");

    do_exit(rc);

    return rc;
}
