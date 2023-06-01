#include "wasi_impl.h"
#include "fastinterp/wasm_memory_ptr.h"

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wreserved-id-macro"
#pragma clang diagnostic ignored "-Wc11-extensions"

#include "wasi_core.h"

#pragma clang diagnostic pop

#include <sys/uio.h>

template<typename T>
using WasmMemPtr = PochiVM::WasmMemPtr<T>;

namespace
{

template<typename T, uint32_t ord>
T WasiGetArg(uintptr_t params)
{
    static_assert(!std::is_pointer<T>::value);
    return *reinterpret_cast<T*>(params + ord * 8 + 8);
}

template<typename T, uint32_t ord>
WasmMemPtr<T> WasiGetMemPtrArg(uintptr_t params)
{
    uint32_t arg = WasiGetArg<uint32_t, ord>(params);
    return reinterpret_cast<WasmMemPtr<T>>(static_cast<uint64_t>(arg));
}

}   // anonymous namespace

struct Preopen
{
    constexpr Preopen(int fd, const char* path, const char* realPath)
        : m_fd(fd)
        , m_pathLen(static_cast<uint32_t>(__builtin_strlen(path)))
        , m_realPathLen(static_cast<uint32_t>(__builtin_strlen(realPath)))
        , m_path(path)
        , m_realPath(realPath)
    { }

    int m_fd;
    uint32_t m_pathLen;
    uint32_t m_realPathLen;
    const char* m_path;
    const char* m_realPath;
};

// For now just replicating the logic of wasm3 (https://github.com/wasm3/wasm3)
// TODO: figure out what actually the logic should be later
//

constexpr uint32_t x_preopen_count = 5;

constexpr Preopen x_preopen[x_preopen_count] = {
    {  0, "<stdin>" , ""   },
    {  1, "<stdout>", ""   },
    {  2, "<stderr>", ""   },
    { -1, "./"      , "./" },
    { -1, "/"       , "./" },
};

static __wasi_errno_t linux_errno_to_wasi(int errnum)
{
    printf("!!! operation hit error number %d (%s)\n", errnum, strerror(errnum));
    switch (errnum)
    {
    case EPERM:   return __WASI_ERRNO_PERM;
    case ENOENT:  return __WASI_ERRNO_NOENT;
    case ESRCH:   return __WASI_ERRNO_SRCH;
    case EINTR:   return __WASI_ERRNO_INTR;
    case EIO:     return __WASI_ERRNO_IO;
    case ENXIO:   return __WASI_ERRNO_NXIO;
    case E2BIG:   return __WASI_ERRNO_2BIG;
    case ENOEXEC: return __WASI_ERRNO_NOEXEC;
    case EBADF:   return __WASI_ERRNO_BADF;
    case ECHILD:  return __WASI_ERRNO_CHILD;
    case EAGAIN:  return __WASI_ERRNO_AGAIN;
    case ENOMEM:  return __WASI_ERRNO_NOMEM;
    case EACCES:  return __WASI_ERRNO_ACCES;
    case EFAULT:  return __WASI_ERRNO_FAULT;
    case EBUSY:   return __WASI_ERRNO_BUSY;
    case EEXIST:  return __WASI_ERRNO_EXIST;
    case EXDEV:   return __WASI_ERRNO_XDEV;
    case ENODEV:  return __WASI_ERRNO_NODEV;
    case ENOTDIR: return __WASI_ERRNO_NOTDIR;
    case EISDIR:  return __WASI_ERRNO_ISDIR;
    case EINVAL:  return __WASI_ERRNO_INVAL;
    case ENFILE:  return __WASI_ERRNO_NFILE;
    case EMFILE:  return __WASI_ERRNO_MFILE;
    case ENOTTY:  return __WASI_ERRNO_NOTTY;
    case ETXTBSY: return __WASI_ERRNO_TXTBSY;
    case EFBIG:   return __WASI_ERRNO_FBIG;
    case ENOSPC:  return __WASI_ERRNO_NOSPC;
    case ESPIPE:  return __WASI_ERRNO_SPIPE;
    case EROFS:   return __WASI_ERRNO_ROFS;
    case EMLINK:  return __WASI_ERRNO_MLINK;
    case EPIPE:   return __WASI_ERRNO_PIPE;
    case EDOM:    return __WASI_ERRNO_DOM;
    case ERANGE:  return __WASI_ERRNO_RANGE;
    default:      return __WASI_ERRNO_INVAL;
    }
}

uint32_t SimpleWasiImpl::fd_prestat_get(uintptr_t params)
{
    __wasi_fd_t fd           = WasiGetArg<__wasi_fd_t,    0>(params);
    WasmMemPtr<uint32_t> buf = WasiGetMemPtrArg<uint32_t, 1>(params);

    // printf("Enter fd_prestat_get with fd = %d\n", static_cast<int>(fd));

    if (fd < 3 || fd >= x_preopen_count) { return __WASI_ERRNO_BADF; }
    buf[0] = __WASI_PREOPENTYPE_DIR;
    buf[1] = static_cast<uint32_t>(x_preopen[fd].m_pathLen);
    return __WASI_ERRNO_SUCCESS;
}

