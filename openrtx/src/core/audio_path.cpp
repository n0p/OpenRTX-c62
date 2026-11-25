/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * 
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "core/audio_path.h"
#include "interfaces/delays.h"

#include <map>
#include <pthread.h>
#include <set>
#include <vector>

/**
 * \internal
 * Data structure representing an audio path with source, destination and
 * priority.
 */
struct Path
{
    int8_t source      = -1;   ///< Source endpoint of the path.
    int8_t destination = -1;   ///< Destination endpoint of the path.
    int8_t priority    = -1;   ///< Path priority level.

    Path() = default;

    Path(enum AudioSource src, enum AudioSink sink, enum AudioPriority prio)
    {
        source = static_cast<int8_t>(src);
        destination = static_cast<int8_t>(sink);
        priority = static_cast<int8_t>(prio);
    }

    bool isValid() const
    {
        return (source      != -1) &&
               (destination != -1) &&
               (priority    != -1);
    }

    void open() const
    {
        if(isValid() == false)
            return;

        enum AudioSource src  = (enum AudioSource) source;
        enum AudioSink   sink = (enum AudioSink)   destination;

        audio_connect(src, sink);
    }

    void close() const
    {
        if(isValid() == false)
            return;

        enum AudioSource src  = (enum AudioSource) source;
        enum AudioSink   sink = (enum AudioSink)   destination;

        audio_disconnect(src, sink);
    }

    bool isCompatible(const Path& other) const
    {
        if((isValid() == false) || (other.isValid() == false))
            return false;

        enum AudioSource p1Source = (enum AudioSource) source;
        enum AudioSource p2Source = (enum AudioSource) other.source;
        enum AudioSink   p1Sink   = (enum AudioSink)   destination;
        enum AudioSink   p2Sink   = (enum AudioSink)   other.destination;

        return audio_checkPathCompatibility(p1Source, p1Sink, p2Source, p2Sink);
    }

    bool operator<(const Path& other) const
    {
        if((isValid() == false) || (other.isValid() == false))
            return false;

        return (source      < other.source) &&
               (destination < other.destination);
    }
};

/**
 * \internal
 * Data structure representing an established audio route.
 */
struct Route
{
    Path path;                    ///< Path associated to this route.
    std::set< int > suspendList;  ///< Suspended paths with lower priority.
    std::set< int > suspendedBy;  ///< List of paths which suspended this route.

    bool isActive() const
    {
        return suspendedBy.empty();
    }
};


static std::set< int >        activePaths;      // IDs of currently active paths.
static std::map< int, Route > routes;           // Route data of currently active paths.
static int                    pathCounter = 1;  // Counter for path ID generation.

/*
 * The table above is shared, and that is the whole point of this file: the RTX
 * thread opens and closes the receiver's path from the squelch of every mode,
 * while the UI thread opens the prompt path for a beep or a voice announcement
 * -- and a change of mode is exactly where the two of them do it at the same
 * instant (the mode queues its announcement, the new mode opens its receiver).
 *
 * Without a lock, two threads insert into, erase from and traverse the same
 * std::map and std::set. The red-black trees they share are corrupted by a
 * concurrent rotation, and the failure is not a wrong answer but a lookup that
 * never comes back -- the UI thread spinning inside a libstdc++ traversal for
 * ever, with the receiver still running -- or a std::out_of_range from
 * routes.at() that aborts the firmware, which is compiled without exceptions.
 *
 * One mutex, and it is never held across a call into the driver:
 * audio_connect() and audio_disconnect() wait for the codec worker (up to
 * CSK6_ROUTE_TIMEOUT_MS on the C62), so holding this lock across them would
 * hand that wait to every thread that only wants to read the table, which is
 * the same class of hang one layer up. The table is mutated under the lock, the
 * device call is made after releasing it.
 */
static pthread_mutex_t pathMutex = PTHREAD_MUTEX_INITIALIZER;

/** How long a caller waits for the table, in bounded steps (see pathLock()). */
static constexpr uint32_t PATH_LOCK_MS      = 50;
static constexpr uint32_t PATH_LOCK_STEP_MS = 1;

/**
 * \internal
 * Take the path table's lock. Bounded: this lock is only ever held for a few
 * instructions, so a timeout means the holder died inside the critical section
 * and the caller must not join it.
 *
 * The bound is what keeps the transmit path honest.  This table is touched by
 * the RTX thread (the squelch of every mode), by the UI thread (a voice prompt)
 * and by the DMR speech thread, and a blocking acquisition here is a wait with
 * no deadline in the middle of the keying path -- exactly the shape that used
 * to leave the radio with the PA keyed and no way out.  A caller that cannot
 * get the table now fails its own call (no path, PATH_CLOSED) and asks again on
 * its next pass.
 *
 * @return true if the lock was taken.
 */
static bool pathLock()
{
    if(pthread_mutex_trylock(&pathMutex) == 0)
        return true;

    uint32_t waited = 0;
    while(waited < PATH_LOCK_MS)
    {
        delayMs(PATH_LOCK_STEP_MS);
        waited += PATH_LOCK_STEP_MS;
        if(pthread_mutex_trylock(&pathMutex) == 0)
            return true;
    }
    return false;
}

