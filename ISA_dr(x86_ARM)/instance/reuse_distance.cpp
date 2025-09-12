/* **********************************************************
 * Copyright (c) 2016-2020 Google, Inc.  All rights reserved.
 * **********************************************************/

/*
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 *
 * * Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 *
 * * Neither the name of Google, Inc. nor the names of its contributors may be
 *   used to endorse or promote products derived from this software without
 *   specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL VMWARE, INC. OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 */

#include <iostream>
#include <stdio.h>
#include <cmath>
#include <cstring>
#include <stddef.h>
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>
#include <string.h>
#include <list>
#include <cstdio>
#include <cstdlib>
#include <cassert>
#include <iostream>
#include <functional>
#include <pthread.h>
#include <unordered_map>
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <vector>
#include "reuse_distance.h"
#include "../common/utils.h"

//const int INST_DIST_STEP = 30;
const int INST_DIST_STEP = 40;
const uint64_t step_1 = 1 << 20;
const uint64_t LRU_CACHE_MAX_SIZE = step_1 << 20;
//const uint64_t LRU_CACHE_MAX_SIZE = 1 << INST_DIST_STEP;

uint64_t cur_inst_pc = 0;                     // instructon program counter
uint64_t reuse_dist_sum[1000000];
uint64_t reuse_inst_num = 0;                  // reused instruction counter
bool lru_cache_inited = false;
uint64_t inst_dist_count[INST_DIST_STEP + 2]; // 最后一个用于最开始的初始
// uint64_t reuse_dist_ssum[1000000];
__uint128_t reuse_dist_ssum = 0;
int loc_id = 0;


namespace lru {
/**
 * a base lock class that can be overridden
 */
class Lock {
public:
    virtual ~Lock() {
    }
    virtual void lock() = 0;
    virtual void unlock() = 0;
protected:
    Lock() {
    }
private:
    Lock(const Lock&);
    const Lock& operator =(const Lock&);
};

/**
 *    Null or noop lock class that is the default synchronization
 *    used in lru cache.
 *
 *    a simple pthread based lock would look like this:
 *    <pre>
 *        class MutexLock : public Lock {
 public:
 MutexLock() { ::pthread_mutex_init(&m_lock, 0); }
 virtual ~MutexLock() { ::pthread_mutex_destroy(&m_lock); }
 void lock() { ::pthread_mutex_lock(&m_lock); }
 void unlock() { ::pthread_mutex_unlock(&m_lock); }
 private:
 pthread_mutex_t m_lock;
 }
 *    </pre>
 *
 */
class NullLock : public Lock
{
public:
    NullLock() {}
    virtual ~NullLock() {}
    virtual void lock() override {}
    virtual void unlock() override {}
};

/**
 *    helper class to auto lock and unlock within a scope
 */
class ScopedLock {
public:
    ScopedLock(Lock& lock) :
            m_lock(lock) {
        m_lock.lock();
    }
    virtual ~ScopedLock() {
        m_lock.unlock();
    }
private:
    Lock& m_lock;
    ScopedLock(const ScopedLock&);
    const ScopedLock& operator =(const ScopedLock&);
};

class MutexLock : public Lock {
public:
    MutexLock() { ::pthread_mutex_init(&m_lock, 0); }
    virtual ~MutexLock() { ::pthread_mutex_destroy(&m_lock); }
    virtual void lock() override { ::pthread_mutex_lock(&m_lock); }
    virtual void unlock() override { ::pthread_mutex_unlock(&m_lock); }

private:
    pthread_mutex_t m_lock;
};

/**
 *    The LRU Cache class templated by
 *        Key - key type
 *        Value - value type
 *        MapType - an associative container like std::map
 *        LockType - a lock type derived from the Lock class (default: NullLock = no synchronization)
 */
template<class Key, class Value, class LockType = NullLock>
class Cache {

private:
    typedef std::pair<Key, Value> ListNode;
    typedef std::list<ListNode> List;
    typedef typename List::iterator ListIterator;
    typedef std::unordered_map<Key, ListIterator> MapType;

    mutable LockType m_lock;
    MapType m_cache;
    List m_keys;
    size_t m_maxSize;
    size_t m_elasticity;

public:

