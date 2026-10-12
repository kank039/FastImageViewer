#pragma once
// The cache brain. Runs on the UI thread only; workers never touch it.
//
// Every frame-relevant event (navigation, a finished job, a budget change)
// calls Pump() with the ordered list of things that *should* be cached. The
// scheduler diffs that against what exists and tells the host to start,
// cancel or release work. Byte accounting is exact because every Want carries
// a DecodePlan computed from the probed image header.
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "DecodePlan.h"
#include "MemoryLimits.h"

namespace fiv::core {

// Fit: screen-sized; Full: full resolution when zoomed in; Thumb: grid and sidebar thumbnails.
enum class Variant : uint8_t { Fit = 0, Full = 1, Thumb = 2 };

struct EntryKey {
    uint64_t item = 0;  // file version id (changes when the file changes)
    Variant variant = Variant::Fit;
    bool operator==(const EntryKey& o) const { return item == o.item && variant == o.variant; }
};

struct EntryKeyHash {
    size_t operator()(const EntryKey& k) const { return std::hash<uint64_t>()(k.item * 4 + uint64_t(k.variant)); }
};

struct Want {
    EntryKey key;
    int rank = 0;                // 0 = most important
    bool sizeKnown = false;      // header probed; plan/transient valid
    DecodePlan plan;
    uint64_t transientBytes = 0;
    bool mayProbe = true;        // allowed to read the header now
    bool mayLoad = true;         // allowed to start a decode now (keeps resident data either way)
};

struct SchedulerConfig {
    int maxDecodes = 2;  // concurrent prefetch decodes; rank 0 gets one extra slot
    int maxProbes = 2;
};

class SchedulerHost {
public:
    virtual ~SchedulerHost() = default;
    virtual void StartProbe(uint64_t job, uint64_t item, int rank) = 0;
    virtual void StartDecode(uint64_t job, const EntryKey& key, const DecodePlan& plan, int rank) = 0;
    virtual void CancelJob(uint64_t job) = 0;
    virtual void SetJobPriority(uint64_t job, int rank) = 0;
    virtual void ReleaseEntry(const EntryKey& key) = 0;  // drop the resident texture
};

enum class EntryState : uint8_t { Decoding, Resident, Failed };

enum class Outcome : uint8_t { Ok, Failed, Retry, Cancelled };

class Scheduler {
public:
    explicit Scheduler(SchedulerHost& host) : host_(host) {}

    void SetConfig(const SchedulerConfig& c) { cfg_ = c; }
    const SchedulerConfig& Config() const { return cfg_; }

    // wants must be sorted by rank. prefetchAllowed=false limits new work to rank 0
    // (used while the user is skimming quickly).
    void Pump(const std::vector<Want>& wants, const Limits& limits, bool prefetchAllowed);

    void OnProbeDone(uint64_t job);
    // Returns true if the host should keep the produced texture.
    bool OnDecodeDone(uint64_t job, Outcome outcome);

    // Drops everything (folder switch, device loss). Running jobs become zombies.
    void Clear();

    // Accounting
    uint64_t ResidentGpu() const;       // texture bytes that exist right now
    uint64_t CommittedGpu() const;      // resident + promised to running decodes
    uint64_t TransientInUse() const;    // RAM reserved by running decodes
    int RunningDecodes() const;
    int RunningProbes() const { return int(probes_.size()); }

    // RAM for one-off work outside the cache (copying an image to the
    // clipboard, a sharpness pass). It counts as transient, so decodes leave
    // room for it until it is released.
    uint64_t TransientRoom(const Limits& limits) const;  // what a reservation may take now
    void ReserveTransient(uint64_t bytes) { extraTransient_ += bytes; }
    void ReleaseTransient(uint64_t bytes) { extraTransient_ -= bytes < extraTransient_ ? bytes : extraTransient_; }
    bool HasWork() const { return RunningDecodes() > 0 || !probes_.empty(); }

    struct EntryView {
        EntryKey key;
        EntryState state;
        int rank;
        DecodePlan plan;
    };
    std::vector<EntryView> Entries() const;
    bool IsResident(const EntryKey& k) const;
    bool IsFailed(const EntryKey& k) const;
    bool IsDecoding(const EntryKey& k) const;
    bool IsProbing(uint64_t item) const { return probeItems_.count(item) != 0; }

private:
    struct Entry {
        EntryState state = EntryState::Decoding;
        uint64_t job = 0;
        DecodePlan plan;
        uint64_t transient = 0;
        int rank = 0;
    };
    struct Zombie {
        uint64_t gpu = 0;
        uint64_t transient = 0;
    };

    void Drop(const EntryKey& key);

    SchedulerHost& host_;
    SchedulerConfig cfg_;
    uint64_t nextJob_ = 1;
    std::unordered_map<EntryKey, Entry, EntryKeyHash> entries_;
    std::unordered_map<uint64_t, EntryKey> jobToKey_;
    std::unordered_map<uint64_t, Zombie> zombies_;    // cancelled decodes still running
    std::unordered_map<uint64_t, uint64_t> probes_;   // job -> item
    std::unordered_map<uint64_t, uint64_t> probeItems_;  // item -> job
    uint64_t extraTransient_ = 0;
};

}  // namespace fiv::core