static void pathUnlock()
{
    pthread_mutex_unlock(&pathMutex);
}

/**
 * \internal
 * Look up a route without routes.at(): this firmware has no exceptions, so a
 * missing key has to be a null pointer and not a std::out_of_range that ends
 * the radio.
 */
static Route *routeFind(int id)
{
    auto it = routes.find(id);
    return (it == routes.end()) ? nullptr : &it->second;
}

pathId audioPath_request(enum AudioSource source, enum AudioSink sink,
                         enum AudioPriority prio)
{
    const Path path(source, sink, prio);
    if (!path.isValid())
        return -1;

    std::set< int >   pathsToSuspend;
    std::vector< Path > toClose;
    int newPathId;

    if (!pathLock())
        return -1;

    // Check if this new path can be activated, otherwise return -1
    for (const auto& id : activePaths)
    {
        const Path& activePath = routeFind(id)->path;
        if(path.isCompatible(activePath))
            continue;

        // Not compatible where active one has higher priority
        if(activePath.priority >= path.priority)
        {
            pathUnlock();
            return -1;
        }

        // Active path has lower priority than this new one
        pathsToSuspend.insert(id);
    }

    // New path can be activated
    newPathId = pathCounter;
    pathCounter += 1;
    const Route newRoute{path, pathsToSuspend, {}};

    /*
     * Move active paths that should be suspended to the suspend-list and take
     * their device calls out of the critical section: they are closed below,
     * with the lock released.
     */
    for(const auto& id : pathsToSuspend)
    {
        Route *suspended = routeFind(id);

        activePaths.erase(id);
        suspended->suspendedBy.insert(newPathId);
        toClose.push_back(suspended->path);
    }

    // Set this new path as active
    routes.insert(std::make_pair(newPathId, newRoute));
    activePaths.insert(newPathId);

    pathUnlock();

    /*
     * Free the resources of the suspended paths and then open this one, in that
     * order and outside the lock: both calls go to the audio driver and can
     * wait for it.
     */
    for (const Path& p : toClose)
        p.close();

    path.open();

    return newPathId;
}

pathInfo_t audioPath_getInfo(const pathId id)
{
    pathInfo_t info = {0, 0, 0, 0};

    if (!pathLock())
    {
        info.status = PATH_CLOSED;
        return info;
    }

    const Route *route = routeFind(id);
    if(route == nullptr)
    {
        pathUnlock();
        info.status = PATH_CLOSED;
        return info;
    }

    info.source = route->path.source;
    info.sink   = route->path.destination;
    info.prio   = route->path.priority;
    if(route->isActive())
        info.status = PATH_OPEN;
    else
        info.status = PATH_SUSPENDED;

    pathUnlock();

    return info;
}

enum PathStatus audioPath_getStatus(const pathId id)
{
    if (!pathLock())
        return PATH_CLOSED;

    const Route *route = routeFind(id);
    enum PathStatus status = PATH_CLOSED;

    if(route != nullptr)
        status = route->isActive() ? PATH_OPEN : PATH_SUSPENDED;

    pathUnlock();

    return status;
}

void audioPath_release(const pathId id)
{
    Route routeToRemove;
    std::vector< Path > toClose;
    std::vector< Path > toResume;

    if (!pathLock())
        return;

    Route *route = routeFind(id);
    if (route == nullptr)  // Does not exists
    {
        pathUnlock();
        return;
    }

    routeToRemove = *route;

    routes.erase(id);
    activePaths.erase(id);

    // If path is active, close it (outside the lock)
    if(routeToRemove.isActive())
        toClose.push_back(routeToRemove.path);

    /*
     * For each path that suspended the one to be removed:
     * - remove the ID from its suspend list.
     * - add to its suspend list the paths suspended by the one being removed.
     */
    for(const auto& i : routeToRemove.suspendedBy)
    {
        Route *holder = routeFind(i);
        if (holder == nullptr)
            continue;

        holder->suspendList.erase(id);

        for(const auto& j : routeToRemove.suspendList)
            holder->suspendList.insert(j);
    }

    /*
     * For each path suspended by the one to be removed:
     * - remove the ID from its suspended-by list.
     * - add to its suspended-by list the paths which suspended the one being
     *   removed.
     * - if the path to be removed was not suspended by any other path, resume
     *   the path (the device call happens outside the lock).
     */
    for(const auto& i : routeToRemove.suspendList)
    {
        Route *suspended = routeFind(i);
        if (suspended == nullptr)
            continue;

        auto& suspendedBy = suspended->suspendedBy;
        suspendedBy.erase(id);

        if(routeToRemove.suspendedBy.empty() == false)
        {
            // If I was suspended, propagate who suspended me
            for(const auto& j : routeToRemove.suspendedBy)
                suspendedBy.insert(j);
        }
        else
        {
            // This path can be started again
            if(suspendedBy.empty())
            {
                activePaths.insert(i);
                toResume.push_back(suspended->path);
            }
        }
    }

    pathUnlock();

    for (const Path& p : toClose)
        p.close();

    for (const Path& p : toResume)
        p.open();
}
