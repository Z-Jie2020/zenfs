// Copyright (c) Facebook, Inc. and its affiliates. All Rights Reserved.
// Copyright (c) 2019-present, Western Digital Corporation
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#if !defined(ROCKSDB_LITE) && !defined(OS_WIN)

#include "zbd_zenfs.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <libzbd/zbd.h>
#include <linux/blkzoned.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <math.h>

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "rocksdb/env.h"
#include "rocksdb/io_status.h"
#include "snapshot.h"
#include "zbdlib_zenfs.h"
#include "zonefs_zenfs.h"

#define KB (1024)
#define MB (1024 * KB)

/* Number of reserved zones for metadata
 * Two non-offline meta zones are needed to be able
 * to roll the metadata log safely. One extra
 * is allocated to cover for one zone going offline.
 */
#define ZENFS_META_ZONES (3)

/* Minimum of number of zones that makes sense */
#define ZENFS_MIN_ZONES (32)

namespace ROCKSDB_NAMESPACE {

Zone::Zone(ZonedBlockDevice *zbd, ZonedBlockDeviceBackend *zbd_be,
           std::unique_ptr<ZoneList> &zones, unsigned int idx)
    : zbd_(zbd),
      zbd_be_(zbd_be),
      busy_(false),
      start_(zbd_be->ZoneStart(zones, idx)),
      max_capacity_(zbd_be->ZoneMaxCapacity(zones, idx)),
      wp_(zbd_be->ZoneWp(zones, idx)) {
  lifetime_ = Env::WLTH_NOT_SET;
  used_capacity_ = 0;
  capacity_ = 0;
  reset_count_ = 0;
  if (zbd_be->ZoneIsWritable(zones, idx))
    capacity_ = max_capacity_ - (wp_ - start_);
}

bool Zone::IsUsed() { return (used_capacity_ > 0); }
uint64_t Zone::GetCapacityLeft() { return capacity_; }
bool Zone::IsFull() { return (capacity_ == 0); }
bool Zone::IsEmpty() { return (wp_ == start_); }
uint64_t Zone::GetZoneNr() { return start_ / zbd_->GetZoneSize(); }
uint32_t Zone::GetResetCount() { return reset_count_.load(); }
void Zone::SetResetCount(uint32_t reset_count) { reset_count_ = reset_count; }
uint64_t Zone::GetCapacityUsed() { return used_capacity_.load(); }
Env::WriteLifeTimeHint Zone::GetLifeTimeHint() { return lifetime_; }

void Zone::EncodeJson(std::ostream &json_stream) {
  json_stream << "{";
  json_stream << "\"start\":" << start_ << ",";
  json_stream << "\"capacity\":" << capacity_ << ",";
  json_stream << "\"max_capacity\":" << max_capacity_ << ",";
  json_stream << "\"wp\":" << wp_ << ",";
  json_stream << "\"lifetime\":" << lifetime_ << ",";
  json_stream << "\"used_capacity\":" << used_capacity_ << ",";
  json_stream << "\"reset_count\":" << reset_count_;
  json_stream << "}";
}

IOStatus Zone::Reset() {
  bool offline;
  uint64_t max_capacity;
  uint32_t io_zones_reset_count;
  uint32_t reset_count_diff;

  assert(!IsUsed());
  assert(IsBusy());

  IOStatus ios = zbd_be_->Reset(start_, &offline, &max_capacity);
  if (ios != IOStatus::OK()) return ios;

  if (offline)
    capacity_ = 0;
  else
    max_capacity_ = capacity_ = max_capacity;

  wp_ = start_;
  lifetime_ = Env::WLTH_NOT_SET;

  reset_count_++;              
  zbd_->total_reset_count_++;  

  io_zones_reset_count = zbd_->GetTotalResetCount() - zbd_->GetMetaZoneResetCountNow();
  if (zbd_->GetTotalResetCount() > zbd_->GetNrZones()) {
    if (zbd_->GetCheckResetCount() < zbd_->GetNrZones()) {
      zbd_->SetCheckResetCount(zbd_->GetNrZones());
    }
    reset_count_diff = io_zones_reset_count - zbd_->GetCheckResetCount();
    if ((100 * reset_count_diff >
         io_zones_reset_count * zbd_->reset_ratio_threshold_) &&
        (reset_count_diff >= zbd_->GetNrZones())) {
      if (zbd_->wl_trigger_count_ >= 2) {
        zbd_->wl_trigger_count_ = 0; 
        double reset_count_std_dev = zbd_->GetResetCountStdDev();
        zbd_->reset_ratio_threshold_ = zbd_->reset_ratio_threshold_ /
                                       (1 + (reset_count_std_dev - 1.5) / 1.5);
      }
      zbd_->WakeupWLWorker();
      zbd_->SetCheckResetCount(io_zones_reset_count);
    }
  }

  return IOStatus::OK();
}

uint64_t Zone::GetZoneReclaimableSpace() {
  uint64_t reclaimable = 0;

  if (IsFull()) {
    reclaimable = max_capacity_ - used_capacity_;
  } else {
    reclaimable = wp_ - start_ - used_capacity_;
  }

  return reclaimable;
}

IOStatus Zone::Finish() {
  assert(IsBusy());

  IOStatus ios = zbd_be_->Finish(start_);
  if (ios != IOStatus::OK()) return ios;

  capacity_ = 0;
  wp_ = start_ + zbd_->GetZoneSize();

  return IOStatus::OK();
}

IOStatus Zone::Close() {
  assert(IsBusy());

  if (!(IsEmpty() || IsFull())) {
    IOStatus ios = zbd_be_->Close(start_);
    if (ios != IOStatus::OK()) return ios;
  }

  return IOStatus::OK();
}

IOStatus Zone::Append(char *data, uint32_t size) {
  ZenFSMetricsLatencyGuard guard(zbd_->GetMetrics(), ZENFS_ZONE_WRITE_LATENCY,
                                 Env::Default());
  zbd_->GetMetrics()->ReportThroughput(ZENFS_ZONE_WRITE_THROUGHPUT, size);
  zbd_->GetMetricsQps()->ReportQPS(ZENFS_WRITE_QPS, 1);

  char *ptr = data;
  uint32_t left = size;
  int ret;

  if (capacity_ < size)
    return IOStatus::NoSpace("Not enough capacity for append");

  assert((size % zbd_->GetBlockSize()) == 0);

  while (left) {
    ret = zbd_be_->Write(ptr, left, wp_);
    if (ret < 0) {
      return IOStatus::IOError(strerror(errno));
    }

    ptr += ret;
    wp_ += ret;
    capacity_ -= ret;
    left -= ret;
    zbd_->AddBytesWritten(ret);
  }

  return IOStatus::OK();
}

inline IOStatus Zone::CheckRelease() {
  if (!Release()) {
    assert(false);
    return IOStatus::Corruption("Failed to unset busy flag of zone " +
                                std::to_string(GetZoneNr()));
  }

  return IOStatus::OK();
}

Zone *ZonedBlockDevice::GetIOZone(uint64_t offset) {
  for (const auto z : io_zones)
    if (z->start_ <= offset && offset < (z->start_ + zbd_be_->GetZoneSize()))
      return z;
  return nullptr;
}

ZonedBlockDevice::ZonedBlockDevice(std::string path, ZbdBackendType backend,
                                   std::shared_ptr<Logger> logger,
                                   std::shared_ptr<ZenFSMetrics> metrics)
    : logger_(logger), metrics_(metrics) {
  total_reset_count_ = 0;
  check_reset_count_ = 0;
  metrics_qps_ = std::make_shared<ZenFSMetricsQps>(Env::Default());
  if (backend == ZbdBackendType::kBlockDev) {
    zbd_be_ = std::unique_ptr<ZbdlibBackend>(new ZbdlibBackend(path));
    Info(logger_, "New Zoned Block Device: %s", zbd_be_->GetFilename().c_str());
  } else if (backend == ZbdBackendType::kZoneFS) {
    zbd_be_ = std::unique_ptr<ZoneFsBackend>(new ZoneFsBackend(path));
    Info(logger_, "New zonefs backing: %s", zbd_be_->GetFilename().c_str());
  }
}

uint32_t ZonedBlockDevice::GetIOZoneResetCountNow() {
  uint32_t total = 0;
  for (const auto z : io_zones) {
    total += z->reset_count_;
  }
  return total;
}

uint32_t* ZonedBlockDevice::GetIOZonesResetCountArray() {
  uint32_t *reset_count = new uint32_t[GetNrIOZones()];
  int i = 0;
  for (const auto z : io_zones) {
    reset_count[i] = z->reset_count_;
    i++;
  }
  return reset_count;
}

void ZonedBlockDevice::SetIOZonesResetCount(uint32_t *reset_count) {
  int i = 0;
  for (const auto z : io_zones) {
    z->reset_count_ = reset_count[i];
    i++;
  }
}

uint32_t ZonedBlockDevice::GetMetaZoneResetCountNow() {
  uint32_t total = 0;
  for (const auto z : meta_zones) {
    total += z->reset_count_;
  }
  return total;
}

void ZonedBlockDevice::SleepWLWorker() {
  std::unique_lock<std::mutex> wl_lock(wl_worker_mutex_);
  wl_worker_sleep_ = true;
}

void ZonedBlockDevice::WakeupWLWorker() {
  std::unique_lock<std::mutex> wl_lock(wl_worker_mutex_);
  wl_worker_cv_.notify_one();
  wl_worker_sleep_ = false;
}

double ZonedBlockDevice::GetResetCountStdDev() {
  double mean = 0.0;
  double sum = 0.0;
  double variance = 0.0;
  int n = io_zones.size();

  mean = GetIOZoneResetCountNow() / n;
  for (const auto z : io_zones) {
    sum += pow(z->reset_count_ - mean, 2);
  }
  variance = sum / n;
  double std_dev = sqrt(variance);

  return std_dev;
}

IOStatus ZonedBlockDevice::GetLeastResetCountZone(Zone **out_zone) {
  Zone *least_reset_count_zone = nullptr;
  uint64_t least_reset_count_zone_score = 0;

  for (const auto z : io_zones) {
    if (z->IsEmpty()) {
      continue;
    }
    if (z->IsUsed()) {
      if (z->lifetime_ == Env::WLTH_EXTREME) {
        uint64_t reclaimable_space = z->GetZoneReclaimableSpace();
        if (reclaimable_space != 0) {
          uint64_t zone_score =
              z->reset_count_ * z->max_capacity_ / reclaimable_space;
          if (least_reset_count_zone == nullptr ||
              zone_score < least_reset_count_zone_score ||
              (zone_score == least_reset_count_zone_score &&
               reclaimable_space >
                   least_reset_count_zone->GetZoneReclaimableSpace())) {
            least_reset_count_zone = z;
            least_reset_count_zone_score = zone_score;
          }
        } else {
          continue;
        }
      }
    }
  }
  *out_zone = least_reset_count_zone;
  if ((*out_zone) == nullptr) {
    return IOStatus::NotFound("The zone with the fewest resets was not found");
  }
  return IOStatus::OK();
}

void ZonedBlockDevice::GetLifetimeZeroZone(std::vector<Zone *> &zero_lifetime_zones) {
  for (auto z : io_zones) {
    if (z->IsUsed()) {
      if (z->GetLifeTimeHint() == Env::WLTH_NOT_SET) {
        zero_lifetime_zones.push_back(z);
      }
    }
  }
}

int ZonedBlockDevice::JudgeQpsTrend() {

  ClearNowQps();
  usleep(1000 * 100);
  uint64_t qps_write1 = GetNowWriteQps();
  uint64_t qps_read1 = GetNowReadQps();

  ClearNowQps();
  usleep(1000 * 100);
  uint64_t qps_write2 = GetNowWriteQps();
  uint64_t qps_read2 = GetNowReadQps();

  if (qps_write2 > qps_write1) {
    if (qps_write2 > window_qps_write_max_) {
      window_qps_write_max_ = qps_write2;
    }
  } else {
    if (qps_write1 > window_qps_write_max_) {
      window_qps_write_max_ = qps_write1;
    }
  }

  if (qps_read2 > qps_read1) {
    if (qps_read2 > window_qps_read_max_) {
      window_qps_read_max_ = qps_read2;
    }
  } else {
    if (qps_read1 > window_qps_read_max_) {
      window_qps_read_max_ = qps_read1;
    }
  }

  if (idle_qps_fail_count_ >= 5) {
    if (window_qps_write_max_ > idle_qps_write_threshold_){
      idle_qps_write_threshold_ = (idle_qps_write_threshold_ + window_qps_write_max_) / 2;
    }
    if (window_qps_read_max_ > idle_qps_read_threshold_){
      idle_qps_read_threshold_ = (idle_qps_read_threshold_ + window_qps_read_max_) / 2;
    }
    window_qps_write_max_ = 0;
    window_qps_read_max_ = 0;
    idle_qps_fail_count_ = 0;
  }

  if ((idle_qps_write_threshold_ != 76) || (idle_qps_read_threshold_ != 5000)) {
    if (idle_qps_successive_count_ >= 5) {
      idle_qps_write_threshold_ = 76;
      idle_qps_read_threshold_ = 5000;
      idle_qps_successive_count_ = 0;
    }
  }

  if ((qps_write1 < idle_qps_write_threshold_) && (qps_write2 < idle_qps_write_threshold_)) {
    if ((qps_read1 < idle_qps_read_threshold_) && (qps_read2 < idle_qps_read_threshold_)) {
      return 1;
    }
    if (qps_read2 > qps_read1) {
      return 0;
    }
    if (100 * (qps_read1 - qps_read2) > idle_qps_read_threshold_ * 5) {
      return 1;
    }
  } else {
    if (qps_write2 > qps_write1) {
      return 0;
    }
    if ((qps_read1 < idle_qps_read_threshold_) && (qps_read2 < idle_qps_read_threshold_)) {
      if (100 * (qps_write1 - qps_write2) > idle_qps_write_threshold_ * 5) {
        return 1;
      }
    }
  }

  return 0;
}
IOStatus ZonedBlockDevice::Open(bool readonly, bool exclusive) {
  std::unique_ptr<ZoneList> zone_rep;
  unsigned int max_nr_active_zones;
  unsigned int max_nr_open_zones;
  Status s;
  uint64_t i = 0;
  uint64_t m = 0;
  // Reserve one zone for metadata and another one for extent migration
  int reserved_zones = 2;

  if (!readonly && !exclusive)
    return IOStatus::InvalidArgument("Write opens must be exclusive");

  IOStatus ios = zbd_be_->Open(readonly, exclusive, &max_nr_active_zones,
                               &max_nr_open_zones);
  if (ios != IOStatus::OK()) return ios;

  if (zbd_be_->GetNrZones() < ZENFS_MIN_ZONES) {
    return IOStatus::NotSupported("To few zones on zoned backend (" +
                                  std::to_string(ZENFS_MIN_ZONES) +
                                  " required)");
  }

  if (max_nr_active_zones == 0)
    max_nr_active_io_zones_ = zbd_be_->GetNrZones();
  else
    max_nr_active_io_zones_ = max_nr_active_zones - reserved_zones;

  if (max_nr_open_zones == 0)
    max_nr_open_io_zones_ = zbd_be_->GetNrZones();
  else
    max_nr_open_io_zones_ = max_nr_open_zones - reserved_zones;

  Info(logger_, "Zone block device nr zones: %u max active: %u max open: %u \n",
       zbd_be_->GetNrZones(), max_nr_active_zones, max_nr_open_zones);

  zone_rep = zbd_be_->ListZones();
  if (zone_rep == nullptr || zone_rep->ZoneCount() != zbd_be_->GetNrZones()) {
    Error(logger_, "Failed to list zones");
    return IOStatus::IOError("Failed to list zones");
  }

  while (m < ZENFS_META_ZONES && i < zone_rep->ZoneCount()) {
    /* Only use sequential write required zones */
    if (zbd_be_->ZoneIsSwr(zone_rep, i)) {
      if (!zbd_be_->ZoneIsOffline(zone_rep, i)) {
        meta_zones.push_back(new Zone(this, zbd_be_.get(), zone_rep, i));
      }
      m++;
    }
    i++;
  }

  active_io_zones_ = 0;
  open_io_zones_ = 0;

  for (; i < zone_rep->ZoneCount(); i++) {
    /* Only use sequential write required zones */
    if (zbd_be_->ZoneIsSwr(zone_rep, i)) {
      if (!zbd_be_->ZoneIsOffline(zone_rep, i)) {
        Zone *newZone = new Zone(this, zbd_be_.get(), zone_rep, i);
        if (!newZone->Acquire()) {
          assert(false);
          return IOStatus::Corruption("Failed to set busy flag of zone " +
                                      std::to_string(newZone->GetZoneNr()));
        }
        io_zones.push_back(newZone);
        if (zbd_be_->ZoneIsActive(zone_rep, i)) {
          active_io_zones_++;
          if (zbd_be_->ZoneIsOpen(zone_rep, i)) {
            if (!readonly) {
              newZone->Close();
            }
          }
        }
        IOStatus status = newZone->CheckRelease();
        if (!status.ok()) {
          return status;
        }
      }
    }
  }

  start_time_ = time(NULL);

  return IOStatus::OK();
}

uint64_t ZonedBlockDevice::GetFreeSpace() {
  uint64_t free = 0;
  for (const auto z : io_zones) {
    free += z->capacity_;
  }
  return free;
}

uint64_t ZonedBlockDevice::GetUsedSpace() {
  uint64_t used = 0;
  for (const auto z : io_zones) {
    used += z->used_capacity_;
  }
  return used;
}

uint64_t ZonedBlockDevice::GetReclaimableSpace() {
  uint64_t reclaimable = 0;
  for (const auto z : io_zones) {
    if (z->IsFull()) reclaimable += (z->max_capacity_ - z->used_capacity_);
  }
  return reclaimable;
}

void ZonedBlockDevice::LogZoneStats() {
  uint64_t used_capacity = 0;
  uint64_t reclaimable_capacity = 0;
  uint64_t reclaimables_max_capacity = 0;
  uint64_t active = 0;

  for (const auto z : io_zones) {
    used_capacity += z->used_capacity_;

    if (z->used_capacity_) {
      reclaimable_capacity += z->max_capacity_ - z->used_capacity_;
      reclaimables_max_capacity += z->max_capacity_;
    }

    if (!(z->IsFull() || z->IsEmpty())) active++;
  }

  if (reclaimables_max_capacity == 0) reclaimables_max_capacity = 1;

  Info(logger_,
       "[Zonestats:time(s),used_cap(MB),reclaimable_cap(MB), "
       "avg_reclaimable(%%), active(#), active_zones(#), open_zones(#)] %ld "
       "%lu %lu %lu %lu %ld %ld\n",
       time(NULL) - start_time_, used_capacity / MB, reclaimable_capacity / MB,
       100 * reclaimable_capacity / reclaimables_max_capacity, active,
       active_io_zones_.load(), open_io_zones_.load());
}

void ZonedBlockDevice::LogZoneUsage() {
  for (const auto z : io_zones) {
    int64_t used = z->used_capacity_;

    if (used > 0) {
      Debug(logger_, "Zone 0x%lX used capacity: %ld bytes (%ld MB)\n",
            z->start_, used, used / MB);
    }
  }
}

void ZonedBlockDevice::LogGarbageInfo() {
  // Log zone garbage stats vector.
  //
  // The values in the vector represents how many zones with target garbage
  // percent. Garbage percent of each index: [0%, <10%, < 20%, ... <100%, 100%]
  // For example `[100, 1, 2, 3....]` means 100 zones are empty, 1 zone has less
  // than 10% garbage, 2 zones have  10% ~ 20% garbage ect.
  //
  // We don't need to lock io_zones since we only read data and we don't need
  // the result to be precise.
  int zone_gc_stat[12] = {0};
  for (auto z : io_zones) {
    if (!z->Acquire()) {
      continue;
    }

    if (z->IsEmpty()) {
      zone_gc_stat[0]++;
      z->Release();
      continue;
    }

    double garbage_rate = 0;
    if (z->IsFull()) {
      garbage_rate =
          double(z->max_capacity_ - z->used_capacity_) / z->max_capacity_;
    } else {
      garbage_rate =
          double(z->wp_ - z->start_ - z->used_capacity_) / z->max_capacity_;
    }
    assert(garbage_rate >= 0);
    int idx = int((garbage_rate + 0.1) * 10);
    zone_gc_stat[idx]++;

    z->Release();
  }

  std::stringstream ss;
  ss << "Zone Garbage Stats: [";
  for (int i = 0; i < 12; i++) {
    ss << zone_gc_stat[i] << " ";
  }
  ss << "]";
  Info(logger_, "%s", ss.str().data());
}

ZonedBlockDevice::~ZonedBlockDevice() {
  for (const auto z : meta_zones) {
    delete z;
  }

  for (const auto z : io_zones) {
    delete z;
  }
}

#define LIFETIME_DIFF_NOT_GOOD (100)
#define LIFETIME_DIFF_COULD_BE_WORSE (50)

unsigned int GetLifeTimeDiff(Env::WriteLifeTimeHint zone_lifetime,
                             Env::WriteLifeTimeHint file_lifetime) {
  assert(file_lifetime <= Env::WLTH_EXTREME);

  if ((file_lifetime == Env::WLTH_NOT_SET) ||
      (file_lifetime == Env::WLTH_NONE)) {
    if (file_lifetime == zone_lifetime) {
      return 0;
    } else {
      return LIFETIME_DIFF_NOT_GOOD;
    }
  }

  if (zone_lifetime > file_lifetime) return zone_lifetime - file_lifetime;
  if (zone_lifetime == file_lifetime) return LIFETIME_DIFF_COULD_BE_WORSE;

  return LIFETIME_DIFF_NOT_GOOD;
}

IOStatus ZonedBlockDevice::GetMigrateTargetZone(
    Zone **out_zone, Env::WriteLifeTimeHint file_lifetime,
    uint64_t min_capacity) {
  std::unique_lock<std::mutex> lock(migrate_zone_mtx_);
  migrate_resource_.wait(lock, [this] { return !migrating_; });

  migrating_ = true;

  Zone *target_zone = nullptr;
  uint64_t target_zone_score = 0;
  IOStatus s;

  WaitForOpenIOZoneToken(true);
  for (const auto z : io_zones) {
    if (z->Acquire()) {
      if (z->IsEmpty()) {
        if (target_zone == nullptr ||
            z->reset_count_ > target_zone->reset_count_) {
          if (target_zone != nullptr) {
            s = target_zone->CheckRelease();
            if (!s.ok()) {
              IOStatus s_ = z->CheckRelease();
              if (!s_.ok()) {
                PutOpenIOZoneToken();
                return s_;
              }
              PutOpenIOZoneToken();
              return s;
            }
          }
          target_zone = z;
        } else {
          s = z->CheckRelease();
          if (!s.ok()) {
            PutOpenIOZoneToken();
            return s;
          }
        }
      } else {
        s = z->CheckRelease();
        if (!s.ok()) {
          PutOpenIOZoneToken();
          return s;
        }
      }
    }
  }

  if (target_zone != nullptr) {
    bool got_token = GetActiveIOZoneTokenIfAvailable();
    if (!got_token) {
      PutOpenIOZoneToken();
      target_zone->Release();
      target_zone = nullptr;
    } else { 
      assert(target_zone->IsBusy());
      target_zone->lifetime_ = file_lifetime;
    }
  }

  if (target_zone == nullptr) {
    for (const auto z : io_zones) {
      if (z->Acquire()) {
        if ((z->used_capacity_ > 0) && !z->IsFull() &&
            z->capacity_ >= min_capacity) {
          uint64_t reclaimable_space = z->GetZoneReclaimableSpace();
          uint64_t zone_score =
              z->reset_count_ * reclaimable_space / z->max_capacity_;
          if (target_zone == nullptr ||
              zone_score > target_zone_score ||
              (zone_score == target_zone_score &&
               z->reset_count_ > target_zone->reset_count_)) {
            unsigned int diff = GetLifeTimeDiff(z->lifetime_, file_lifetime);
            if (diff != LIFETIME_DIFF_NOT_GOOD) {
              if (target_zone != nullptr) {
                s = target_zone->CheckRelease();
                if (!s.ok()) {
                  IOStatus s_ = z->CheckRelease();
                  if (!s_.ok()) return s_;
                  return s;
                }
              }
              target_zone = z;
              target_zone_score = zone_score;
            } else {
              s = z->CheckRelease();
              if (!s.ok()) return s;
            }
          } else {
            s = z->CheckRelease();
            if (!s.ok()) return s;
          }
        } else {
          s = z->CheckRelease();
          if (!s.ok()) return s;
        }
      }
    }
  }

  *out_zone = target_zone;
  if ((*out_zone) == nullptr) {
    migrating_ = false;
    return IOStatus::NotFound("The migrate target zone was not found");
  } else {
    Info(logger_, "Take Wear Leveling Migrate Zone: %lu", (*out_zone)->start_);
    return IOStatus::OK();
  }
}

IOStatus ZonedBlockDevice::AllocateMetaZone(Zone **out_meta_zone) {
  assert(out_meta_zone);
  *out_meta_zone = nullptr;
  ZenFSMetricsLatencyGuard guard(metrics_, ZENFS_META_ALLOC_LATENCY,
                                 Env::Default());
  metrics_->ReportQPS(ZENFS_META_ALLOC_QPS, 1);

  for (const auto z : meta_zones) {
    /* If the zone is not used, reset and use it */
    if (z->Acquire()) {
      if (!z->IsUsed()) {
        if (!z->IsEmpty() && !z->Reset().ok()) {
          Warn(logger_, "Failed resetting zone!");
          IOStatus status = z->CheckRelease();
          if (!status.ok()) return status;
          continue;
        }
        *out_meta_zone = z;
        return IOStatus::OK();
      }
    }
  }
  assert(true);
  Error(logger_, "Out of metadata zones, we should go to read only now.");
  return IOStatus::NoSpace("Out of metadata zones");
}

IOStatus ZonedBlockDevice::ResetUnusedIOZones() {
  for (const auto z : io_zones) {
    if (z->Acquire()) {
      if (!z->IsEmpty() && !z->IsUsed()) {
        bool full = z->IsFull();
        IOStatus reset_status = z->Reset();
        IOStatus release_status = z->CheckRelease();
        if (!reset_status.ok()) return reset_status;
        if (!release_status.ok()) return release_status;
        if (!full) PutActiveIOZoneToken();
      } else {
        IOStatus release_status = z->CheckRelease();
        if (!release_status.ok()) return release_status;
      }
    }
  }
  return IOStatus::OK();
}

void ZonedBlockDevice::WaitForOpenIOZoneToken(bool prioritized) {
  long allocator_open_limit;

  /* Avoid non-priortized allocators from starving prioritized ones */
  if (prioritized) {
    allocator_open_limit = max_nr_open_io_zones_;
  } else {
    allocator_open_limit = max_nr_open_io_zones_ - 1;
  }

  /* Wait for an open IO Zone token - after this function returns
   * the caller is allowed to write to a closed zone. The callee
   * is responsible for calling a PutOpenIOZoneToken to return the resource
   */
  std::unique_lock<std::mutex> lk(zone_resources_mtx_);
  zone_resources_.wait(lk, [this, allocator_open_limit] {
    if (open_io_zones_.load() < allocator_open_limit) {
      open_io_zones_++;
      return true;
    } else {
      return false;
    }
  });
}

bool ZonedBlockDevice::GetActiveIOZoneTokenIfAvailable() {
  /* Grap an active IO Zone token if available - after this function returns
   * the caller is allowed to write to a closed zone. The callee
   * is responsible for calling a PutActiveIOZoneToken to return the resource
   */
  std::unique_lock<std::mutex> lk(zone_resources_mtx_);
  if (active_io_zones_.load() < max_nr_active_io_zones_) {
    active_io_zones_++;
    return true;
  }
  return false;
}

void ZonedBlockDevice::PutOpenIOZoneToken() {
  {
    std::unique_lock<std::mutex> lk(zone_resources_mtx_);
    open_io_zones_--;
  }
  zone_resources_.notify_one();
}

void ZonedBlockDevice::PutActiveIOZoneToken() {
  {
    std::unique_lock<std::mutex> lk(zone_resources_mtx_);
    active_io_zones_--;
  }
  zone_resources_.notify_one();
}

IOStatus ZonedBlockDevice::ApplyFinishThreshold() {
  IOStatus s;

  if (finish_threshold_ == 0) return IOStatus::OK();

  for (const auto z : io_zones) {
    if (z->Acquire()) {
      bool within_finish_threshold =
          z->capacity_ < (z->max_capacity_ * finish_threshold_ / 100);
      if (!(z->IsEmpty() || z->IsFull()) && within_finish_threshold) {
        /* If there is less than finish_threshold_% remaining capacity in a
         * non-open-zone, finish the zone */
        s = z->Finish();
        if (!s.ok()) {
          z->Release();
          Debug(logger_, "Failed finishing zone");
          return s;
        }
        s = z->CheckRelease();
        if (!s.ok()) return s;
        PutActiveIOZoneToken();
      } else {
        s = z->CheckRelease();
        if (!s.ok()) return s;
      }
    }
  }

  return IOStatus::OK();
}

IOStatus ZonedBlockDevice::FinishCheapestIOZone() {
  IOStatus s;
  Zone *finish_victim = nullptr;

  for (const auto z : io_zones) {
    if (z->Acquire()) {
      if (z->IsEmpty() || z->IsFull()) {
        s = z->CheckRelease();
        if (!s.ok()) return s;
        continue;
      }
      if (finish_victim == nullptr) {
        finish_victim = z;
        continue;
      }
      if (finish_victim->capacity_ > z->capacity_) {
        s = finish_victim->CheckRelease();
        if (!s.ok()) return s;
        finish_victim = z;
      } else {
        s = z->CheckRelease();
        if (!s.ok()) return s;
      }
    }
  }

  // If all non-busy zones are empty or full, we should return success.
  if (finish_victim == nullptr) {
    Info(logger_, "All non-busy zones are empty or full, skip.");
    return IOStatus::OK();
  }

  s = finish_victim->Finish();
  IOStatus release_status = finish_victim->CheckRelease();

  if (s.ok()) {
    PutActiveIOZoneToken();
  }

  if (!release_status.ok()) {
    return release_status;
  }

  return s;
}

IOStatus ZonedBlockDevice::GetBestOpenZoneMatch(
    Env::WriteLifeTimeHint file_lifetime, unsigned int *best_diff_out,
    Zone **zone_out, uint32_t min_capacity) {
  unsigned int best_diff = LIFETIME_DIFF_NOT_GOOD;
  Zone *allocated_zone = nullptr;
  IOStatus s;

  for (const auto z : io_zones) {
    if (z->Acquire()) {
      if ((z->used_capacity_ > 0) && !z->IsFull() &&
          z->capacity_ >= min_capacity) {
        unsigned int diff = GetLifeTimeDiff(z->lifetime_, file_lifetime);
        if (diff <= best_diff) {
          if (allocated_zone != nullptr) {
            s = allocated_zone->CheckRelease();
            if (!s.ok()) {
              IOStatus s_ = z->CheckRelease();
              if (!s_.ok()) return s_;
              return s;
            }
          }
          allocated_zone = z;
          best_diff = diff;
        } else {
          s = z->CheckRelease();
          if (!s.ok()) return s;
        }
      } else {
        s = z->CheckRelease();
        if (!s.ok()) return s;
      }
    }
  }

  *best_diff_out = best_diff;
  *zone_out = allocated_zone;

  return IOStatus::OK();
}

IOStatus ZonedBlockDevice::AllocateEmptyZone(Zone **zone_out) {
  IOStatus s;
  Zone *allocated_zone = nullptr;
  for (const auto z : io_zones) {
    if (z->Acquire()) {
      if (z->IsEmpty()) {
        allocated_zone = z;
        break;
      } else {
        s = z->CheckRelease();
        if (!s.ok()) return s;
      }
    }
  }
  *zone_out = allocated_zone;
  return IOStatus::OK();
}
IOStatus ZonedBlockDevice::AllocateEmptyZone(Env::WriteLifeTimeHint file_lifetime, Zone **zone_out) {
  IOStatus s;
  Zone *allocated_zone = nullptr;

  if (file_lifetime < Env::WLTH_SHORT) {
    for (const auto z : io_zones) {
      if (z->Acquire()) {
        if (z->IsEmpty()) {
          if (allocated_zone == nullptr ||
              z->reset_count_ > allocated_zone->reset_count_) {
            if (allocated_zone != nullptr) {
              s = allocated_zone->CheckRelease();
              if (!s.ok()) {
                IOStatus s_ = z->CheckRelease();
                if (!s_.ok()) return s_;
                return s;
              }
            }
            allocated_zone = z;
          } else {
            s = z->CheckRelease();
            if (!s.ok()) return s;
          }
        } else {
          s = z->CheckRelease();
          if (!s.ok()) return s;
        }
      }
    }
  } else if(file_lifetime >= Env::WLTH_SHORT) {
    for (const auto z : io_zones) {
      if (z->Acquire()) {
        if (z->IsEmpty()) {
          if (allocated_zone == nullptr ||
              z->reset_count_ < allocated_zone->reset_count_) {
            if (allocated_zone != nullptr) {
              s = allocated_zone->CheckRelease();
              if (!s.ok()) {
                IOStatus s_ = z->CheckRelease();
                if (!s_.ok()) return s_;
                return s;
              }
            }
            allocated_zone = z;
            if (allocated_zone->reset_count_ == 0) {
              break;
            }
          } else {
            s = z->CheckRelease();
            if (!s.ok()) return s;
          }
        } else {
          s = z->CheckRelease();
          if (!s.ok()) return s;
        }
      }
    }
  }

  *zone_out = allocated_zone;
  return IOStatus::OK();
}

IOStatus ZonedBlockDevice::InvalidateCache(uint64_t pos, uint64_t size) {
  int ret = zbd_be_->InvalidateCache(pos, size);

  if (ret) {
    return IOStatus::IOError("Failed to invalidate cache");
  }
  return IOStatus::OK();
}

int ZonedBlockDevice::Read(char *buf, uint64_t offset, int n, bool direct) {
    
  metrics_qps_->ReportQPS(ZENFS_READ_QPS, 1);
 
  int ret = 0;
  int left = n;
  int r = -1;

  while (left) {
    r = zbd_be_->Read(buf, left, offset, direct);
    if (r <= 0) {
      if (r == -1 && errno == EINTR) {
        continue;
      }
      break;
    }
    ret += r;
    buf += r;
    left -= r;
    offset += r;
  }

  if (r < 0) return r;
  return ret;
}

IOStatus ZonedBlockDevice::ReleaseMigrateZone(Zone *zone) {
  IOStatus s = IOStatus::OK();
  {
    std::unique_lock<std::mutex> lock(migrate_zone_mtx_);
    migrating_ = false;
    if (zone != nullptr) {
      s = zone->CheckRelease();
      Info(logger_, "ReleaseMigrateZone: %lu", zone->start_);
    }
  }
  migrate_resource_.notify_one();
  return s;
}

IOStatus ZonedBlockDevice::TakeMigrateZone(Zone **out_zone,
                                           Env::WriteLifeTimeHint file_lifetime,
                                           uint32_t min_capacity) {
  std::unique_lock<std::mutex> lock(migrate_zone_mtx_);
  migrate_resource_.wait(lock, [this] { return !migrating_; });

  migrating_ = true;

  unsigned int best_diff = LIFETIME_DIFF_NOT_GOOD;
  auto s =
      GetBestOpenZoneMatch(file_lifetime, &best_diff, out_zone, min_capacity);
  if (s.ok() && (*out_zone) != nullptr) {
    Info(logger_, "TakeMigrateZone: %lu", (*out_zone)->start_);
  } else {
    migrating_ = false;
  }

  return s;
}

IOStatus ZonedBlockDevice::AllocateIOZone(Env::WriteLifeTimeHint file_lifetime,
                                          IOType io_type, Zone **out_zone) {
  Zone *allocated_zone = nullptr;
  unsigned int best_diff = LIFETIME_DIFF_NOT_GOOD;
  int new_zone = 0;
  IOStatus s;

  auto tag = ZENFS_WAL_IO_ALLOC_LATENCY;
  if (io_type != IOType::kWAL) {
    // L0 flushes have lifetime MEDIUM
    if (file_lifetime == Env::WLTH_MEDIUM) {
      tag = ZENFS_L0_IO_ALLOC_LATENCY;
    } else {
      tag = ZENFS_NON_WAL_IO_ALLOC_LATENCY;
    }
  }

  ZenFSMetricsLatencyGuard guard(metrics_, tag, Env::Default());
  metrics_->ReportQPS(ZENFS_IO_ALLOC_QPS, 1);

  // Check if a deferred IO error was set
  s = GetZoneDeferredStatus();
  if (!s.ok()) {
    return s;
  }

  if (io_type != IOType::kWAL) {
    s = ApplyFinishThreshold();
    if (!s.ok()) {
      return s;
    }
  }

  WaitForOpenIOZoneToken(io_type == IOType::kWAL);

  /* Try to fill an already open zone(with the best life time diff) */
  s = GetBestOpenZoneMatch(file_lifetime, &best_diff, &allocated_zone);
  if (!s.ok()) {
    PutOpenIOZoneToken();
    return s;
  }

  // Holding allocated_zone if != nullptr

  if (best_diff >= LIFETIME_DIFF_COULD_BE_WORSE) {
    bool got_token = GetActiveIOZoneTokenIfAvailable();

    /* If we did not get a token, try to use the best match, even if the life
     * time diff not good but a better choice than to finish an existing zone
     * and open a new one
     */
    if (allocated_zone != nullptr) {
      if (!got_token && best_diff == LIFETIME_DIFF_COULD_BE_WORSE) {
        Debug(logger_,
              "Allocator: avoided a finish by relaxing lifetime diff "
              "requirement\n");
      } else {
        s = allocated_zone->CheckRelease();
        if (!s.ok()) {
          PutOpenIOZoneToken();
          if (got_token) PutActiveIOZoneToken();
          return s;
        }
        allocated_zone = nullptr;
      }
    }

    /* If we haven't found an open zone to fill, open a new zone */
    if (allocated_zone == nullptr) {
      /* We have to make sure we can open an empty zone */
      while (!got_token && !GetActiveIOZoneTokenIfAvailable()) {
        s = FinishCheapestIOZone();
        if (!s.ok()) {
          PutOpenIOZoneToken();
          return s;
        }
      }

      // s = AllocateEmptyZone(&allocated_zone);
      s = AllocateEmptyZone(file_lifetime, &allocated_zone);
      if (!s.ok()) {
        PutActiveIOZoneToken();
        PutOpenIOZoneToken();
        return s;
      }

      if (allocated_zone != nullptr) {
        assert(allocated_zone->IsBusy());
        allocated_zone->lifetime_ = file_lifetime;
        new_zone = true;
      } else {
        PutActiveIOZoneToken();
      }
    }
  }

  if (allocated_zone) {
    assert(allocated_zone->IsBusy());
    Debug(logger_,
          "Allocating zone(new=%d) start: 0x%lx wp: 0x%lx lt: %d file lt: %d\n",
          new_zone, allocated_zone->start_, allocated_zone->wp_,
          allocated_zone->lifetime_, file_lifetime);
  } else {
    PutOpenIOZoneToken();
  }

  if (io_type != IOType::kWAL) {
    LogZoneStats();
  }

  *out_zone = allocated_zone;

  metrics_->ReportGeneral(ZENFS_OPEN_ZONES_COUNT, open_io_zones_);
  metrics_->ReportGeneral(ZENFS_ACTIVE_ZONES_COUNT, active_io_zones_);

  return IOStatus::OK();
}

std::string ZonedBlockDevice::GetFilename() { return zbd_be_->GetFilename(); }

uint32_t ZonedBlockDevice::GetBlockSize() { return zbd_be_->GetBlockSize(); }

uint64_t ZonedBlockDevice::GetZoneSize() { return zbd_be_->GetZoneSize(); }

uint32_t ZonedBlockDevice::GetNrZones() { return zbd_be_->GetNrZones(); }

uint32_t ZonedBlockDevice::GetNrIOZones() { return zbd_be_->GetNrIOZones(); }

void ZonedBlockDevice::EncodeJsonZone(std::ostream &json_stream,
                                      const std::vector<Zone *> zones) {
  bool first_element = true;
  json_stream << "[";
  for (Zone *zone : zones) {
    if (first_element) {
      first_element = false;
    } else {
      json_stream << ",";
    }
    zone->EncodeJson(json_stream);
  }

  json_stream << "]";
}

void ZonedBlockDevice::EncodeJson(std::ostream &json_stream) {
  json_stream << "{";
  json_stream << "\"meta\":";
  EncodeJsonZone(json_stream, meta_zones);
  json_stream << ",\"io\":";
  EncodeJsonZone(json_stream, io_zones);
  json_stream << "}";
}

IOStatus ZonedBlockDevice::GetZoneDeferredStatus() {
  std::lock_guard<std::mutex> lock(zone_deferred_status_mutex_);
  return zone_deferred_status_;
}

void ZonedBlockDevice::SetZoneDeferredStatus(IOStatus status) {
  std::lock_guard<std::mutex> lk(zone_deferred_status_mutex_);
  if (!zone_deferred_status_.ok()) {
    zone_deferred_status_ = status;
  }
}

void ZonedBlockDevice::GetZoneSnapshot(std::vector<ZoneSnapshot> &snapshot) {
  for (auto *zone : io_zones) {
    snapshot.emplace_back(*zone);
  }
}

}  // namespace ROCKSDB_NAMESPACE

#endif  // !defined(ROCKSDB_LITE) && !defined(OS_WIN)
