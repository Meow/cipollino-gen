#include "platform.h"

#include <cerrno>
#include <cstring>

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdio>
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>

namespace plat {

static std::string describe(const std::string &path)
{
    char msg[256] = "error";
    strerror_s(msg, sizeof msg, errno);
    return path + ": " + msg;
}

bool make_private_dir(const std::string &path, bool exist_ok, std::string &err)
{
    if (_mkdir(path.c_str()) == 0 || (exist_ok && errno == EEXIST)) return true;
    err = describe(path);
    return false;
}

bool write_new_file(const std::string &path, const void *data, size_t len, std::string &err)
{
    int fd = -1;
    if (_sopen_s(&fd, path.c_str(), _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY, _SH_DENYRW, _S_IREAD | _S_IWRITE) != 0) {
        err = describe(path);
        return false;
    }
    const char *p = (const char *)data;
    while (len > 0) {
        int n = _write(fd, p, (unsigned)len);
        if (n < 0) {
            err = describe(path);
            _close(fd);
            return false;
        }
        p += n;
        len -= (size_t)n;
    }
    if (_commit(fd) != 0 || _close(fd) != 0) {
        err = describe(path);
        return false;
    }
    return true;
}

bool dir_writable(const std::string &path, std::string &err)
{
    if (_access(path.c_str(), 2) == 0) return true;
    err = describe(path);
    return false;
}

bool read_file(const std::string &path, void *buf, size_t cap, size_t &len, std::string &err)
{
    int fd = -1;
    if (_sopen_s(&fd, path.c_str(), _O_RDONLY | _O_BINARY, _SH_DENYWR, 0) != 0) {
        err = describe(path);
        return false;
    }
    char *p = (char *)buf, extra;
    len = 0;
    for (;;) {
        int n = len < cap ? _read(fd, p + len, (unsigned)(cap - len)) : _read(fd, &extra, 1);
        if (n < 0) {
            err = describe(path);
            _close(fd);
            return false;
        }
        if (n == 0) break;
        if (len == cap) {
            err = path + ": larger than expected";
            _close(fd);
            return false;
        }
        len += (size_t)n;
    }
    _close(fd);
    return true;
}

bool open_to_others(const std::string &) { return false; }

bool stderr_is_terminal()
{
    if (!_isatty(_fileno(stderr))) return false;
    HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    DWORD mode = 0;
    if (h == INVALID_HANDLE_VALUE || !GetConsoleMode(h, &mode)) return false;
    // ENABLE_VIRTUAL_TERMINAL_PROCESSING; consoles older than Windows 10 refuse it.
    return SetConsoleMode(h, mode | 0x0004) != 0;
}

}  // namespace plat

#else

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace plat {

static std::string describe(const std::string &path) { return path + ": " + strerror(errno); }

bool make_private_dir(const std::string &path, bool exist_ok, std::string &err)
{
    if (mkdir(path.c_str(), 0700) == 0 || (exist_ok && errno == EEXIST)) return true;
    err = describe(path);
    return false;
}

bool write_new_file(const std::string &path, const void *data, size_t len, std::string &err)
{
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        err = describe(path);
        return false;
    }
    const char *p = (const char *)data;
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            err = describe(path);
            close(fd);
            return false;
        }
        p += n;
        len -= (size_t)n;
    }
    if (fsync(fd) != 0 || close(fd) != 0) {
        err = describe(path);
        return false;
    }
    return true;
}

bool dir_writable(const std::string &path, std::string &err)
{
    if (access(path.c_str(), W_OK | X_OK) == 0) return true;
    err = describe(path);
    return false;
}

bool read_file(const std::string &path, void *buf, size_t cap, size_t &len, std::string &err)
{
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        err = describe(path);
        return false;
    }
    char *p = (char *)buf, extra;
    len = 0;
    for (;;) {
        ssize_t n = len < cap ? read(fd, p + len, cap - len) : read(fd, &extra, 1);
        if (n < 0) {
            if (errno == EINTR) continue;
            err = describe(path);
            close(fd);
            return false;
        }
        if (n == 0) break;
        if (len == cap) {
            err = path + ": larger than expected";
            close(fd);
            return false;
        }
        len += (size_t)n;
    }
    close(fd);
    return true;
}

bool open_to_others(const std::string &path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0 && (st.st_mode & 077) != 0;
}

bool stderr_is_terminal() { return isatty(STDERR_FILENO) != 0; }

}  // namespace plat

#endif

// ---- the same on every system ------------------------------------------------------------

#include <algorithm>
#include <filesystem>

namespace plat {

bool exists(const std::string &path)
{
    std::error_code ec;
    return std::filesystem::exists(path, ec);
}

bool list_subdirs(const std::string &path, std::vector<std::string> &names, std::string &err)
{
    std::error_code ec;
    names.clear();
    for (std::filesystem::directory_iterator it(path, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_directory(ec)) names.push_back(it->path().filename().string());
    if (ec) {
        err = path + ": " + ec.message();
        return false;
    }
    std::sort(names.begin(), names.end());
    return true;
}

}  // namespace plat