uint32_t SimpleWasiImpl::fd_prestat_dir_name(uintptr_t params)
{
    __wasi_fd_t fd         = WasiGetArg<__wasi_fd_t,    0>(params);
    WasmMemPtr<char> path  = WasiGetMemPtrArg<char,     1>(params);
    __wasi_size_t path_len = WasiGetArg<__wasi_size_t,  2>(params);

    // printf("Enter fd_prestat_dir_name with fd = %d, path_len = %d\n", static_cast<int>(fd), static_cast<int>(path_len));

    if (fd < 3 || fd >= x_preopen_count) { return __WASI_ERRNO_BADF; }
    path_len = std::min(path_len, x_preopen[fd].m_pathLen);
    for (uint32_t i = 0; i < path_len; i++)
    {
        path[i] = x_preopen[fd].m_path[i];
    }
    return __WASI_ERRNO_SUCCESS;
}

uint32_t SimpleWasiImpl::environ_sizes_get(uintptr_t params)
{
    WasmMemPtr<__wasi_size_t> env_count    = WasiGetMemPtrArg<__wasi_size_t, 0>(params);
    WasmMemPtr<__wasi_size_t> env_buf_size = WasiGetMemPtrArg<__wasi_size_t, 1>(params);

    // printf("Enter environ_sizes_get\n");

    // TODO
    *env_count = 0;
    *env_buf_size = 0;

    return __WASI_ERRNO_SUCCESS;
}

uint32_t SimpleWasiImpl::environ_get(uintptr_t /*params*/)
{
    // WasmMemPtr<uint32_t> env_count = WasiGetMemPtrArg<uint32_t, 0>(params);
    // WasmMemPtr<char> env_buf_size  = WasiGetMemPtrArg<char, 1>(params);
    // TODO

    // printf("Enter environ_get\n");

    return __WASI_ERRNO_SUCCESS;
}

// constexpr const char* x_fake_arg = "./main";

uint32_t SimpleWasiImpl::args_sizes_get(uintptr_t params)
{
    WasmMemPtr<__wasi_size_t> argc          = WasiGetMemPtrArg<__wasi_size_t, 0>(params);
    WasmMemPtr<__wasi_size_t> argv_buf_size = WasiGetMemPtrArg<__wasi_size_t, 1>(params);

    // printf("Enter args_sizes_get\n");

    // argc is # of arguments, argv_buf_size is total size of all arguments (including trailing '\0')
    //
    /*
    *argc = 1;
    *argv_buf_size = __builtin_strlen(x_fake_arg) + 1;
    */
    *argc = 0;
    *argv_buf_size = 0;
    return __WASI_ERRNO_SUCCESS;
}

uint32_t SimpleWasiImpl::args_get(uintptr_t /*params*/)
{
    // WasmMemPtr<uint32_t> argv = WasiGetMemPtrArg<uint32_t, 0>(params);
    // WasmMemPtr<char> buf      = WasiGetMemPtrArg<char,     1>(params);

    // printf("Enter args_get\n");

    // Populate all arguments to 'buf' and populate argv[i] to be the address of argument i
/*
    argv[0] = static_cast<uint32_t>(reinterpret_cast<uint64_t>(buf));
    for (uint32_t i = 0; i <= __builtin_strlen(x_fake_arg); i++)
    {
        buf[i] = x_fake_arg[i];
    }
*/
    return __WASI_ERRNO_SUCCESS;
}

static int convert_clockid(__wasi_clockid_t in) {
    switch (in) {
    case __WASI_CLOCKID_MONOTONIC:            return CLOCK_MONOTONIC;
    case __WASI_CLOCKID_PROCESS_CPUTIME_ID:   return CLOCK_PROCESS_CPUTIME_ID;
    case __WASI_CLOCKID_REALTIME:             return CLOCK_REALTIME;
    case __WASI_CLOCKID_THREAD_CPUTIME_ID:    return CLOCK_THREAD_CPUTIME_ID;
    default: return -1;
    }
}