    // class KeyNotFound: public std::exception {
    // public:
    //     const char* what() const throw () {
    //         return "KeyNotFound";
    //     }
    // };
    // -- methods
    Cache(size_t maxSize = 64, size_t elasticity = 10) :
            m_maxSize(maxSize), m_elasticity(elasticity) {
    }
    virtual ~Cache() = default;
    void clear() {
        ScopedLock scoped(m_lock);
        m_cache.clear();
        m_keys.clear();
    }
    void insert(const Key& key, const Value& value) {
        ScopedLock scoped(m_lock);
        auto iter = m_cache.find(key);
        if (iter != m_cache.end()) {
            m_keys.erase(iter->second);
            m_keys.push_front(std::make_pair(key, value));
            m_cache[key] = m_keys.begin();
        } else {
            m_keys.push_front(std::make_pair(key, value));
            m_cache[key] = m_keys.begin();
            prune();
        }

    }
    bool tryGet(const Key& key, Value& value, bool fresh_key=true) {
        ScopedLock scoped(m_lock);
        auto iter = m_cache.find(key);
        if (iter == m_cache.end()) {
            return false;
        } else {
            value = iter->second->second;
            if (fresh_key) {
                m_keys.erase(iter->second);
                m_keys.push_front(std::make_pair(key, value));
                m_cache[key] = m_keys.begin();
            }
            return true;
        }
    }
    const Value& get(const Key& key, bool fresh_key=true) {
        ScopedLock scoped(m_lock);
        auto iter = m_cache.find(key);
        if (iter == m_cache.end()) { 
            // throw KeyNotFound();
			std::cerr <<"KeyNotFound" << std::endl;
			abort();
        }
        auto value = iter->second->second;
        if (fresh_key) {
            m_keys.erase(iter->second);
            m_keys.push_front(std::make_pair(key, value));
            m_cache[key] = m_keys.begin();
        }
        return m_cache[key]->second;
    }
    void remove(const Key& key) {
        ScopedLock scoped(m_lock);
        auto iter = m_cache.find(key);
        if (iter != m_cache.end()) {
            m_keys.erase(iter->second);
            m_cache.remove(key);
        }
    }
    bool contains(const Key& key) {
        return m_cache.find(key) != m_cache.end();
    }
    static void printVisitor(const ListNode& node) {
        std::cout << "{" << node.first << ":" << node.second << "}" << std::endl;
    }

    void dumpDebug( std::ostream& os) const {
        ScopedLock scoped(m_lock);
        std::cout << "Cache Size : " << m_cache.size() << " (max:" << m_maxSize
                << ") (elasticity: " << m_elasticity << ")" << std::endl;
        for (auto& node : m_keys) {
            printVisitor(node);
        }

    }
protected:
    size_t prune() {
        if (m_maxSize > 0 && m_cache.size() >= (m_maxSize + m_elasticity)) {
            size_t count = 0;
            while (m_cache.size() > m_maxSize) {
                m_cache.erase(m_keys.back().first);
                m_keys.pop_back();
                count++;
            }
            return count;
        } else {
            return 0;
        }
    }
private:
    Cache(const Cache&);
    const Cache& operator =(const Cache&);
};

}

lru::Cache<addr_t, uint64_t, lru::MutexLock> lru_cache(LRU_CACHE_MAX_SIZE, 1000);
//const std::string reuse_distance_t::TOOL_NAME = "Reuse distance tool";

unsigned int ::reuse_distance_t::knob_verbose;

analysis_tool_t *
reuse_distance_tool_create(const reuse_distance_knobs_t &knobs)
{
    return new reuse_distance_t(knobs);
}

reuse_distance_t::reuse_distance_t(const reuse_distance_knobs_t &knobs)
    : knobs_(knobs)
    , line_size_bits_(compute_log2((int)knobs_.line_size))
{
    if (DEBUG_VERBOSE(2)) {
        std::cerr << "cache line size " << knobs_.line_size << ", "
                  << "reuse distance threshold " << knobs_.distance_threshold
                  << std::endl;
    }
}

reuse_distance_t::~reuse_distance_t()
{
    for (auto &shard : shard_map_) {
        delete shard.second;
    }
}

reuse_distance_t::shard_data_t::shard_data_t(uint64_t reuse_threshold, uint64_t skip_dist,
                                             bool verify)
{
    ref_list = std::unique_ptr<line_ref_list_t>(
        new line_ref_list_t(reuse_threshold, skip_dist, verify));
}

bool
reuse_distance_t::parallel_shard_supported()
{
    return true;
}

void *
reuse_distance_t::parallel_shard_init(int shard_index, void *worker_data)
{
    auto shard = new shard_data_t(knobs_.distance_threshold, knobs_.skip_list_distance,
                                  knobs_.verify_skip);
    std::lock_guard<std::mutex> guard(shard_map_mutex_);
    shard_map_[shard_index] = shard;
    return reinterpret_cast<void *>(shard);

    lru_cache.clear();
    cur_inst_pc = 0;
    lru_cache_inited = true;
    memset(inst_dist_count, 0, sizeof(inst_dist_count));
    memset(reuse_dist_sum, 0, sizeof(reuse_dist_sum));
    loc_id = 0;
    std::cout << "[INFO: lru cache initialized" << "]\n";
}

