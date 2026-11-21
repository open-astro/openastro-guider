/*
 *  atomic_file_write.h
 *  PHD Guiding
 *
 *  Writes a whole file so that a crash or power cut at any point leaves either
 *  the old file or the new one on disk, never an empty, torn or missing one
 *  (#142). PhdConfig::Flush writes the config file with it: on RC51 the power
 *  is cut in the field. Kept free of wx so it can be unit tested
 *  (tests/startup/test_atomic_file_write.cpp).
 */

#ifndef ATOMIC_FILE_WRITE_H_INCLUDED
#define ATOMIC_FILE_WRITE_H_INCLUDED

#include <string>

// The temporary file AtomicFileWrite writes `path` through, in the same directory.
std::string AtomicFileWriteTempPath(const std::string& path);

// Replaces the file at `path` with `data`: writes `data` to AtomicFileWriteTempPath(path), fsyncs it, renames it
// over `path` (rename(2) replaces the old file in one step; it is never deleted first) and fsyncs the directory so
// the rename itself is on disk. The new file keeps the permission bits of the one it replaces; a new file gets
// 0666 less the umask. Returns true on success. On failure returns false with *error (when not null) saying which
// step failed and why, and removes the temporary file; `path` is then as it was, unless only the final directory
// fsync failed, after the rename.
bool AtomicFileWrite(const std::string& path, const std::string& data, std::string *error);

#endif
