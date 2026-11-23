/*
 *  algorithm_slot.h
 *  PHD Guiding
 *
 *  The guard between a mount's guide computation and the changes to it (#142 T2b).
 *
 *  For a mount with a non-GUI move (ScopeAlpaca, the simulators), Mount::MoveOffset runs on the
 *  worker thread (WorkerThread::HandleMove): it feeds the offsets to the guide algorithms and the
 *  Dec backlash comp. The main thread changes the same objects: set_algo deletes an algorithm and
 *  creates another, set_algo_param, guiding_assistant_apply, set_backlash_comp and the
 *  NotifyGuiding* fan-out (reset() and the other state changes) change their state. Until this
 *  guard nothing kept the two apart, so a set_algo while guiding could free the algorithm a move
 *  was using.
 *
 *  One AlgorithmSlot per mount. Hold it only for the computation or for the change, never across
 *  Mount::MoveAxis or any other call to the mount, camera or network, so a stalled mount cannot
 *  hold up the main thread (and the JSON-RPC server) behind it. It is not recursive: a holder must
 *  not call a method that takes it again. wx-free, so the guard has a unit test of its own built
 *  with ThreadSanitizer (tests/guiding/test_algorithm_slot.cpp).
 */

#ifndef ALGORITHM_SLOT_H_INCLUDED
#define ALGORITHM_SLOT_H_INCLUDED

#include <mutex>

class AlgorithmSlot
{
    std::mutex m_mutex;

public:
    using Hold = std::unique_lock<std::mutex>;

    AlgorithmSlot() = default;
    AlgorithmSlot(const AlgorithmSlot&) = delete;
    AlgorithmSlot& operator=(const AlgorithmSlot&) = delete;

    // The guard, held until the returned lock goes out of scope
    Hold Take() { return Hold(m_mutex); }
};

#endif