bool
reuse_distance_t::parallel_shard_exit(void *shard_data)
{
    // Nothing (we read the shard data in print_results).
    return true;
}

std::string
reuse_distance_t::parallel_shard_error(void *shard_data)
{
    shard_data_t *shard = reinterpret_cast<shard_data_t *>(shard_data);
    return shard->error;
}

bool
reuse_distance_t::parallel_shard_memref(void *shard_data, const memref_t &memref)
{
    shard_data_t *shard = reinterpret_cast<shard_data_t *>(shard_data);
    if (DEBUG_VERBOSE(3)) {
        std::cerr << " ::" << memref.data.pid << "." << memref.data.tid
                  << ":: " << trace_type_names[memref.data.type];
        if (memref.data.type != TRACE_TYPE_THREAD_EXIT) {
            std::cerr << " @ ";
            if (!type_is_instr(memref.data.type))
                std::cerr << (void *)memref.data.pc << " ";
            std::cerr << (void *)memref.data.addr << " x" << memref.data.size;
        }
        std::cerr << std::endl;
    }
    if (memref.data.type == TRACE_TYPE_THREAD_EXIT) {
        shard->tid = memref.exit.tid;
        return true;
    }
    if (type_is_instr(memref.instr.type) 
        ) {
        ++shard->total_refs;
        //addr_t tag = memref.data.addr >> line_size_bits_;
        addr_t tag = memref.data.addr;
        std::unordered_map<addr_t, line_ref_t *>::iterator it =
            shard->cache_map.find(tag);
        if (it == shard->cache_map.end()) {
            line_ref_t *ref = new line_ref_t(tag);
            // insert into the map
            shard->cache_map.insert(std::pair<addr_t, line_ref_t *>(tag, ref));
            // insert into the list
            shard->ref_list->add_to_front(ref);
        } else {
            int_least64_t dist = shard->ref_list->move_to_front(it->second);
            std::unordered_map<int_least64_t, int_least64_t>::iterator dist_it =
                shard->dist_map.find(dist);
            if (dist_it == shard->dist_map.end())
                shard->dist_map.insert(std::pair<int_least64_t, int_least64_t>(dist, 1));
            else
                ++dist_it->second;
            if (DEBUG_VERBOSE(3)) {
                std::cerr << "Distance is " << dist << "\n";
            }
        }


        ++cur_inst_pc;
        if (lru_cache.contains(tag)) {
            uint64_t prev_inst_pc = lru_cache.get(tag, false);
            uint64_t reuse_dist = cur_inst_pc - prev_inst_pc;
            if ((reuse_dist_sum[loc_id] + reuse_dist) > INT64_MAX) {
                loc_id += 1;
                if (reuse_dist_sum[loc_id] != 0) {
                    reuse_dist_sum[loc_id] = 0;
                }
            }
            reuse_dist_sum[loc_id] += reuse_dist;
            reuse_dist_ssum += (__uint128_t)(reuse_dist * reuse_dist);
            int log2_reuse_dist = std::min(int(log2(reuse_dist)), INST_DIST_STEP);
            ++inst_dist_count[log2_reuse_dist];
            ++reuse_inst_num;
        } else {
            ++inst_dist_count[INST_DIST_STEP + 1]; // 最后一个用来记录第一次插入或者只执行一次的
        }
        lru_cache.insert(tag, cur_inst_pc);
    }
    return true;
}

bool
reuse_distance_t::process_memref(const memref_t &memref)
{
    // For serial operation we index using the tid.
    shard_data_t *shard;
    const auto &lookup = shard_map_.find(memref.data.tid);
    if (lookup == shard_map_.end()) {
        shard = new shard_data_t(knobs_.distance_threshold, knobs_.skip_list_distance,
                                 knobs_.verify_skip);
        shard_map_[memref.data.tid] = shard;
    } else
        shard = lookup->second;
    if (!parallel_shard_memref(reinterpret_cast<void *>(shard), memref)) {
        error_string_ = shard->error;
        return false;
    }
    return true;
}

static bool
cmp_dist_key(const std::pair<int_least64_t, int_least64_t> &l,
             const std::pair<int_least64_t, int_least64_t> &r)
{
    return l.first < r.first;
}

