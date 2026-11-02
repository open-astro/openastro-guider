/*
 *  capture_cleanup.h
 *  PHD Guiding
 *
 *  Pruning of the frames the daemon saves under names it picks itself (#93).
 *  capture_single_frame {save:true} without a path and save_image write a new
 *  save_image_XXXXXX file for each call. A remote client fetches it over
 *  GET /api/capture/<filename> but cannot delete it, so the daemon removes
 *  old ones itself. Kept free of wx so it can be unit tested
 *  (tests/capture/test_capture_cleanup.cpp).
 */

#ifndef CAPTURE_CLEANUP_H_INCLUDED
#define CAPTURE_CLEANUP_H_INCLUDED

#include <chrono>
#include <cstddef>
#include <string>

// How many auto-named frames to keep per directory, and how long. A client
// fetches the frame as soon as SingleFrameComplete arrives (or reads it as
// soon as save_image returns), so these leave plenty of slack while bounding
// the disk use to a few frames.
constexpr std::size_t kAutoSavedFrameKeepCount = 10;
constexpr std::chrono::seconds kAutoSavedFrameMaxAge = std::chrono::minutes(15);

// True for a name wxFileName::CreateTempFileName(dir + "/save_image_") gives:
// "save_image_" followed by the six characters mkstemp fills in, nothing else.
// Files a client named itself through capture_single_frame's path param don't
// match, so they are never pruned.
bool IsAutoSavedFrameName(const std::string& filename);

// Deletes the auto-named frames in dir that are older than maxAge or beyond
// the newest keepCount (by modification time). Only regular files are
// touched; symlinks and anything not matching IsAutoSavedFrameName are left
// alone. keepPath, when not empty, is never deleted (the frame just written).
// Returns the number of files removed. Errors (missing dir, a file that can't
// be removed) are skipped, not reported.
std::size_t PruneAutoSavedFrames(const std::string& dir, std::size_t keepCount, std::chrono::seconds maxAge,
                                 const std::string& keepPath = std::string());

#endif
