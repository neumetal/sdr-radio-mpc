#pragma once
// The Saved and Blocked channels, shared by SDR Radio and SDR Haze and by
// every project: one small text file in the Synths folder that holds both
// plugin folders (".sdrlab-channels.txt", next to them). Each plugin instance
// keeps a copy, writes the file on a background thread after a change
// (write a temp file, then rename, so a crash can't leave half a file), and
// reloads it when another instance changed it.
//
//   # SdrLab channels (SDR Radio, SDR Haze): S = saved, B = blocked
//   S 162550000 2        (Hz, mode: 0 FM, 1 AM, 2 NFM)
//   B 155250000
//
// Without a folder (a host test) it works in memory only.

#include "SdrReceiver.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <vector>

class ChannelStore
{
public:
    using Channel = SdrReceiver::Channel;
    static constexpr int kMaxSaved = 512, kMaxBlocked = 512;

    ~ChannelStore()
    {
        {
            std::lock_guard<std::mutex> lock (mutex);
            quit = true;
        }
        wake.notify_all();
        if (writer.joinable()) writer.join();
    }

    // dataDir: the plugin's own folder (the wrapper's MODULE_SUBDIR "."); the file goes one level up.
    void open (const char* dataDir)
    {
        if (dataDir && *dataDir)
        {
            path = std::string (dataDir) + "/../.sdrlab-channels.txt";
            load();
            writer = std::thread ([this] { writeLoop(); });
        }
    }

    const std::vector<Channel>& saved() const     { return savedList; }
    const std::vector<uint32_t>& blocked() const  { return blockedList; }
    int generation() const                        { return gen; }       // bumps on every change, here or from the file

    static bool near (uint32_t a, uint32_t b)     { return (a > b ? a - b : b - a) < 500u; }
    // Both lists are kept sorted, so these are binary searches (a scan list can be thousands of channels).
    bool isSaved (uint32_t hz) const
    {
        auto it = std::lower_bound (savedList.begin(), savedList.end(), hz >= 500u ? hz - 499u : 0u,
                                    [] (const Channel& c, uint32_t h) { return c.hz < h; });
        return it != savedList.end() && near (it->hz, hz);
    }
    bool isBlocked (uint32_t hz) const
    {
        auto it = std::lower_bound (blockedList.begin(), blockedList.end(), hz >= 500u ? hz - 499u : 0u);
        return it != blockedList.end() && near (*it, hz);
    }

    // KEEP: saved (and no longer blocked). Returns false if it already was.
    bool keep (Channel c)
    {
        const bool unblocked = removeBlocked (c.hz);
        if (isSaved (c.hz)) { if (unblocked) changed(); return unblocked; }
        if (savedList.size() >= (size_t) kMaxSaved) return false;
        insertSorted (c);
        changed();
        return true;
    }

    // A survey find: saved unless it's blocked or saved already.
    bool addFound (Channel c)
    {
        if (isBlocked (c.hz) || isSaved (c.hz) || savedList.size() >= (size_t) kMaxSaved) return false;
        insertSorted (c);
        changed();
        return true;
    }

    // BLOCK: no longer saved, and skipped by every scan.
    void block (uint32_t hz)
    {
        savedList.erase (std::remove_if (savedList.begin(), savedList.end(), [hz] (const Channel& c) { return near (c.hz, hz); }), savedList.end());
        if (! isBlocked (hz) && blockedList.size() < (size_t) kMaxBlocked)
            blockedList.insert (std::upper_bound (blockedList.begin(), blockedList.end(), hz), hz);
        changed();
    }

    void unblock (uint32_t hz)  { if (removeBlocked (hz)) changed(); }
    void clearSaved()           { if (! savedList.empty())   { savedList.clear();   changed(); } }
    void clearBlocked()         { if (! blockedList.empty()) { blockedList.clear(); changed(); } }

    // Every couple of seconds: reload if another instance (or the other plugin) wrote the file.
    void poll()
    {
        if (path.empty()) return;
        {
            std::lock_guard<std::mutex> lock (mutex);
            if (dirty) return;                     // ours is newer; it will be written shortly
        }
        struct stat st {};
        if (::stat (path.c_str(), &st) == 0 && (long long) st.st_mtime * 1000000000LL + mtimeNs (st) != fileTime)
            load();
    }

private:
    std::string path;
    std::vector<Channel> savedList;
    std::vector<uint32_t> blockedList;
    int gen = 0;
    std::atomic<long long> fileTime { 0 };      // the file's mtime when we last read or wrote it (the writer thread sets it too)