static bool
cmp_total_refs(const std::pair<addr_t, line_ref_t *> &l,
               const std::pair<addr_t, line_ref_t *> &r)
{
    if (l.second->total_refs > r.second->total_refs)
        return true;
    if (l.second->total_refs < r.second->total_refs)
        return false;
    if (l.second->distant_refs > r.second->distant_refs)
        return true;
    if (l.second->distant_refs < r.second->distant_refs)
        return false;
    return l.first < r.first;
}

static bool
cmp_distant_refs(const std::pair<addr_t, line_ref_t *> &l,
                 const std::pair<addr_t, line_ref_t *> &r)
{
    if (l.second->distant_refs > r.second->distant_refs)
        return true;
    if (l.second->distant_refs < r.second->distant_refs)
        return false;
    if (l.second->total_refs > r.second->total_refs)
        return true;
    if (l.second->total_refs < r.second->total_refs)
        return false;
    return l.first < r.first;
}

void
reuse_distance_t::print_shard_results(const shard_data_t *shard)
{
    std::cerr << "Total accesses: " << shard->total_refs << "\n";
    std::cerr << "Unique accesses: " << shard->ref_list->cur_time_ << "\n";
    std::cerr << "Unique cache lines accessed: " << shard->cache_map.size() << "\n";
    std::cerr << "\n";

    std::cerr.precision(2);
    std::cerr.setf(std::ios::fixed);

    double sum = 0.0;
    int_least64_t count = 0;
    for (const auto &it : shard->dist_map) {
        sum += (it.first + 1) * it.second;
        count += it.second;
    }
    double mean = sum / count;
    std::cerr << "Reuse distance sum: " << sum << "\n";
    std::cerr << "Reuse distance mean: " << mean << "\n";
    std::cerr << "reuse inst count: " << count << "\n";
    double sum_of_squares = 0;
    int_least64_t recount = 0;
    bool have_median = false;
    std::vector<std::pair<int_least64_t, int_least64_t>> sorted(shard->dist_map.size());
    std::partial_sort_copy(shard->dist_map.begin(), shard->dist_map.end(), sorted.begin(),
                           sorted.end(), cmp_dist_key);
    for (auto it = sorted.begin(); it != sorted.end(); ++it) {
        double diff = it->first - mean;
        sum_of_squares += (diff * diff) * it->second;
        if (!have_median) {
            recount += it->second;
            if (recount >= count / 2) {
                std::cerr << "Reuse distance median: " << it->first << "\n";
                have_median = true;
            }
        }
    }
    double stddev = std::sqrt(sum_of_squares / count);
    std::cerr << "Reuse distance standard deviation: " << stddev << "\n";

    printf("====> Instruction Reuse Distance <====\n");
    uint64_t le = 1;
    uint64_t ri = 2;
    for (int i = 0; i < INST_DIST_STEP; ++i) {
        printf("[%8lu, %8lu): %lu\n", le, ri, inst_dist_count[i]);
        le *= 2;
        ri *= 2;
    }
    printf("[%8lu, %8s): %lu\n", le, "inf", inst_dist_count[INST_DIST_STEP]);
    printf("[%8s]: %lu\n", "the total number of instruction key",
            inst_dist_count[INST_DIST_STEP + 1]);
    printf( "[%8s]: %lu\n", "the total number of reuse data num", reuse_inst_num);
    printf("[%8s]: %lu\n", "the total number of instruction counter", cur_inst_pc);
    double resue_mean = 0;
    for (int i = 0; i <= loc_id; i++) {
        resue_mean += (double)((double)reuse_dist_sum[i] / (double)reuse_inst_num);
    }
    double reuse_stdev = 0;
    reuse_stdev = static_cast<double>((uint64_t)(reuse_dist_ssum >> 64) * (double)(INT64_MAX / reuse_inst_num));
    reuse_stdev *= 2;
    reuse_stdev += static_cast<double>((uint64_t)(reuse_dist_ssum) / reuse_inst_num);
    reuse_stdev -= (resue_mean * resue_mean);
    reuse_stdev = std::sqrt(reuse_stdev);
    printf("%8s: %f\n", "the stdev of reuse dist is", reuse_stdev);
    printf("%8s: %f\n", "the mean of reuse dist is", resue_mean);

    if (knobs_.report_histogram) {
        std::cerr << "Reuse distance histogram:\n";
        std::cerr << "Distance" << std::setw(12) << "Count"
                  << "  Percent  Cumulative\n";
        double cum_percent = 0;
        for (auto it = sorted.begin(); it != sorted.end(); ++it) {
            double percent = it->second / static_cast<double>(count);
            cum_percent += percent;
            std::cerr << std::setw(8) << it->first + 1<< std::setw(12) << it->second
                      << std::setw(8) << percent * 100. << "%" << std::setw(8)
                      << cum_percent * 100. << "%\n";
        }
    } else {
        std::cerr << "(Pass -reuse_distance_histogram to see all the data.)\n";
    }

    std::cerr << "\n";
    std::cerr << "Reuse distance threshold = " << knobs_.distance_threshold
              << " cache lines\n";
    std::vector<std::pair<addr_t, line_ref_t *>> top(knobs_.report_top);
    std::partial_sort_copy(shard->cache_map.begin(), shard->cache_map.end(), top.begin(),
                           top.end(), cmp_total_refs);
    std::cerr << "Top " << top.size() << " frequently referenced cache lines\n";
    std::cerr << std::setw(18) << "cache line"
              << ": " << std::setw(17) << "#references  " << std::setw(14)
              << "#distant refs"
              << "\n";
    for (std::vector<std::pair<addr_t, line_ref_t *>>::iterator it = top.begin();
         it != top.end(); ++it) {
        if (it->second == NULL) // Very small app.
            break;
        std::cerr << std::setw(18) << std::hex << std::showbase
                  << (it->first << line_size_bits_) << ": " << std::setw(12) << std::dec
                  << it->second->total_refs << ", " << std::setw(12) << std::dec
                  << it->second->distant_refs << "\n";
    }
    top.clear();
    top.resize(knobs_.report_top);
    std::partial_sort_copy(shard->cache_map.begin(), shard->cache_map.end(), top.begin(),
                           top.end(), cmp_distant_refs);
    std::cerr << "Top " << top.size() << " distant repeatedly referenced cache lines\n";
    std::cerr << std::setw(18) << "cache line"
              << ": " << std::setw(17) << "#references  " << std::setw(14)
              << "#distant refs"
              << "\n";
    for (std::vector<std::pair<addr_t, line_ref_t *>>::iterator it = top.begin();
         it != top.end(); ++it) {
        if (it->second == NULL) // Very small app.
            break;
        std::cerr << std::setw(18) << std::hex << std::showbase
                  << (it->first << line_size_bits_) << ": " << std::setw(12) << std::dec
                  << it->second->total_refs << ", " << std::setw(12) << std::dec
                  << it->second->distant_refs << "\n";
    }
}