uint32_t SimpleWasiImpl::clock_time_get(uintptr_t params)
{
    __wasi_clockid_t clockId           = WasiGetArg<__wasi_clockid_t,         0>(params);
    // arg 1 is a value of '__wasi_timestamp_t', but ignored
    WasmMemPtr<__wasi_timestamp_t> buf = WasiGetMemPtrArg<__wasi_timestamp_t, 2>(params);

    // printf("Enter clock_time_get\n");

    int linux_clockid = convert_clockid(clockId);
    if (linux_clockid == -1) { return __WASI_ERRNO_INVAL; }

    struct timespec ts;
    if (clock_gettime(linux_clockid, &ts) != 0)
    {
        return linux_errno_to_wasi(errno);
    }

    __wasi_timestamp_t result = static_cast<__wasi_timestamp_t>(ts.tv_sec) * 1000000000 + static_cast<__wasi_timestamp_t>(ts.tv_nsec);
    *buf = result;

    return __WASI_ERRNO_SUCCESS;
}

void __attribute__((__noreturn__)) SimpleWasiImpl::proc_exit(uintptr_t params)
{
    int exitCode = WasiGetArg<int, 0>(params);
    // TODO: we should use a longjmp to gracefully return to the caller
    // printf("Client program called proc_exit with exit code %d, exiting...\n", exitCode);
    exit(exitCode);
}

uint32_t SimpleWasiImpl::fd_fdstat_get(uintptr_t params)
{
    __wasi_fd_t fd                     = WasiGetArg<__wasi_fd_t,           0>(params);
    WasmMemPtr<__wasi_fdstat_t> fdstat = WasiGetMemPtrArg<__wasi_fdstat_t, 1>(params);

    // printf("Enter fd_fdstat_get with fd = %d\n", static_cast<int>(fd));

    if (fd < 3)
    {
        struct stat fd_stat;

        int fl = fcntl(static_cast<int>(fd), F_GETFL);
        if (fl < 0) { return linux_errno_to_wasi(errno); }

        int r = fstat(static_cast<int>(fd), &fd_stat);
        if (r != 0) { return linux_errno_to_wasi(errno); }

        uint32_t mode = fd_stat.st_mode;
        fdstat->fs_filetype = (S_ISBLK(mode)   ? __WASI_FILETYPE_BLOCK_DEVICE     : 0) |
                              (S_ISCHR(mode)   ? __WASI_FILETYPE_CHARACTER_DEVICE : 0) |
                              (S_ISDIR(mode)   ? __WASI_FILETYPE_DIRECTORY        : 0) |
                              (S_ISREG(mode)   ? __WASI_FILETYPE_REGULAR_FILE     : 0) |
                              //(S_ISSOCK(mode)  ? __WASI_FILETYPE_SOCKET_STREAM    : 0) |
                              (S_ISLNK(mode)   ? __WASI_FILETYPE_SYMBOLIC_LINK    : 0);

        fdstat->fs_flags = ((fl & O_APPEND)    ? __WASI_FDFLAGS_APPEND    : 0) |
                           ((fl & O_DSYNC)     ? __WASI_FDFLAGS_DSYNC     : 0) |
                           ((fl & O_NONBLOCK)  ? __WASI_FDFLAGS_NONBLOCK  : 0) |
                           //((fl & O_RSYNC)     ? __WASI_FDFLAGS_RSYNC     : 0) |
                           ((fl & O_SYNC)      ? __WASI_FDFLAGS_SYNC      : 0);
    }
    else if (fd < x_preopen_count)
    {
        fdstat->fs_filetype = __WASI_FILETYPE_DIRECTORY;
        fdstat->fs_flags = 0;
    }
    else
    {
        fdstat->fs_filetype = __WASI_FILETYPE_REGULAR_FILE;
        fdstat->fs_flags = 0;
    }

    fdstat->fs_rights_base = static_cast<uint64_t>(-1); // all rights
    fdstat->fs_rights_inheriting = static_cast<uint64_t>(-1); // all rights

    return __WASI_ERRNO_SUCCESS;
}

uint32_t SimpleWasiImpl::fd_close(uintptr_t params)
{
    __wasi_fd_t fd = WasiGetArg<__wasi_fd_t, 0>(params);

    // printf("Enter fd_close with fd = %d\n", static_cast<int>(fd));

    close(static_cast<int>(fd));
    return __WASI_ERRNO_SUCCESS;
}

uint32_t SimpleWasiImpl::fd_seek(uintptr_t params)
{
    __wasi_fd_t fd                       = WasiGetArg<__wasi_fd_t,             0>(params);
    __wasi_filedelta_t offset            = WasiGetArg<__wasi_filedelta_t,      1>(params);
    __wasi_whence_t wasi_whence          = WasiGetArg<__wasi_whence_t,         2>(params);
    WasmMemPtr<__wasi_filesize_t> result = WasiGetMemPtrArg<__wasi_filesize_t, 3>(params);

    // printf("Enter fd_seek with fd = %d\n", static_cast<int>(fd));

    int whence;
    switch (wasi_whence)
    {
    case __WASI_WHENCE_CUR: { whence = SEEK_CUR; break; }
    case __WASI_WHENCE_END: { whence = SEEK_END; break; }
    case __WASI_WHENCE_SET: { whence = SEEK_SET; break; }
    default:                { return __WASI_ERRNO_INVAL; }
    }

    int64_t ret;
    ret = lseek(static_cast<int>(fd), offset, whence);
    if (ret < 0) { return linux_errno_to_wasi(errno); }

    *result = static_cast<uint64_t>(ret);

    return __WASI_ERRNO_SUCCESS;
}

