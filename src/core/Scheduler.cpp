#include "Scheduler.h"

namespace fiv::core {

void Scheduler::Drop(const EntryKey& key) {
    auto it = entries_.find(key);
    if (it == entries_.end()) return;
    const Entry e = it->second;
    entries_.erase(it);
    if (e.state == EntryState::Decoding) {
        // The worker may already be writing pixels; its bytes stay counted
        // until it reports back, so cancelling never frees memory early.
        zombies_[e.job] = {e.plan.gpuBytes, e.transient};
        jobToKey_.erase(e.job);
        host_.CancelJob(e.job);
    } else if (e.state == EntryState::Resident) {
        host_.ReleaseEntry(key);
    }
}

void Scheduler::Pump(const std::vector<Want>& wants, const Limits& limits, bool prefetchAllowed) {
    std::unordered_map<EntryKey, const Want*, EntryKeyHash> wanted;
    std::unordered_set<uint64_t> wantedItems;
    wanted.reserve(wants.size());
    for (const Want& w : wants) {
        wanted.emplace(w.key, &w);
        wantedItems.insert(w.key.item);
    }

    // 1. Release what fell out of the window or needs a different size.
    std::vector<EntryKey> drop;
    for (const auto& [key, e] : entries_) {
        auto it = wanted.find(key);
        if (it == wanted.end()) {
            drop.push_back(key);
        } else if (it->second->sizeKnown && it->second->plan != e.plan) {
            drop.push_back(key);
        }
    }
    for (const EntryKey& k : drop) Drop(k);

    for (const auto& [job, item] : probes_) {
        if (!wantedItems.count(item) && probeItems_.count(item) && probeItems_[item] == job) {
            host_.CancelJob(job);
            probeItems_.erase(item);  // allow a fresh probe if it comes back into view
        }
    }

    // 2. Admission in priority order. Only higher-ranked wants are counted
    //    before a lower-ranked one, so a lower rank can never block a higher one.
    const uint64_t gpuLimit = limits.unified ? limits.ram : limits.gpu;
    uint64_t admitted = 0;
    for (const auto& [job, z] : zombies_) admitted += z.gpu;

    std::vector<const Want*> pending;
    for (const Want& w : wants) {
        auto it = entries_.find(w.key);
        const bool exists = it != entries_.end();
        if (exists && it->second.state == EntryState::Failed) continue;

        if (!w.sizeKnown) {
            const int probeCap = cfg_.maxProbes + (w.rank == 0 ? 1 : 0);
            if (!exists && w.mayProbe && (w.rank == 0 || prefetchAllowed) && !probeItems_.count(w.key.item) &&
                int(probes_.size()) < probeCap) {
                const uint64_t job = nextJob_++;
                probes_[job] = w.key.item;
                probeItems_[w.key.item] = job;
                host_.StartProbe(job, w.key.item, w.rank);
            }
            continue;
        }
        if (!exists && (!w.mayLoad || (w.rank != 0 && !prefetchAllowed))) continue;

        if (admitted + w.plan.gpuBytes > gpuLimit) {
            if (exists) Drop(w.key);  // budget shrank: evict lowest priority first
            continue;
        }
        admitted += w.plan.gpuBytes;
        if (exists) {
            Entry& e = it->second;
            if (e.rank != w.rank) {
                e.rank = w.rank;
                if (e.state == EntryState::Decoding) host_.SetJobPriority(e.job, w.rank);
            }
            continue;
        }
        pending.push_back(&w);
    }

    // 3. Dispatch admitted work while decode slots and transient RAM allow.
    int running = int(zombies_.size());
    uint64_t transient = 0, committed = 0;
    for (const auto& [job, z] : zombies_) {
        transient += z.transient;
        committed += z.gpu;
    }
    for (const auto& [key, e] : entries_) {
        if (e.state == EntryState::Decoding) {
            ++running;
            transient += e.transient;
        }
        if (e.state != EntryState::Failed) committed += e.plan.gpuBytes;
    }

    for (const Want* w : pending) {
        const int cap = cfg_.maxDecodes + (w->rank == 0 ? 1 : 0);
        if (running >= cap) continue;
        bool fits;
        if (limits.unified) {
            fits = committed + transient + w->plan.gpuBytes + w->transientBytes <= limits.ram;
        } else {
            fits = transient + w->transientBytes <= limits.ram && committed + w->plan.gpuBytes <= limits.gpu;
        }
        // The image on screen must always be decodable: alone, it may briefly
        // exceed the transient pool (its texture still fits the texture pool).
        if (!fits && w->rank == 0 && running == 0 && committed + w->plan.gpuBytes <= gpuLimit) fits = true;
        // Strict priority: lower ranks must not keep the pool busy while a
        // higher rank waits for memory, so running work drains and it gets in.
        if (!fits) break;

        const uint64_t job = nextJob_++;
        Entry e;
        e.state = EntryState::Decoding;
        e.job = job;
        e.plan = w->plan;
        e.transient = w->transientBytes;
        e.rank = w->rank;
        entries_[w->key] = e;
        jobToKey_[job] = w->key;
        ++running;
        transient += w->transientBytes;
        committed += w->plan.gpuBytes;
        host_.StartDecode(job, w->key, w->plan, w->rank);
    }
}

void Scheduler::OnProbeDone(uint64_t job) {
    auto it = probes_.find(job);
    if (it == probes_.end()) return;
    auto pi = probeItems_.find(it->second);
    if (pi != probeItems_.end() && pi->second == job) probeItems_.erase(pi);
    probes_.erase(it);
}

bool Scheduler::OnDecodeDone(uint64_t job, Outcome outcome) {
    if (zombies_.erase(job)) return false;
    auto jk = jobToKey_.find(job);
    if (jk == jobToKey_.end()) return false;
    const EntryKey key = jk->second;
    jobToKey_.erase(jk);
    auto it = entries_.find(key);
    if (it == entries_.end()) return false;
    switch (outcome) {
        case Outcome::Ok:
            it->second.state = EntryState::Resident;
            return true;
        case Outcome::Failed:
            it->second.state = EntryState::Failed;
            return false;
        default:
            entries_.erase(it);
            return false;
    }
}

void Scheduler::Clear() {
    std::vector<EntryKey> keys;
    for (const auto& [k, e] : entries_) keys.push_back(k);
    for (const EntryKey& k : keys) Drop(k);
    entries_.clear();
    for (const auto& [item, job] : probeItems_) host_.CancelJob(job);
    probeItems_.clear();
}

uint64_t Scheduler::ResidentGpu() const {
    uint64_t t = 0;
    for (const auto& [k, e] : entries_)
        if (e.state == EntryState::Resident) t += e.plan.gpuBytes;
    return t;
}

uint64_t Scheduler::CommittedGpu() const {
    uint64_t t = 0;
    for (const auto& [k, e] : entries_)
        if (e.state != EntryState::Failed) t += e.plan.gpuBytes;
    for (const auto& [j, z] : zombies_) t += z.gpu;
    return t;
}

uint64_t Scheduler::TransientInUse() const {
    uint64_t t = 0;
    for (const auto& [k, e] : entries_)
        if (e.state == EntryState::Decoding) t += e.transient;
    for (const auto& [j, z] : zombies_) t += z.transient;
    return t;
}

int Scheduler::RunningDecodes() const {
    int n = int(zombies_.size());
    for (const auto& [k, e] : entries_)
        if (e.state == EntryState::Decoding) ++n;
    return n;
}

std::vector<Scheduler::EntryView> Scheduler::Entries() const {
    std::vector<EntryView> v;
    v.reserve(entries_.size());
    for (const auto& [k, e] : entries_) v.push_back({k, e.state, e.rank, e.plan});
    return v;
}

bool Scheduler::IsResident(const EntryKey& k) const {
    auto it = entries_.find(k);
    return it != entries_.end() && it->second.state == EntryState::Resident;
}

bool Scheduler::IsFailed(const EntryKey& k) const {
    auto it = entries_.find(k);
    return it != entries_.end() && it->second.state == EntryState::Failed;
}

bool Scheduler::IsDecoding(const EntryKey& k) const {
    auto it = entries_.find(k);
    return it != entries_.end() && it->second.state == EntryState::Decoding;
}

}  // namespace fiv::core