bool
reuse_distance_t::print_results()
{
    // First, aggregate the per-shard data into whole-trace data.
    auto aggregate = std::unique_ptr<shard_data_t>(new shard_data_t(
        knobs_.distance_threshold, knobs_.skip_list_distance, knobs_.verify_skip));
    for (const auto &shard : shard_map_) {
        aggregate->total_refs += shard.second->total_refs;
        // We simply sum the unique accesses.
        // If the user wants the unique accesses over the merged trace they
        // can create a single shard and invoke the parallel operations.
        aggregate->ref_list->cur_time_ += shard.second->ref_list->cur_time_;
        // We merge the histogram and the cache_map.
        for (const auto &entry : shard.second->dist_map) {
            aggregate->dist_map[entry.first] += entry.second;
        }
        for (const auto &entry : shard.second->cache_map) {
            const auto &existing = aggregate->cache_map.find(entry.first);
            line_ref_t *ref;
            if (existing == aggregate->cache_map.end()) {
                ref = new line_ref_t(entry.first);
                aggregate->cache_map.insert(
                    std::pair<addr_t, line_ref_t *>(entry.first, ref));
                ref->total_refs = 0;
            } else {
                ref = existing->second;
            }
            ref->total_refs += entry.second->total_refs;
            ref->distant_refs += entry.second->distant_refs;
        }
    }

    //std::cerr << TOOL_NAME << " aggregated results:\n";
    std::cerr << "Reuse distance tool aggregated results:\n";
    print_shard_results(aggregate.get());

    // For regular shards the line_ref_t's are deleted in ~line_ref_list_t.
    for (auto &iter : aggregate->cache_map) {
        delete iter.second;
    }

    if (shard_map_.size() > 1) {
        using keyval_t = std::pair<memref_tid_t, shard_data_t *>;
        std::vector<keyval_t> sorted(shard_map_.begin(), shard_map_.end());
        std::sort(sorted.begin(), sorted.end(), [](const keyval_t &l, const keyval_t &r) {
            return l.second->total_refs > r.second->total_refs;
        });
        for (const auto &shard : sorted) {
            std::cerr << "\n==================================================\n"
                      << "Reuse distance tool results for shard " << shard.first << " (thread "
                      << shard.second->tid << "):\n";
            print_shard_results(shard.second);
        }
    }

    // Reset the i/o format for subsequent tool invocations.
    std::cerr << std::dec;
    return true;
}
