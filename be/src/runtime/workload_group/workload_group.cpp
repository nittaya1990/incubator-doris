// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "workload_group.h"

#include <fmt/format.h>
#include <gen_cpp/PaloInternalService_types.h>

#include <charconv>
#include <map>
#include <mutex>
#include <ostream>
#include <utility>

#include "common/logging.h"
#include "exec/schema_scanner/schema_scanner_helper.h"
#include "io/fs/local_file_reader.h"
#include "olap/storage_engine.h"
#include "pipeline/task_queue.h"
#include "pipeline/task_scheduler.h"
#include "runtime/exec_env.h"
#include "runtime/memory/global_memory_arbitrator.h"
#include "runtime/memory/mem_tracker_limiter.h"
#include "runtime/workload_group/workload_group_metrics.h"
#include "runtime/workload_management/io_throttle.h"
#include "util/mem_info.h"
#include "util/parse_util.h"
#include "util/runtime_profile.h"
#include "util/threadpool.h"
#include "vec/exec/scan/scanner_scheduler.h"

namespace doris {

const static std::string MEMORY_LIMIT_DEFAULT_VALUE = "0%";
const static bool ENABLE_MEMORY_OVERCOMMIT_DEFAULT_VALUE = true;
const static int CPU_HARD_LIMIT_DEFAULT_VALUE = -1;
const static int MEMORY_LOW_WATERMARK_DEFAULT_VALUE = 50;
const static int MEMORY_HIGH_WATERMARK_DEFAULT_VALUE = 80;

WorkloadGroup::WorkloadGroup(const WorkloadGroupInfo& wg_info) : WorkloadGroup(wg_info, true) {}

WorkloadGroup::WorkloadGroup(const WorkloadGroupInfo& tg_info, bool need_create_query_thread_pool)
        : _id(tg_info.id),
          _name(tg_info.name),
          _version(tg_info.version),
          _memory_limit(tg_info.memory_limit),
          _enable_memory_overcommit(tg_info.enable_memory_overcommit),
          _cpu_share(tg_info.cpu_share),
          _mem_tracker_limiter_pool(MEM_TRACKER_GROUP_NUM),
          _cpu_hard_limit(tg_info.cpu_hard_limit),
          _scan_thread_num(tg_info.scan_thread_num),
          _max_remote_scan_thread_num(tg_info.max_remote_scan_thread_num),
          _min_remote_scan_thread_num(tg_info.min_remote_scan_thread_num),
          _memory_low_watermark(tg_info.memory_low_watermark),
          _memory_high_watermark(tg_info.memory_high_watermark),
          _scan_bytes_per_second(tg_info.read_bytes_per_second),
          _remote_scan_bytes_per_second(tg_info.remote_read_bytes_per_second),
          _need_create_query_thread_pool(need_create_query_thread_pool) {
    std::vector<DataDirInfo>& data_dir_list = io::BeConfDataDirReader::be_config_data_dir_list;
    for (const auto& data_dir : data_dir_list) {
        _scan_io_throttle_map[data_dir.path] = std::make_shared<IOThrottle>(data_dir.metric_name);
    }
    _remote_scan_io_throttle = std::make_shared<IOThrottle>();

    _wg_metrics = std::make_shared<WorkloadGroupMetrics>(this);
}

std::string WorkloadGroup::debug_string() const {
    std::shared_lock<std::shared_mutex> rl {_mutex};
    return fmt::format(
            "TG[id = {}, name = {}, cpu_share = {}, memory_limit = {}, enable_memory_overcommit = "
            "{}, version = {}, cpu_hard_limit = {}, scan_thread_num = "
            "{}, max_remote_scan_thread_num = {}, min_remote_scan_thread_num = {}, "
            "memory_low_watermark={}, memory_high_watermark={}, is_shutdown={}, query_num={}, "
            "read_bytes_per_second={}, remote_read_bytes_per_second={}]",
            _id, _name, cpu_share(), PrettyPrinter::print(_memory_limit, TUnit::BYTES),
            _enable_memory_overcommit ? "true" : "false", _version, cpu_hard_limit(),
            _scan_thread_num, _max_remote_scan_thread_num, _min_remote_scan_thread_num,
            _memory_low_watermark, _memory_high_watermark, _is_shutdown, _query_ctxs.size(),
            _scan_bytes_per_second, _remote_scan_bytes_per_second);
}

std::string WorkloadGroup::memory_debug_string() const {
    return fmt::format(
            "TG[id = {}, name = {}, memory_limit = {}, enable_memory_overcommit = "
            "{}, weighted_memory_limit = {}, total_mem_used = {}, "
            "wg_refresh_interval_memory_growth = {}, memory_low_watermark = {}, "
            "memory_high_watermark = {}, version = {}, is_shutdown = {}, query_num = {}]",
            _id, _name, PrettyPrinter::print(_memory_limit, TUnit::BYTES),
            _enable_memory_overcommit ? "true" : "false",
            PrettyPrinter::print(_weighted_memory_limit, TUnit::BYTES),
            PrettyPrinter::print(_total_mem_used, TUnit::BYTES),
            PrettyPrinter::print(_wg_refresh_interval_memory_growth, TUnit::BYTES),
            _memory_low_watermark, _memory_high_watermark, _version, _is_shutdown,
            _query_ctxs.size());
}

void WorkloadGroup::check_and_update(const WorkloadGroupInfo& tg_info) {
    if (UNLIKELY(tg_info.id != _id)) {
        return;
    }
    {
        std::shared_lock<std::shared_mutex> rl {_mutex};
        if (LIKELY(tg_info.version <= _version)) {
            return;
        }
    }
    {
        std::lock_guard<std::shared_mutex> wl {_mutex};
        if (tg_info.version > _version) {
            _name = tg_info.name;
            _version = tg_info.version;
            _memory_limit = tg_info.memory_limit;
            _enable_memory_overcommit = tg_info.enable_memory_overcommit;
            _cpu_share = tg_info.cpu_share;
            _cpu_hard_limit = tg_info.cpu_hard_limit;
            _scan_thread_num = tg_info.scan_thread_num;
            _max_remote_scan_thread_num = tg_info.max_remote_scan_thread_num;
            _min_remote_scan_thread_num = tg_info.min_remote_scan_thread_num;
            _memory_low_watermark = tg_info.memory_low_watermark;
            _memory_high_watermark = tg_info.memory_high_watermark;
            _scan_bytes_per_second = tg_info.read_bytes_per_second;
            _remote_scan_bytes_per_second = tg_info.remote_read_bytes_per_second;
        } else {
            return;
        }
    }
}

// MemtrackerLimiter is not removed during query context release, so that should remove it here.
int64_t WorkloadGroup::make_memory_tracker_snapshots(
        std::list<std::shared_ptr<MemTrackerLimiter>>* tracker_snapshots) {
    int64_t used_memory = 0;
    for (auto& mem_tracker_group : _mem_tracker_limiter_pool) {
        std::lock_guard<std::mutex> l(mem_tracker_group.group_lock);
        for (auto trackerWptr = mem_tracker_group.trackers.begin();
             trackerWptr != mem_tracker_group.trackers.end();) {
            auto tracker = trackerWptr->lock();
            if (tracker == nullptr) {
                trackerWptr = mem_tracker_group.trackers.erase(trackerWptr);
            } else {
                if (tracker_snapshots != nullptr) {
                    tracker_snapshots->insert(tracker_snapshots->end(), tracker);
                }
                used_memory += tracker->consumption();
                ++trackerWptr;
            }
        }
    }
    // refresh total memory used.
    _total_mem_used = used_memory;
    _wg_metrics->update_memory_used_bytes(used_memory);
    // reserve memory is recorded in the query mem tracker
    // and _total_mem_used already contains all the current reserve memory.
    // so after refreshing _total_mem_used, reset _wg_refresh_interval_memory_growth.
    _wg_refresh_interval_memory_growth.store(0.0);
    return used_memory;
}

int64_t WorkloadGroup::memory_used() {
    return make_memory_tracker_snapshots(nullptr);
}

void WorkloadGroup::do_sweep() {
    // Clear memtracker limiter that is registered during query or load.
    for (auto& mem_tracker_group : _mem_tracker_limiter_pool) {
        std::lock_guard<std::mutex> l(mem_tracker_group.group_lock);
        for (auto trackerWptr = mem_tracker_group.trackers.begin();
             trackerWptr != mem_tracker_group.trackers.end();) {
            auto tracker = trackerWptr->lock();
            if (tracker == nullptr) {
                trackerWptr = mem_tracker_group.trackers.erase(trackerWptr);
            } else {
                ++trackerWptr;
            }
        }
    }

    // Clear query context that is registered during query context ctor
    std::unique_lock<std::shared_mutex> wlock(_mutex);
    for (auto iter = _query_ctxs.begin(); iter != _query_ctxs.end();) {
        if (iter->second.lock() == nullptr) {
            iter = _query_ctxs.erase(iter);
        } else {
            iter++;
        }
    }
}

void WorkloadGroup::add_mem_tracker_limiter(std::shared_ptr<MemTrackerLimiter> mem_tracker_ptr) {
    std::unique_lock<std::shared_mutex> wlock(_mutex);
    auto group_num = mem_tracker_ptr->group_num();
    std::lock_guard<std::mutex> l(_mem_tracker_limiter_pool[group_num].group_lock);
    _mem_tracker_limiter_pool[group_num].trackers.insert(
            _mem_tracker_limiter_pool[group_num].trackers.end(), mem_tracker_ptr);
}

int64_t WorkloadGroup::gc_memory(int64_t need_free_mem, RuntimeProfile* profile, bool is_minor_gc) {
    if (need_free_mem <= 0) {
        return 0;
    }
    int64_t used_memory = memory_used();
    int64_t freed_mem = 0;

    std::string cancel_str = "";
    if (is_minor_gc) {
        cancel_str = fmt::format(
                "MinorGC kill overcommit query, wg id:{}, name:{}, used:{}, limit:{}, "
                "backend:{}.",
                _id, _name, MemCounter::print_bytes(used_memory),
                MemCounter::print_bytes(_memory_limit), BackendOptions::get_localhost());
    } else {
        if (_enable_memory_overcommit) {
            cancel_str = fmt::format(
                    "FullGC release wg overcommit mem, wg id:{}, name:{}, "
                    "used:{},limit:{},backend:{}.",
                    _id, _name, MemCounter::print_bytes(used_memory),
                    MemCounter::print_bytes(_memory_limit), BackendOptions::get_localhost());
        } else {
            cancel_str = fmt::format(
                    "GC wg for hard limit, wg id:{}, name:{}, used:{}, limit:{}, "
                    "backend:{}.",
                    _id, _name, MemCounter::print_bytes(used_memory),
                    MemCounter::print_bytes(_memory_limit), BackendOptions::get_localhost());
        }
    }
    auto cancel_top_overcommit_str = [cancel_str](int64_t mem_consumption,
                                                  const std::string& label) {
        return fmt::format(
                "{} cancel top memory overcommit tracker <{}> consumption {}. details:{}, "
                "Execute "
                "again after enough memory, details see be.INFO.",
                cancel_str, label, MemCounter::print_bytes(mem_consumption),
                GlobalMemoryArbitrator::process_limit_exceeded_errmsg_str());
    };
    auto cancel_top_usage_str = [cancel_str](int64_t mem_consumption, const std::string& label) {
        return fmt::format(
                "{} cancel top memory used tracker <{}> consumption {}. details:{}, Execute "
                "again "
                "after enough memory, details see be.INFO.",
                cancel_str, label, MemCounter::print_bytes(mem_consumption),
                GlobalMemoryArbitrator::process_soft_limit_exceeded_errmsg_str());
    };

    LOG(INFO) << fmt::format(
            "[MemoryGC] work load group start gc, id:{} name:{}, memory limit: {}, used: {}, "
            "need_free_mem: {}.",
            _id, _name, _memory_limit, used_memory, need_free_mem);
    Defer defer {[&]() {
        LOG(INFO) << fmt::format(
                "[MemoryGC] work load group finished gc, id:{} name:{}, memory limit: {}, "
                "used: "
                "{}, need_free_mem: {}, freed memory: {}.",
                _id, _name, _memory_limit, used_memory, need_free_mem, freed_mem);
    }};

    // 1. free top overcommit query
    if (config::enable_query_memory_overcommit) {
        RuntimeProfile* tmq_profile = profile->create_child(
                fmt::format("FreeGroupTopOvercommitQuery:Name {}", _name), true, true);
        freed_mem += MemTrackerLimiter::free_top_overcommit_query(
                need_free_mem - freed_mem, MemTrackerLimiter::Type::QUERY,
                _mem_tracker_limiter_pool, cancel_top_overcommit_str, tmq_profile,
                MemTrackerLimiter::GCType::WORK_LOAD_GROUP);
    }
    // To be compatible with the non-group's gc logic, minorGC just gc overcommit query
    if (is_minor_gc || freed_mem >= need_free_mem) {
        return freed_mem;
    }

    // 2. free top usage query
    RuntimeProfile* tmq_profile =
            profile->create_child(fmt::format("FreeGroupTopUsageQuery:Name {}", _name), true, true);
    freed_mem += MemTrackerLimiter::free_top_memory_query(
            need_free_mem - freed_mem, MemTrackerLimiter::Type::QUERY, _mem_tracker_limiter_pool,
            cancel_top_usage_str, tmq_profile, MemTrackerLimiter::GCType::WORK_LOAD_GROUP);
    if (freed_mem >= need_free_mem) {
        return freed_mem;
    }

    // 3. free top overcommit load
    if (config::enable_query_memory_overcommit) {
        tmq_profile = profile->create_child(
                fmt::format("FreeGroupTopOvercommitLoad:Name {}", _name), true, true);
        freed_mem += MemTrackerLimiter::free_top_overcommit_query(
                need_free_mem - freed_mem, MemTrackerLimiter::Type::LOAD, _mem_tracker_limiter_pool,
                cancel_top_overcommit_str, tmq_profile, MemTrackerLimiter::GCType::WORK_LOAD_GROUP);
        if (freed_mem >= need_free_mem) {
            return freed_mem;
        }
    }

    // 4. free top usage load
    tmq_profile =
            profile->create_child(fmt::format("FreeGroupTopUsageLoad:Name {}", _name), true, true);
    freed_mem += MemTrackerLimiter::free_top_memory_query(
            need_free_mem - freed_mem, MemTrackerLimiter::Type::LOAD, _mem_tracker_limiter_pool,
            cancel_top_usage_str, tmq_profile, MemTrackerLimiter::GCType::WORK_LOAD_GROUP);
    return freed_mem;
}

WorkloadGroupInfo WorkloadGroupInfo::parse_topic_info(
        const TWorkloadGroupInfo& tworkload_group_info) {
    // 1 id
    uint64_t tg_id = 0;
    if (tworkload_group_info.__isset.id) {
        tg_id = tworkload_group_info.id;
    } else {
        return {.name = "", .valid = false};
    }

    // 2 name
    std::string name = "INVALID_NAME";
    if (tworkload_group_info.__isset.name) {
        name = tworkload_group_info.name;
    }

    // 3 version
    int version = 0;
    if (tworkload_group_info.__isset.version) {
        version = tworkload_group_info.version;
    } else {
        return {.name {}, .valid = false};
    }

    // 4 cpu_share
    uint64_t cpu_share = CgroupCpuCtl::cpu_soft_limit_default_value();
    if (tworkload_group_info.__isset.cpu_share && tworkload_group_info.cpu_share > 0) {
        cpu_share = tworkload_group_info.cpu_share;
    }

    // 5 cpu hard limit
    int cpu_hard_limit = CPU_HARD_LIMIT_DEFAULT_VALUE;
    if (tworkload_group_info.__isset.cpu_hard_limit && tworkload_group_info.cpu_hard_limit > 0) {
        cpu_hard_limit = tworkload_group_info.cpu_hard_limit;
    }

    // 6 mem_limit
    std::string mem_limit_str = MEMORY_LIMIT_DEFAULT_VALUE;
    if (tworkload_group_info.__isset.mem_limit && tworkload_group_info.mem_limit != "-1") {
        mem_limit_str = tworkload_group_info.mem_limit;
    }
    bool is_percent = true;
    int64_t mem_limit =
            ParseUtil::parse_mem_spec(mem_limit_str, -1, MemInfo::mem_limit(), &is_percent);

    // 7 mem overcommit
    bool enable_memory_overcommit = ENABLE_MEMORY_OVERCOMMIT_DEFAULT_VALUE;
    if (tworkload_group_info.__isset.enable_memory_overcommit) {
        enable_memory_overcommit = tworkload_group_info.enable_memory_overcommit;
    }

    // 8 cpu soft limit or hard limit
    bool enable_cpu_hard_limit = false;
    if (tworkload_group_info.__isset.enable_cpu_hard_limit) {
        enable_cpu_hard_limit = tworkload_group_info.enable_cpu_hard_limit;
    }

    // 9 scan thread num
    int scan_thread_num = config::doris_scanner_thread_pool_thread_num;
    if (tworkload_group_info.__isset.scan_thread_num && tworkload_group_info.scan_thread_num > 0) {
        scan_thread_num = tworkload_group_info.scan_thread_num;
    }

    // 10 max remote scan thread num
    int max_remote_scan_thread_num = vectorized::ScannerScheduler::get_remote_scan_thread_num();
    if (tworkload_group_info.__isset.max_remote_scan_thread_num &&
        tworkload_group_info.max_remote_scan_thread_num > 0) {
        max_remote_scan_thread_num = tworkload_group_info.max_remote_scan_thread_num;
    }

    // 11 min remote scan thread num
    int min_remote_scan_thread_num = config::doris_scanner_min_thread_pool_thread_num;
    if (tworkload_group_info.__isset.min_remote_scan_thread_num &&
        tworkload_group_info.min_remote_scan_thread_num > 0) {
        min_remote_scan_thread_num = tworkload_group_info.min_remote_scan_thread_num;
    }

    // 12 memory low watermark
    int memory_low_watermark = MEMORY_LOW_WATERMARK_DEFAULT_VALUE;
    if (tworkload_group_info.__isset.memory_low_watermark) {
        memory_low_watermark = tworkload_group_info.memory_low_watermark;
    }

    // 13 memory high watermark
    int memory_high_watermark = MEMORY_HIGH_WATERMARK_DEFAULT_VALUE;
    if (tworkload_group_info.__isset.memory_high_watermark) {
        memory_high_watermark = tworkload_group_info.memory_high_watermark;
    }

    // 14 scan io
    int read_bytes_per_second = -1;
    if (tworkload_group_info.__isset.read_bytes_per_second &&
        tworkload_group_info.read_bytes_per_second > 0) {
        read_bytes_per_second = tworkload_group_info.read_bytes_per_second;
    }

    // 15 remote scan io
    int remote_read_bytes_per_second = -1;
    if (tworkload_group_info.__isset.remote_read_bytes_per_second &&
        tworkload_group_info.remote_read_bytes_per_second > 0) {
        remote_read_bytes_per_second = tworkload_group_info.remote_read_bytes_per_second;
    }

    return {.id = tg_id,
            .name = name,
            .cpu_share = cpu_share,
            .memory_limit = mem_limit,
            .enable_memory_overcommit = enable_memory_overcommit,
            .version = version,
            .cpu_hard_limit = cpu_hard_limit,
            .enable_cpu_hard_limit = enable_cpu_hard_limit,
            .scan_thread_num = scan_thread_num,
            .max_remote_scan_thread_num = max_remote_scan_thread_num,
            .min_remote_scan_thread_num = min_remote_scan_thread_num,
            .memory_low_watermark = memory_low_watermark,
            .memory_high_watermark = memory_high_watermark,
            .read_bytes_per_second = read_bytes_per_second,
            .remote_read_bytes_per_second = remote_read_bytes_per_second};
}

std::weak_ptr<CgroupCpuCtl> WorkloadGroup::get_cgroup_cpu_ctl_wptr() {
    std::shared_lock<std::shared_mutex> rlock(_task_sched_lock);
    return _cgroup_cpu_ctl;
}

void WorkloadGroup::create_cgroup_cpu_ctl() {
    std::lock_guard<std::shared_mutex> wlock(_task_sched_lock);
    create_cgroup_cpu_ctl_no_lock();
}

void WorkloadGroup::create_cgroup_cpu_ctl_no_lock() {
    if (config::doris_cgroup_cpu_path != "" && _cgroup_cpu_ctl == nullptr) {
        std::shared_ptr<CgroupCpuCtl> cgroup_cpu_ctl = CgroupCpuCtl::create_cgroup_cpu_ctl(_id);
        if (cgroup_cpu_ctl) {
            Status ret = cgroup_cpu_ctl->init();
            if (ret.ok()) {
                _cgroup_cpu_ctl = std::move(cgroup_cpu_ctl);
                LOG(INFO) << "[upsert wg thread pool] cgroup init success, wg_id=" << _id;
            } else {
                LOG(INFO) << "[upsert wg thread pool] cgroup init failed, wg_id=" << _id
                          << ", reason=" << ret.to_string();
            }
        } else {
            LOG(INFO) << "[upsert wg thread pool] create cgroup cpu ctl wg_id=" << _id << " failed";
        }
    }
}

void WorkloadGroup::upsert_thread_pool_no_lock(WorkloadGroupInfo* wg_info,
                                               std::shared_ptr<CgroupCpuCtl> cg_cpu_ctl_ptr) {
    uint64_t wg_id = wg_info->id;
    std::string wg_name = wg_info->name;
    int scan_thread_num = wg_info->scan_thread_num;
    int max_remote_scan_thread_num = wg_info->max_remote_scan_thread_num;
    int min_remote_scan_thread_num = wg_info->min_remote_scan_thread_num;
    if (_task_sched == nullptr) {
        int32_t executors_size = config::pipeline_executor_size;
        if (executors_size <= 0) {
            executors_size = CpuInfo::num_cores();
        }
        std::unique_ptr<pipeline::TaskScheduler> pipeline_task_scheduler =
                std::make_unique<pipeline::TaskScheduler>(executors_size, "Pipe_" + wg_name,
                                                          cg_cpu_ctl_ptr);
        Status ret = pipeline_task_scheduler->start();
        if (ret.ok()) {
            _task_sched = std::move(pipeline_task_scheduler);
        } else {
            LOG(INFO) << "[upsert wg thread pool] task scheduler start failed, gid= " << wg_id;
        }
    }

    if (_scan_task_sched == nullptr) {
        std::unique_ptr<vectorized::SimplifiedScanScheduler> scan_scheduler =
                std::make_unique<vectorized::SimplifiedScanScheduler>("Scan_" + wg_name,
                                                                      cg_cpu_ctl_ptr, wg_name);
        Status ret = scan_scheduler->start(config::doris_scanner_thread_pool_thread_num,
                                           config::doris_scanner_thread_pool_thread_num,
                                           config::doris_scanner_thread_pool_queue_size);
        if (ret.ok()) {
            _scan_task_sched = std::move(scan_scheduler);
        } else {
            LOG(INFO) << "[upsert wg thread pool] scan scheduler start failed, gid=" << wg_id;
        }
    }
    if (scan_thread_num > 0 && _scan_task_sched) {
        _scan_task_sched->reset_thread_num(scan_thread_num, scan_thread_num);
    }

    if (_remote_scan_task_sched == nullptr) {
        int remote_max_thread_num = vectorized::ScannerScheduler::get_remote_scan_thread_num();
        int remote_scan_thread_queue_size =
                vectorized::ScannerScheduler::get_remote_scan_thread_queue_size();
        std::unique_ptr<vectorized::SimplifiedScanScheduler> remote_scan_scheduler =
                std::make_unique<vectorized::SimplifiedScanScheduler>("RScan_" + wg_name,
                                                                      cg_cpu_ctl_ptr, wg_name);
        Status ret = remote_scan_scheduler->start(remote_max_thread_num,
                                                  config::doris_scanner_min_thread_pool_thread_num,
                                                  remote_scan_thread_queue_size);
        if (ret.ok()) {
            _remote_scan_task_sched = std::move(remote_scan_scheduler);
        } else {
            LOG(INFO) << "[upsert wg thread pool] remote scan scheduler start failed, gid="
                      << wg_id;
        }
    }
    if (max_remote_scan_thread_num >= min_remote_scan_thread_num && _remote_scan_task_sched) {
        _remote_scan_task_sched->reset_thread_num(max_remote_scan_thread_num,
                                                  min_remote_scan_thread_num);
    }

    if (_memtable_flush_pool == nullptr) {
        int num_disk = ExecEnv::GetInstance()->storage_engine().get_disk_num();
        // -1 means disk num may not be inited, so not create flush pool
        if (num_disk != -1) {
            std::unique_ptr<ThreadPool> thread_pool = nullptr;
            num_disk = std::max(1, num_disk);
            int num_cpus = std::thread::hardware_concurrency();

            int min_threads = std::max(1, config::wg_flush_thread_num_per_store);
            int max_threads = num_cpus == 0
                                      ? num_disk * min_threads
                                      : std::min(num_disk * min_threads,
                                                 num_cpus * config::wg_flush_thread_num_per_cpu);

            std::string pool_name = "wg_flush_" + wg_name;
            auto ret = ThreadPoolBuilder(pool_name)
                               .set_min_threads(min_threads)
                               .set_max_threads(max_threads)
                               .set_cgroup_cpu_ctl(cg_cpu_ctl_ptr)
                               .build(&thread_pool);
            if (!ret.ok()) {
                LOG(INFO) << "[upsert wg thread pool] create " + pool_name + " failed, gid="
                          << wg_id;
            } else {
                _memtable_flush_pool = std::move(thread_pool);
                LOG(INFO) << "[upsert wg thread pool] create " + pool_name + " succ, gid=" << wg_id
                          << ", max thread num=" << max_threads
                          << ", min thread num=" << min_threads;
            }
        }
    }
}

void WorkloadGroup::upsert_cgroup_cpu_ctl_no_lock(WorkloadGroupInfo* wg_info) {
    uint64_t wg_id = wg_info->id;
    int cpu_hard_limit = wg_info->cpu_hard_limit;
    uint64_t cpu_shares = wg_info->cpu_share;
    bool enable_cpu_hard_limit = wg_info->enable_cpu_hard_limit;
    create_cgroup_cpu_ctl_no_lock();

    if (_cgroup_cpu_ctl) {
        if (enable_cpu_hard_limit) {
            if (cpu_hard_limit > 0) {
                _cgroup_cpu_ctl->update_cpu_hard_limit(cpu_hard_limit);
                _cgroup_cpu_ctl->update_cpu_soft_limit(
                        CgroupCpuCtl::cpu_soft_limit_default_value());
            } else {
                LOG(INFO) << "[upsert wg thread pool] enable cpu hard limit but value is "
                             "illegal: "
                          << cpu_hard_limit << ", gid=" << wg_id;
            }
        } else {
            _cgroup_cpu_ctl->update_cpu_soft_limit(cpu_shares);
            _cgroup_cpu_ctl->update_cpu_hard_limit(
                    CPU_HARD_LIMIT_DEFAULT_VALUE); // disable cpu hard limit
        }
        _cgroup_cpu_ctl->get_cgroup_cpu_info(&(wg_info->cgroup_cpu_shares),
                                             &(wg_info->cgroup_cpu_hard_limit));
    }
}

void WorkloadGroup::upsert_task_scheduler(WorkloadGroupInfo* wg_info) {
    std::lock_guard<std::shared_mutex> wlock(_task_sched_lock);
    upsert_cgroup_cpu_ctl_no_lock(wg_info);

    if (_need_create_query_thread_pool) {
        upsert_thread_pool_no_lock(wg_info, _cgroup_cpu_ctl);
    }
}

void WorkloadGroup::get_query_scheduler(doris::pipeline::TaskScheduler** exec_sched,
                                        vectorized::SimplifiedScanScheduler** scan_sched,
                                        ThreadPool** memtable_flush_pool,
                                        vectorized::SimplifiedScanScheduler** remote_scan_sched) {
    std::shared_lock<std::shared_mutex> rlock(_task_sched_lock);
    *exec_sched = _task_sched.get();
    *scan_sched = _scan_task_sched.get();
    *remote_scan_sched = _remote_scan_task_sched.get();
    *memtable_flush_pool = _memtable_flush_pool.get();
}

std::string WorkloadGroup::thread_debug_info() {
    std::string str = "";
    if (_task_sched != nullptr) {
        std::vector<int> exec_t_info = _task_sched->thread_debug_info();
        str = fmt::format("[exec num:{}, real_num:{}, min_num:{}, max_num:{}],", exec_t_info[0],
                          exec_t_info[1], exec_t_info[2], exec_t_info[3]);
    }

    if (_scan_task_sched != nullptr) {
        std::vector<int> exec_t_info = _scan_task_sched->thread_debug_info();
        str += fmt::format("[l_scan num:{}, real_num:{}, min_num:{}, max_num{}],", exec_t_info[0],
                           exec_t_info[1], exec_t_info[2], exec_t_info[3]);
    }

    if (_remote_scan_task_sched != nullptr) {
        std::vector<int> exec_t_info = _remote_scan_task_sched->thread_debug_info();
        str += fmt::format("[r_scan num:{}, real_num:{}, min_num:{}, max_num:{}],", exec_t_info[0],
                           exec_t_info[1], exec_t_info[2], exec_t_info[3]);
    }

    if (_memtable_flush_pool != nullptr) {
        std::vector<int> exec_t_info = _memtable_flush_pool->debug_info();
        str += fmt::format("[mem_tab_flush num:{}, real_num:{}, min_num:{}, max_num:{}]",
                           exec_t_info[0], exec_t_info[1], exec_t_info[2], exec_t_info[3]);
    }
    return str;
}

void WorkloadGroup::upsert_scan_io_throttle(WorkloadGroupInfo* tg_info) {
    for (const auto& [key, io_throttle] : _scan_io_throttle_map) {
        io_throttle->set_io_bytes_per_second(tg_info->read_bytes_per_second);
    }

    _remote_scan_io_throttle->set_io_bytes_per_second(tg_info->remote_read_bytes_per_second);
}

std::shared_ptr<IOThrottle> WorkloadGroup::get_local_scan_io_throttle(const std::string& disk_dir) {
    auto find_ret = _scan_io_throttle_map.find(disk_dir);
    if (find_ret != _scan_io_throttle_map.end()) {
        return find_ret->second;
    }
    return nullptr;
}

std::shared_ptr<IOThrottle> WorkloadGroup::get_remote_scan_io_throttle() {
    return _remote_scan_io_throttle;
}

void WorkloadGroup::update_cpu_time(int64_t delta_cpu_time) {
    _wg_metrics->update_cpu_time_nanos(delta_cpu_time);
}

void WorkloadGroup::update_local_scan_io(std::string path, size_t scan_bytes) {
    _wg_metrics->update_local_scan_io_bytes(path, (uint64_t)scan_bytes);
}

void WorkloadGroup::update_remote_scan_io(size_t scan_bytes) {
    _wg_metrics->update_remote_scan_io_bytes((uint64_t)scan_bytes);
}

int64_t WorkloadGroup::get_mem_used() {
    return _total_mem_used;
}

void WorkloadGroup::try_stop_schedulers() {
    std::lock_guard<std::shared_mutex> wlock(_task_sched_lock);
    if (_task_sched) {
        _task_sched->stop();
    }
    if (_scan_task_sched) {
        _scan_task_sched->stop();
    }
    if (_remote_scan_task_sched) {
        _remote_scan_task_sched->stop();
    }
    if (_memtable_flush_pool) {
        _memtable_flush_pool->shutdown();
        _memtable_flush_pool->wait();
    }
}

} // namespace doris
