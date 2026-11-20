/*
 *  instance_lock.cpp
 *  PHD Guiding
 *
 *  See instance_lock.h.
 */

#include "instance_lock.h"

#include <sys/stat.h>
#include <unistd.h>

namespace InstanceLock
{
std::string LockDir(const char *runtimeDirectory)
{
    if (!runtimeDirectory)
        return std::string();

    std::string dir(runtimeDirectory);
    std::string::size_type sep = dir.find(':');
    if (sep != std::string::npos)
        dir.erase(sep);

    if (dir.empty() || dir[0] != '/')
        return std::string();

    struct stat st;
    if (stat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode))
        return std::string();
    if (access(dir.c_str(), W_OK | X_OK) != 0)
        return std::string();

    return dir;
}
} // namespace InstanceLock
