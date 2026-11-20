/*
 *  instance_lock.h
 *  PHD Guiding
 *
 *  Where the wxSingleInstanceChecker lock goes (#135). Under systemd the
 *  unit's RuntimeDirectory= gives the daemon a directory on /run, a tmpfs
 *  that systemd also removes whenever the service stops, so a lock left by a
 *  power loss or a crash cannot survive to name a PID that a process of the
 *  next boot has reused. Kept free of wx so it can be unit tested
 *  (tests/startup/test_instance_lock.cpp).
 */

#ifndef INSTANCE_LOCK_H_INCLUDED
#define INSTANCE_LOCK_H_INCLUDED

#include <string>

namespace InstanceLock
{
// The directory for the instance lock, given the value of $RUNTIME_DIRECTORY
// (nullptr when it is not set): its first entry (systemd separates several
// RuntimeDirectory= entries with ':') when that is an absolute path to an
// existing directory this process can create files in. Otherwise an empty
// string, which tells wxSingleInstanceChecker to use its default, the home
// directory, as before.
std::string LockDir(const char *runtimeDirectory);
} // namespace InstanceLock

#endif
