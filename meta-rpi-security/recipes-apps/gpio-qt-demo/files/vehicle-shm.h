// ============================================================================
// vehicle-shm.h  -  shared by the CAN application (writer) and the display
//                   application (reader).
//
//  * Seqlock      : reader never sees a half-written VehicleData (torn read)
//  * Futex wake   : reader sleeps in the kernel and is woken the instant the
//                   writer publishes -> no polling timer anywhere
//  * Header check : magic / version / size guard against mismatched builds
//
// Single writer, any number of readers.
// ============================================================================
#pragma once

#include <atomic>
#include <climits>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <type_traits>

#include <fcntl.h>
#include <sched.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <linux/futex.h>

#include "vehicle-data.h"

namespace vshm {

inline constexpr const char *NAME    = "/vehicle_data";
inline constexpr uint32_t    MAGIC   = 0x56454831u; // "VEH1"
inline constexpr uint32_t    VERSION = 1;

static_assert(std::is_trivially_copyable_v<VehicleData>,
              "VehicleData must be plain data (no QString/std::string/pointers) "
              "to be shared between processes");
static_assert(ATOMIC_INT_LOCK_FREE == 2, "need lock-free 32-bit atomics");

struct Block
{
    uint32_t              magic;
    uint32_t              version;
    uint32_t              payloadSize;
    uint32_t              reserved;
    std::atomic<uint32_t> seq;   // even = stable, odd = writer busy. Also the futex word.
    VehicleData           data;
};

// ---------------------------------------------------------------------------
// futex helpers (shared, i.e. NOT FUTEX_PRIVATE - works across processes)
// ---------------------------------------------------------------------------
inline void futexWait(const std::atomic<uint32_t> *addr, uint32_t expected, int timeoutMs)
{
    timespec ts;
    ts.tv_sec  = timeoutMs / 1000;
    ts.tv_nsec = static_cast<long>(timeoutMs % 1000) * 1000000L;

    syscall(SYS_futex, reinterpret_cast<const uint32_t *>(addr),
            FUTEX_WAIT, expected, &ts, nullptr, 0);
}

inline void futexWakeAll(std::atomic<uint32_t> *addr)
{
    syscall(SYS_futex, reinterpret_cast<uint32_t *>(addr),
            FUTEX_WAKE, INT_MAX, nullptr, nullptr, 0);
}

// ---------------------------------------------------------------------------
// Writer (CAN application)
// ---------------------------------------------------------------------------
class Writer
{
public:
    ~Writer() { release(); }

    bool create()
    {
        fd_ = shm_open(NAME, O_CREAT | O_RDWR | O_CLOEXEC, 0666);
        if (fd_ < 0) { perror("shm_open"); return false; }

        if (ftruncate(fd_, sizeof(Block)) < 0) { perror("ftruncate"); release(); return false; }

        void *p = mmap(nullptr, sizeof(Block), PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        if (p == MAP_FAILED) { perror("mmap"); release(); return false; }
        blk_ = static_cast<Block *>(p);

        // Only (re)initialise a fresh or foreign segment, so readers that are
        // already attached survive a restart of the writer.
        if (blk_->magic != MAGIC || blk_->version != VERSION ||
            blk_->payloadSize != sizeof(VehicleData))
        {
            std::memset(static_cast<void *>(blk_), 0, sizeof(Block));
            blk_->version     = VERSION;
            blk_->payloadSize = sizeof(VehicleData);
            blk_->magic       = MAGIC;
        }
        return true;
    }

    // Single writer only. Safe to call at any rate; also works as a heartbeat.
    void publish(const VehicleData &d)
    {
        if (!blk_) return;

        const uint32_t s = blk_->seq.load(std::memory_order_relaxed) & ~1u; // heal odd value
        blk_->seq.store(s + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        std::memcpy(static_cast<void *>(&blk_->data), &d, sizeof(VehicleData));
        blk_->seq.store(s + 2, std::memory_order_release);

        futexWakeAll(&blk_->seq);
    }

    void release()
    {
        if (blk_) { munmap(blk_, sizeof(Block)); blk_ = nullptr; }
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
        // Deliberately no shm_unlink(): the segment must outlive this process.
    }

private:
    int    fd_  = -1;
    Block *blk_ = nullptr;
};

// ---------------------------------------------------------------------------
// Reader (display application)
// ---------------------------------------------------------------------------
class Reader
{
public:
    ~Reader() { release(); }

    bool isOpen() const { return blk_ != nullptr; }

    bool open()
    {
        release();

        fd_ = shm_open(NAME, O_RDONLY | O_CLOEXEC, 0);
        if (fd_ < 0) return false;

        struct stat st {};
        // Mapping past EOF would SIGBUS, so check the size first.
        if (fstat(fd_, &st) != 0 || static_cast<size_t>(st.st_size) < sizeof(Block))
        { release(); return false; }

        void *p = mmap(nullptr, sizeof(Block), PROT_READ, MAP_SHARED, fd_, 0);
        if (p == MAP_FAILED) { release(); return false; }
        blk_ = static_cast<const Block *>(p);

        if (blk_->magic != MAGIC || blk_->version != VERSION ||
            blk_->payloadSize != sizeof(VehicleData))
        { release(); return false; }

        return true;
    }

    void release()
    {
        if (blk_) { munmap(const_cast<Block *>(blk_), sizeof(Block)); blk_ = nullptr; }
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    }

    uint32_t seq() const { return blk_->seq.load(std::memory_order_acquire); }

    // Consistent snapshot (seqlock read). False if the writer was too busy.
    bool read(VehicleData &out, uint32_t &seqOut) const
    {
        for (int attempt = 0; attempt < 16; ++attempt)
        {
            const uint32_t s1 = blk_->seq.load(std::memory_order_acquire);
            if (s1 & 1u) { sched_yield(); continue; }

            std::memcpy(&out, &blk_->data, sizeof(VehicleData));
            std::atomic_thread_fence(std::memory_order_acquire);

            if (blk_->seq.load(std::memory_order_relaxed) == s1)
            {
                seqOut = s1;
                return true;
            }
        }
        return false;
    }

    // Sleep in the kernel until seq != seen (writer published) or timeout.
    void waitChange(uint32_t seen, int timeoutMs) const
    {
        futexWait(&blk_->seq, seen, timeoutMs);
    }

private:
    int          fd_  = -1;
    const Block *blk_ = nullptr;
};

} // namespace vshm