    std::mutex mutex;                              // guards the copy below, dirty and quit
    std::condition_variable wake;
    std::thread writer;
    bool dirty = false, quit = false;
    std::vector<Channel> pendingSaved;
    std::vector<uint32_t> pendingBlocked;

    static long long mtimeNs (const struct stat& st)
    {
#if defined (__APPLE__)
        return st.st_mtimespec.tv_nsec;
#else
        return st.st_mtim.tv_nsec;
#endif
    }

    void insertSorted (Channel c)
    {
        savedList.insert (std::upper_bound (savedList.begin(), savedList.end(), c,
                                            [] (const Channel& a, const Channel& b) { return a.hz < b.hz; }), c);
    }

    bool removeBlocked (uint32_t hz)
    {
        const size_t before = blockedList.size();
        blockedList.erase (std::remove_if (blockedList.begin(), blockedList.end(), [hz] (uint32_t b) { return near (b, hz); }), blockedList.end());
        return blockedList.size() != before;
    }

    void changed()
    {
        ++gen;
        if (path.empty()) return;
        {
            std::lock_guard<std::mutex> lock (mutex);
            pendingSaved = savedList;
            pendingBlocked = blockedList;
            dirty = true;
        }
        wake.notify_all();
    }

    void load()
    {
        FILE* f = std::fopen (path.c_str(), "r");
        if (! f) return;
        std::vector<Channel> s;
        std::vector<uint32_t> b;
        char line[128];
        while (std::fgets (line, sizeof line, f))
        {
            char kind = 0; unsigned long long hz = 0; int mode = 0;
            const int n = std::sscanf (line, " %c %llu %d", &kind, &hz, &mode);
            if (n >= 2 && hz >= 100000ULL && hz <= 2000000000ULL)
            {
                if (kind == 'S' && s.size() < (size_t) kMaxSaved) s.push_back ({ (uint32_t) hz, std::clamp (n == 3 ? mode : 0, 0, 2) });
                else if (kind == 'B' && b.size() < (size_t) kMaxBlocked) b.push_back ((uint32_t) hz);
            }
        }
        std::fclose (f);
        std::sort (s.begin(), s.end(), [] (const Channel& x, const Channel& y) { return x.hz < y.hz; });
        std::sort (b.begin(), b.end());
        savedList = std::move (s);
        blockedList = std::move (b);
        struct stat st {};
        if (::stat (path.c_str(), &st) == 0) fileTime = (long long) st.st_mtime * 1000000000LL + mtimeNs (st);
        ++gen;
    }

    // The writer thread: after a change, waits a moment (a burst of BLOCKs is one write), then writes the file.
    void writeLoop()
    {
        std::unique_lock<std::mutex> lock (mutex);
        while (! quit)
        {
            wake.wait (lock, [this] { return dirty || quit; });
            if (quit && ! dirty) break;
            wake.wait_for (lock, std::chrono::milliseconds (300), [this] { return quit; });
            const auto s = pendingSaved;
            const auto b = pendingBlocked;
            dirty = false;
            lock.unlock();

            const std::string tmp = path + ".new";
            if (FILE* f = std::fopen (tmp.c_str(), "w"))
            {
                std::fprintf (f, "# SdrLab channels (SDR Radio, SDR Haze): S = saved (Hz, mode 0 FM 1 AM 2 NFM), B = blocked (Hz)\n");
                for (const auto& c : s) std::fprintf (f, "S %u %d\n", c.hz, c.mode);
                for (const auto h : b)  std::fprintf (f, "B %u\n", h);
                const bool ok = std::fflush (f) == 0;
                std::fclose (f);
                if (ok && std::rename (tmp.c_str(), path.c_str()) == 0)
                {
                    struct stat st {};
                    lock.lock();
                    if (::stat (path.c_str(), &st) == 0) fileTime = (long long) st.st_mtime * 1000000000LL + mtimeNs (st);
                    continue;
                }
            }
            lock.lock();
        }
    }
};
