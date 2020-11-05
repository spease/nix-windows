#include "machines.hh"
#include "worker.hh"
#include "substitution-goal.hh"
#include "derivation-goal.hh"
#include "hook-instance.hh"

#ifndef _WIN32
#include <poll.h>
#endif


namespace nix {

Worker::Worker(LocalStore & store)
    : act(*logger, actRealise)
    , actDerivations(*logger, actBuilds)
    , actSubstitutions(*logger, actCopyPaths)
    , store(store)
{
    /* Debugging: prevent recursive workers. */
    nrLocalBuilds = 0;
    lastWokenUp = steady_time_point::min();
    permanentFailure = false;
    timedOut = false;
    hashMismatch = false;
    checkMismatch = false;
#ifdef _WIN32
    ioport = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    if (ioport.get() == NULL)
        throw WinError("CreateIoCompletionPort");
#endif
}


Worker::~Worker()
{
    /* Explicitly get rid of all strong pointers now.  After this all
       goals that refer to this worker should be gone.  (Otherwise we
       are in trouble, since goals may call childTerminated() etc. in
       their destructors). */
    topGoals.clear();

    assert(expectedSubstitutions == 0);
    assert(expectedDownloadSize == 0);
    assert(expectedNarSize == 0);
}


std::shared_ptr<DerivationGoal> Worker::makeDerivationGoalCommon(
    const StorePath & drvPath,
    const StringSet & wantedOutputs,
    std::function<std::shared_ptr<DerivationGoal>()> mkDrvGoal)
{
    std::weak_ptr<DerivationGoal> & goal_weak = derivationGoals[drvPath];
    std::shared_ptr<DerivationGoal> goal = goal_weak.lock();
    if (!goal) {
        goal = mkDrvGoal();
        goal_weak = goal;
        wakeUp(goal);
    } else {
        goal->addWantedOutputs(wantedOutputs);
    }
    return goal;
}


std::shared_ptr<DerivationGoal> Worker::makeDerivationGoal(const StorePath & drvPath,
    const StringSet & wantedOutputs, BuildMode buildMode)
{
    return makeDerivationGoalCommon(drvPath, wantedOutputs, [&]() {
        return std::make_shared<DerivationGoal>(drvPath, wantedOutputs, *this, buildMode);
    });
}


std::shared_ptr<DerivationGoal> Worker::makeBasicDerivationGoal(const StorePath & drvPath,
    const BasicDerivation & drv, const StringSet & wantedOutputs, BuildMode buildMode)
{
    return makeDerivationGoalCommon(drvPath, wantedOutputs, [&]() {
        return std::make_shared<DerivationGoal>(drvPath, drv, wantedOutputs, *this, buildMode);
    });
}


std::shared_ptr<SubstitutionGoal> Worker::makeSubstitutionGoal(const StorePath & path, RepairFlag repair, std::optional<ContentAddress> ca)
{
    std::weak_ptr<SubstitutionGoal> & goal_weak = substitutionGoals[path];
    auto goal = goal_weak.lock(); // FIXME
    if (!goal) {
        goal = std::make_shared<SubstitutionGoal>(path, *this, repair, ca);
        goal_weak = goal;
        wakeUp(goal);
    }
    return goal;
}

template<typename G>
static void removeGoal(std::shared_ptr<G> goal, std::map<StorePath, std::weak_ptr<G>> & goalMap)
{
    /* !!! inefficient */
    for (auto i = goalMap.begin();
         i != goalMap.end(); )
        if (i->second.lock() == goal) {
            auto j = i; ++j;
            goalMap.erase(i);
            i = j;
        }
        else ++i;
}


void Worker::removeGoal(GoalPtr goal)
{
    if (auto drvGoal = std::dynamic_pointer_cast<DerivationGoal>(goal))
        nix::removeGoal(drvGoal, derivationGoals);
    else if (auto subGoal = std::dynamic_pointer_cast<SubstitutionGoal>(goal))
        nix::removeGoal(subGoal, substitutionGoals);
    else
        assert(false);
    if (topGoals.find(goal) != topGoals.end()) {
        topGoals.erase(goal);
        /* If a top-level goal failed, then kill all other goals
           (unless keepGoing was set). */
        if (goal->exitCode == Goal::ecFailed && !settings.keepGoing)
            topGoals.clear();
    }

    /* Wake up goals waiting for any goal to finish. */
    for (auto & i : waitingForAnyGoal) {
        GoalPtr goal = i.lock();
        if (goal) wakeUp(goal);
    }

    waitingForAnyGoal.clear();
}


void Worker::wakeUp(GoalPtr goal)
{
    goal->trace("woken up");
    addToWeakGoals(awake, goal);
}


unsigned Worker::getNrLocalBuilds()
{
    return nrLocalBuilds;
}


#ifndef _WIN32
void Worker::childStarted(GoalPtr goal, const set<int> & fds,
    bool inBuildSlot, bool respectTimeouts)
#else
void Worker::childStarted(GoalPtr goal, const vector<AsyncPipe*> & pipes,
    bool inBuildSlot, bool respectTimeouts)
#endif
{
    Child child;
    child.goal = goal;
    child.goal2 = goal.get();
#ifndef _WIN32
    child.fds = fds;
#else
    child.pipes = pipes;
#endif
    child.timeStarted = child.lastOutput = steady_time_point::clock::now();
    child.inBuildSlot = inBuildSlot;
    child.respectTimeouts = respectTimeouts;
    children.emplace_back(child);
    if (inBuildSlot) nrLocalBuilds++;
}


void Worker::childTerminated(Goal * goal, bool wakeSleepers)
{
    auto i = std::find_if(children.begin(), children.end(),
        [&](const Child & child) { return child.goal2 == goal; });
    if (i == children.end()) return;

    if (i->inBuildSlot) {
        assert(nrLocalBuilds > 0);
        nrLocalBuilds--;
    }

    children.erase(i);

    if (wakeSleepers) {

        /* Wake up goals waiting for a build slot. */
        for (auto & j : wantingToBuild) {
            GoalPtr goal = j.lock();
            if (goal) wakeUp(goal);
        }

        wantingToBuild.clear();
    }
}


void Worker::waitForBuildSlot(GoalPtr goal)
{
    debug("wait for build slot");
    if (getNrLocalBuilds() < settings.maxBuildJobs)
        wakeUp(goal); /* we can do it right away */
    else
        addToWeakGoals(wantingToBuild, goal);
}


void Worker::waitForAnyGoal(GoalPtr goal)
{
    debug("wait for any goal");
    addToWeakGoals(waitingForAnyGoal, goal);
}


void Worker::waitForAWhile(GoalPtr goal)
{
    debug("wait for a while");
    addToWeakGoals(waitingForAWhile, goal);
}


void Worker::run(const Goals & _topGoals)
{
//std::cerr << "~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~-Worker::run" << std::endl;

    for (auto & i : _topGoals) topGoals.insert(i);

    debug("entered goal loop");

    while (1) {

        checkInterrupt();

        store.autoGC(false);

        /* Call every wake goal (in the ordering established by
           CompareGoalPtrs). */
        while (!awake.empty() && !topGoals.empty()) {
            Goals awake2;
            for (auto & i : awake) {
                GoalPtr goal = i.lock();
                if (goal) awake2.insert(goal);
            }
            awake.clear();
            for (auto & goal : awake2) {
                checkInterrupt();
                goal->work();
                if (topGoals.empty()) break; // stuff may have been cancelled
            }
        }

        if (topGoals.empty()) break;

        /* Wait for input. */
        if (!children.empty() || !waitingForAWhile.empty())
            waitForInput();
        else {
            if (awake.empty() && 0 == settings.maxBuildJobs)
            {
                if (getMachines().empty())
                   throw Error("unable to start any build; either increase '--max-jobs' "
                            "or enable remote builds."
                            "\nhttps://nixos.org/nix/manual/#chap-distributed-builds");
                else
                   throw Error("unable to start any build; remote machines may not have "
                            "all required system features."
                            "\nhttps://nixos.org/nix/manual/#chap-distributed-builds");

            }
            assert(!awake.empty());
        }
    }

    /* If --keep-going is not set, it's possible that the main goal
       exited while some of its subgoals were still active.  But if
       --keep-going *is* set, then they must all be finished now. */
    assert(!settings.keepGoing || awake.empty());
    assert(!settings.keepGoing || wantingToBuild.empty());
    assert(!settings.keepGoing || children.empty());
}

#ifdef _WIN32
typedef BOOL (WINAPI * TGetQueuedCompletionStatusEx)(HANDLE, LPOVERLAPPED_ENTRY, ULONG, PULONG, DWORD, BOOL);

TGetQueuedCompletionStatusEx pGetQueuedCompletionStatusEx = (TGetQueuedCompletionStatusEx)(-1);
#endif

void Worker::waitForInput()
{
    printMsg(lvlVomit, "waiting for children");

//std::cerr << "~~~~~~~~~~~~--------------Worker::waitForInput() children.size()=" << children.size() << std::endl;

    /* Process output from the file descriptors attached to the
       children, namely log output and output path creation commands.
       We also use this to detect child termination: if we get EOF on
       the logger pipe of a build, we assume that the builder has
       terminated. */

    bool useTimeout = false;
    long timeout = 0;
    auto before = steady_time_point::clock::now();

    /* If we're monitoring for silence on stdout/stderr, or if there
       is a build timeout, then wait for input until the first
       deadline for any child. */
    auto nearest = steady_time_point::max(); // nearest deadline
    if (settings.minFree.get() != 0)
        // Periodicallty wake up to see if we need to run the garbage collector.
        nearest = before + std::chrono::seconds(10);
    for (auto & i : children) {
        if (!i.respectTimeouts) continue;
        if (0 != settings.maxSilentTime)
            nearest = std::min(nearest, i.lastOutput + std::chrono::seconds(settings.maxSilentTime));
        if (0 != settings.buildTimeout)
            nearest = std::min(nearest, i.timeStarted + std::chrono::seconds(settings.buildTimeout));
    }
    if (nearest != steady_time_point::max()) {
        timeout = std::max(1L, (long) std::chrono::duration_cast<std::chrono::seconds>(nearest - before).count());
        useTimeout = true;
    }

    /* If we are polling goals that are waiting for a lock, then wake
       up after a few seconds at most. */
    if (!waitingForAWhile.empty()) {
        useTimeout = true;
        if (lastWokenUp == steady_time_point::min() || lastWokenUp > before) lastWokenUp = before;
        timeout = std::max(1L,
            (long) std::chrono::duration_cast<std::chrono::seconds>(
                lastWokenUp + std::chrono::seconds(settings.pollInterval) - before).count());
    } else lastWokenUp = steady_time_point::min();

    if (useTimeout)
        vomit("sleeping %d seconds", timeout);

#ifndef _WIN32
    /* Use select() to wait for the input side of any logger pipe to
       become `available'.  Note that `available' (i.e., non-blocking)
       includes EOF. */
    std::vector<struct pollfd> pollStatus;
    std::map <int, int> fdToPollStatus;
    for (auto & i : children) {
        for (auto & j : i.fds) {
            pollStatus.push_back((struct pollfd) { .fd = j, .events = POLLIN });
            fdToPollStatus[j] = pollStatus.size() - 1;
        }
    }

    if (poll(pollStatus.data(), pollStatus.size(),
            useTimeout ? timeout * 1000 : -1) == -1) {
        if (errno == EINTR) return;
        throw PosixError("waiting for input");
    }
#else

    if (pGetQueuedCompletionStatusEx == (TGetQueuedCompletionStatusEx)(-1)) {
      pGetQueuedCompletionStatusEx = (TGetQueuedCompletionStatusEx) GetProcAddress(GetModuleHandle("kernel32"), "GetQueuedCompletionStatusEx");
//    pGetQueuedCompletionStatusEx = 0; // <- for testing
      assert(pGetQueuedCompletionStatusEx != (TGetQueuedCompletionStatusEx)(-1));
    }

    OVERLAPPED_ENTRY oentries[0x20] = {0};
    ULONG removed;
    bool gotEOF = false;

    if (pGetQueuedCompletionStatusEx != 0) {
        // we are on at least Windows Vista / Server 2008 and can get many (countof(oentries)) statuses in one API call
        if (!pGetQueuedCompletionStatusEx(ioport.get(), oentries, sizeof(oentries)/sizeof(*oentries), &removed, useTimeout ? (timeout.tv_sec*1000) : INFINITE, FALSE)) {
            WinError winError("GetQueuedCompletionStatusEx");
            if (winError.lastError != WAIT_TIMEOUT)
                throw winError;
            assert(removed == 0);
        } else {
            assert(0 < removed && removed <= sizeof(oentries)/sizeof(*oentries));
        }
    } else {
        // we are on legacy Windows and can get only one status in one API call
        if (!GetQueuedCompletionStatus(ioport.get(), &oentries[0].dwNumberOfBytesTransferred, &oentries[0].lpCompletionKey, &oentries[0].lpOverlapped, useTimeout ? (timeout.tv_sec*1000) : INFINITE)) {
            WinError winError("GetQueuedCompletionStatus");
            if (winError.lastError == ERROR_BROKEN_PIPE) {
                gotEOF = true;
                removed = 1;
            } else if (winError.lastError != WAIT_TIMEOUT)
                throw winError;
            removed = 0;
        } else {
            removed = 1;
        }
    }


/*
    std::cerr << "XXX removed       " << removed        << std::endl;
    for (ULONG i = 0; i<removed; i++ ) {
std::cerr << "lpCompletionKey               " << oentries[i].lpCompletionKey            << std::endl;
std::cerr << ".hEvent                       " << oentries[i].lpOverlapped->hEvent       << std::endl;
std::cerr << ".Offset                       " << oentries[i].lpOverlapped->Offset       << std::endl;
std::cerr << ".OffsetHigh                   " << oentries[i].lpOverlapped->OffsetHigh   << std::endl;
std::cerr << ".Internal                     " << oentries[i].lpOverlapped->Internal     << std::endl;
std::cerr << ".InternalHigh                 " << oentries[i].lpOverlapped->InternalHigh << std::endl;
std::cerr << "Internal                      " << oentries[i].Internal                   << std::endl;
std::cerr << "dwNumberOfBytesTransferred    " << oentries[i].dwNumberOfBytesTransferred << std::endl;
    }*/
/*
    DWORD bytesRead;
    ULONG_PTR completionKey;
    OVERLAPPED *poverlapped;
    if (!GetQueuedCompletionStatus(ioport.get(), &bytesRead, &completionKey, &poverlapped, useTimeout ? (timeout.tv_sec*1000) : INFINITE))
        throw WinError("GetQueuedCompletionStatus");
std::cerr << "completionKey " << completionKey             << std::endl;
std::cerr << ".hEvent       " << poverlapped->hEvent       << std::endl;
std::cerr << ".Offset       " << poverlapped->Offset       << std::endl;
std::cerr << ".OffsetHigh   " << poverlapped->OffsetHigh   << std::endl;
std::cerr << ".Internal     " << poverlapped->Internal     << std::endl;
std::cerr << ".InternalHigh " << poverlapped->InternalHigh << std::endl;
std::cerr << "bytesRead     " << bytesRead                 << std::endl;
*/

#endif
    auto after = steady_time_point::clock::now();

    /* Process all available file descriptors. FIXME: this is
       O(children * fds). */
    decltype(children)::iterator i;
    for (auto j = children.begin(); j != children.end(); j = i) {
        i = std::next(j);

        checkInterrupt();

        GoalPtr goal = j->goal.lock();
        assert(goal);

#ifndef _WIN32
        std::vector<unsigned char> buffer(4096);
        set<int> fds2(j->fds);
        for (auto & k : fds2) {
            if (pollStatus.at(fdToPollStatus.at(k)).revents) {
                ssize_t rd = ::read(k, buffer.data(), buffer.size());
                // FIXME: is there a cleaner way to handle pt close
                // than EIO? Is this even standard?
                if (rd == 0 || (rd == -1 && errno == EIO)) {
                    debug("%1%: got EOF", goal->getName());
                    goal->handleEOF(k);
                    j->fds.erase(k);
                } else if (rd == -1) {
                    if (errno != EINTR)
                        throw PosixError("%s: read failed", goal->getName());
                } else {
                    printMsg(lvlVomit, "%1%: read %2% bytes",
                        goal->getName(), rd);
                    string data((char *) buffer.data(), rd);
                    j->lastOutput = after;
                    goal->handleChildOutput(k, data);
                }
            }
        }
#else
        decltype(j->pipes)::iterator p = j->pipes.begin();
        while (p != j->pipes.end()) {
            decltype(p) nextp = p+1;
            for (ULONG i = 0; i<removed; i++) {
                if (oentries[i].lpCompletionKey == ((ULONG_PTR)((*p)->hRead.get()) ^ 0x5555)) {
                    if (gotEOF) {
                        debug(format("%1%: got EOF") % goal->getName());
                        goal->handleEOF((*p)->hRead.get());
                        nextp = j->pipes.erase(p); // no need to maintain `j->pipes` ?
                    } else {
                        if (oentries[i].dwNumberOfBytesTransferred > 0) {
                            printMsg(lvlVomit, format("%1%: read %2% bytes") % goal->getName() % oentries[i].dwNumberOfBytesTransferred);
                            string data((char *) (*p)->buffer.data(), oentries[i].dwNumberOfBytesTransferred);
                          //std::cerr << "read  [" << data << "]" << std::endl;
                            j->lastOutput = after;
                            goal->handleChildOutput((*p)->hRead.get(), data);
                        }

                        BOOL rc = ReadFile((*p)->hRead.get(), (*p)->buffer.data(), (*p)->buffer.size(), &(*p)->got, &(*p)->overlapped);
                        if (rc) {
                           // here is possible (but not obligatory) to call `goal->handleChildOutput` and repeat ReadFile immediately
                        } else {
                            WinError winError("ReadFile(%1%, ..)", (*p)->hRead.get());
                            if (winError.lastError == ERROR_BROKEN_PIPE) {
                                debug(format("%1%: got EOF") % goal->getName());
                                goal->handleEOF((*p)->hRead.get());
                                nextp = j->pipes.erase(p); // no need to maintain `j->pipes` ?
                            } else if (winError.lastError != ERROR_IO_PENDING)
                                throw winError;
                        }
                    }
                    break;
                }
            }
            p = nextp;
        }
#endif

        if (goal->exitCode == Goal::ecBusy &&
            0 != settings.maxSilentTime &&
            j->respectTimeouts &&
            after - j->lastOutput >= std::chrono::seconds(settings.maxSilentTime))
        {
            goal->timedOut(Error(
                    "%1% timed out after %2% seconds of silence",
                    goal->getName(), settings.maxSilentTime));
        }

        else if (goal->exitCode == Goal::ecBusy &&
            0 != settings.buildTimeout &&
            j->respectTimeouts &&
            after - j->timeStarted >= std::chrono::seconds(settings.buildTimeout))
        {
            goal->timedOut(Error(
                    "%1% timed out after %2% seconds",
                    goal->getName(), settings.buildTimeout));
        }
    }

    if (!waitingForAWhile.empty() && lastWokenUp + std::chrono::seconds(settings.pollInterval) <= after) {
        lastWokenUp = after;
        for (auto & i : waitingForAWhile) {
            GoalPtr goal = i.lock();
            if (goal) wakeUp(goal);
        }
        waitingForAWhile.clear();
    }
//std::cerr << "~~~~~~~~~~~~/////---------Worker::waitForInput() children.size()=" << children.size() << std::endl;
}


unsigned int Worker::exitStatus()
{
    /*
     * 1100100
     *    ^^^^
     *    |||`- timeout
     *    ||`-- output hash mismatch
     *    |`--- build failure
     *    `---- not deterministic
     */
    unsigned int mask = 0;
    bool buildFailure = permanentFailure || timedOut || hashMismatch;
    if (buildFailure)
        mask |= 0x04;  // 100
    if (timedOut)
        mask |= 0x01;  // 101
    if (hashMismatch)
        mask |= 0x02;  // 102
    if (checkMismatch) {
        mask |= 0x08;  // 104
    }

    if (mask)
        mask |= 0x60;
    return mask ? mask : 1;
}


bool Worker::pathContentsGood(const StorePath & path)
{
    auto i = pathContentsGoodCache.find(path);
    if (i != pathContentsGoodCache.end()) return i->second;
    printInfo("checking path '%s'...", store.printStorePath(path));
    auto info = store.queryPathInfo(path);
    bool res;
    if (!pathExists(store.printStorePath(path)))
        res = false;
    else {
        HashResult current = hashPath(info->narHash.type, store.printStorePath(path));
        Hash nullHash(htSHA256);
        res = info->narHash == nullHash || info->narHash == current.first;
    }
    pathContentsGoodCache.insert_or_assign(path, res);
    if (!res)
        logError({
            .name = "Corrupted path",
            .hint = hintfmt("path '%s' is corrupted or missing!", store.printStorePath(path))
        });
    return res;
}


void Worker::markContentsGood(const StorePath & path)
{
    pathContentsGoodCache.insert_or_assign(path, true);
}


GoalPtr upcast_goal(std::shared_ptr<SubstitutionGoal> subGoal) {
    return subGoal;
}

}