struct wasi_iovec_t
{
    __wasi_size_t buf;
    __wasi_size_t buf_len;
};

static void copy_iov_to_host(struct iovec* host_iov, WasmMemPtr<wasi_iovec_t> wasi_iov, uint32_t iovs_len)
{
    uint64_t gsLoc = *reinterpret_cast<WasmMemPtr<uint64_t>>(static_cast<uint64_t>(-16));
    for (uint32_t i = 0; i < iovs_len; i++)
    {
        host_iov[i].iov_base = reinterpret_cast<void*>(gsLoc + wasi_iov[i].buf);
        host_iov[i].iov_len  = wasi_iov[i].buf_len;
    }
}

uint32_t SimpleWasiImpl::fd_write(uintptr_t params)
{
    __wasi_fd_t fd                     = WasiGetArg<__wasi_fd_t,             0>(params);
    WasmMemPtr<wasi_iovec_t> wasi_iovs = WasiGetMemPtrArg<wasi_iovec_t,      1>(params);
    __wasi_size_t iovs_len             = WasiGetArg<__wasi_size_t,           2>(params);
    WasmMemPtr<__wasi_size_t> nwritten = WasiGetMemPtrArg<__wasi_size_t,     3>(params);

    // printf("Enter fd_write with fd = %d\n", static_cast<int>(fd));

    struct iovec* iovs = reinterpret_cast<struct iovec*>(alloca(sizeof(struct iovec) * iovs_len));
    copy_iov_to_host(iovs, wasi_iovs, iovs_len);

    ssize_t ret = writev(static_cast<int>(fd), iovs, static_cast<int>(iovs_len));
    if (ret < 0) { return linux_errno_to_wasi(errno); }
    *nwritten = static_cast<__wasi_size_t>(ret);
    return __WASI_ERRNO_SUCCESS;
}

uint32_t SimpleWasiImpl::poll_oneoff(uintptr_t params)
{
  return params == 0 ? 0 : 0;
}

uint32_t SimpleWasiImpl::random_get(uintptr_t params)
{
  return params == 0 ? 0 : 0;
}


std::map< std::pair<std::string, std::string>, uintptr_t> g_wasiLinkMapping =
{
    { { "wasi_snapshot_preview1", "fd_prestat_get"},      reinterpret_cast<uintptr_t>(&SimpleWasiImpl::fd_prestat_get) },
    { { "wasi_snapshot_preview1", "fd_prestat_dir_name"}, reinterpret_cast<uintptr_t>(&SimpleWasiImpl::fd_prestat_dir_name) },
    { { "wasi_snapshot_preview1", "environ_sizes_get"},   reinterpret_cast<uintptr_t>(&SimpleWasiImpl::environ_sizes_get) },
    { { "wasi_snapshot_preview1", "environ_get"},         reinterpret_cast<uintptr_t>(&SimpleWasiImpl::environ_get) },
    { { "wasi_snapshot_preview1", "args_sizes_get"},      reinterpret_cast<uintptr_t>(&SimpleWasiImpl::args_sizes_get) },
    { { "wasi_snapshot_preview1", "args_get"},            reinterpret_cast<uintptr_t>(&SimpleWasiImpl::args_get) },
    { { "wasi_snapshot_preview1", "clock_time_get"},      reinterpret_cast<uintptr_t>(&SimpleWasiImpl::clock_time_get) },
    { { "wasi_snapshot_preview1", "proc_exit"},           reinterpret_cast<uintptr_t>(&SimpleWasiImpl::proc_exit) },
    { { "wasi_snapshot_preview1", "fd_fdstat_get"},       reinterpret_cast<uintptr_t>(&SimpleWasiImpl::fd_fdstat_get) },
    { { "wasi_snapshot_preview1", "fd_close"},            reinterpret_cast<uintptr_t>(&SimpleWasiImpl::fd_close) },
    { { "wasi_snapshot_preview1", "fd_seek"},             reinterpret_cast<uintptr_t>(&SimpleWasiImpl::fd_seek) },
    { { "wasi_snapshot_preview1", "poll_oneoff"},         reinterpret_cast<uintptr_t>(&SimpleWasiImpl::poll_oneoff) },
    { { "wasi_snapshot_preview1", "random_get"},          reinterpret_cast<uintptr_t>(&SimpleWasiImpl::random_get) },
    { { "wasi_snapshot_preview1", "fd_write"},            reinterpret_cast<uintptr_t>(&SimpleWasiImpl::fd_write) }
};
