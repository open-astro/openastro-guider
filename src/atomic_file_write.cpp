/*
 *  atomic_file_write.cpp
 *  PHD Guiding
 *
 *  See atomic_file_write.h.
 */

#include "atomic_file_write.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace
{
std::string Describe(const char *step, const std::string& path, int err)
{
    return std::string(step) + " " + path + ": " + std::strerror(err);
}

void SetError(std::string *error, const std::string& msg)
{
    if (error)
        *error = msg;
}

std::string DirectoryOf(const std::string& path)
{
    std::string::size_type slash = path.rfind('/');
    if (slash == std::string::npos)
        return ".";
    if (slash == 0)
        return "/";
    return path.substr(0, slash);
}

bool WriteAll(int fd, const std::string& data)
{
    const char *p = data.data();
    size_t left = data.size();
    while (left > 0)
    {
        ssize_t n = ::write(fd, p, left);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            return false;
        }
        p += n;
        left -= static_cast<size_t>(n);
    }
    return true;
}

int FsyncRetry(int fd)
{
    int r;
    do
        r = ::fsync(fd);
    while (r != 0 && errno == EINTR);
    return r;
}
} // namespace

std::string AtomicFileWriteTempPath(const std::string& path)
{
    return path + ".tmp";
}

bool AtomicFileWrite(const std::string& path, const std::string& data, std::string *error)
{
    const std::string tmp = AtomicFileWriteTempPath(path);

    // O_TRUNC: a temporary file left by a write that was killed is simply overwritten
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
    if (fd < 0)
    {
        SetError(error, Describe("cannot create", tmp, errno));
        return false;
    }

    auto fail = [&](const char *step, const std::string& what)
    {
        int err = errno;
        if (fd >= 0)
            ::close(fd);
        ::unlink(tmp.c_str());
        SetError(error, Describe(step, what, err));
        return false;
    };

    struct stat st;
    if (::stat(path.c_str(), &st) == 0 && ::fchmod(fd, st.st_mode & 07777) != 0)
        return fail("cannot set the mode of", tmp);
    if (!WriteAll(fd, data))
        return fail("cannot write", tmp);
    if (FsyncRetry(fd) != 0)
        return fail("cannot fsync", tmp);
    int r = ::close(fd);
    fd = -1;
    if (r != 0)
        return fail("cannot close", tmp);
    if (::rename(tmp.c_str(), path.c_str()) != 0)
        return fail("cannot rename onto", path);

    // the rename is a change to the directory: without this it can be lost in a power cut
    const std::string dir = DirectoryOf(path);
    int dfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd < 0)
    {
        SetError(error, Describe("cannot open the directory", dir, errno));
        return false;
    }
    if (FsyncRetry(dfd) != 0)
    {
        int err = errno;
        ::close(dfd);
        SetError(error, Describe("cannot fsync the directory", dir, err));
        return false;
    }
    ::close(dfd);
    return true;
}
