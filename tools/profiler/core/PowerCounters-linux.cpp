/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "PowerCounters.h"
#include "nsXULAppAPI.h"
#include "mozilla/Maybe.h"
#include "mozilla/Logging.h"

#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cstdio>

#include <linux/perf_event.h>

// From the kernel rapl_scale() function:
//
// > users must then scale back: count * 1/(1e9*2^32) to get Joules
#define PERF_EVENT_SCALE_NANOJOULES 2.3283064365386962890625e-1
#define SCALE_NANOJOULES_TO_PICOWATTHOUR 3.6
#define SYSFS_PERF_POWER_TYPE_PATH "/sys/bus/event_source/devices/power/type"

static mozilla::LazyLogModule sRaplEventLog("profiler.rapl");
#define RAPL_LOG(...) \
  MOZ_LOG(sRaplEventLog, mozilla::LogLevel::Debug, (__VA_ARGS__));

enum class RaplEventType : uint64_t {
  RAPL_ENERGY_CORES = 0x01,
  RAPL_ENERGY_PKG = 0x02,
  RAPL_ENERGY_DRAM = 0x03,
  RAPL_ENERGY_GPU = 0x04,
  RAPL_ENERGY_PSYS = 0x05,
};

struct RaplDomain {
  RaplEventType mRaplEventType;
  const char* mLabel;
  const char* mDescription;
};

constexpr RaplDomain kSupportedRaplDomains[] = {
    {RaplEventType::RAPL_ENERGY_CORES, "Power: CPU cores",
     "Consumption of all physical cores"},
    {
        RaplEventType::RAPL_ENERGY_PKG,
        "Power: CPU package",
        "Consumption of the whole processor package",
    },
    {
        RaplEventType::RAPL_ENERGY_DRAM,
        "Power: DRAM",
        "Consumption of the dram domain",
    },
    {
        RaplEventType::RAPL_ENERGY_GPU,
        "Power: iGPU",
        "Consumption of the builtin-gpu domain",
    },
    {
        RaplEventType::RAPL_ENERGY_PSYS,
        "Power: System",
        "Consumption of the builtin-psys domain",
    }};

class RaplProfilerCount final : public BaseProfilerCount {
 public:
  explicit RaplProfilerCount(int aPerfEventType,
                             const RaplEventType& aPerfEventConfig,
                             const char* aLabel, const char* aDescription)
      : BaseProfilerCount(aLabel, nullptr, nullptr, "power", aDescription),
        mLastResult(0),
        mPerfEventFd(-1) {
    RAPL_LOG("Creating RAPL Event for type: %s", mLabel);

    // Optimize for ease of use and do not set an excludes value. This
    // ensures we do not require PERF_PMU_CAP_NO_EXCLUDE.
    struct perf_event_attr attr = {0};
    memset(&attr, 0, sizeof(attr));
    attr.type = aPerfEventType;
    attr.size = sizeof(struct perf_event_attr);
    attr.config = static_cast<uint64_t>(aPerfEventConfig);
    attr.sample_period = 0;
    attr.sample_type = PERF_SAMPLE_IDENTIFIER;
    attr.inherit = 1;

    long fd = syscall(__NR_perf_event_open, &attr, -1, 0, -1, 0);
    if (fd < 0) {
      RAPL_LOG("Event descriptor creation failed for event: %s", mLabel);
      mPerfEventFd = -1;
      return;
    }

    RAPL_LOG("Created descriptor for event: %s", mLabel)
    mPerfEventFd = static_cast<int>(fd);
  }

  ~RaplProfilerCount() {
    if (ValidPerfEventFd()) {
      ioctl(mPerfEventFd, PERF_EVENT_IOC_DISABLE, 0);
      close(mPerfEventFd);
    }
  }

  RaplProfilerCount(const RaplProfilerCount&) = delete;
  RaplProfilerCount& operator=(const RaplProfilerCount&) = delete;

  CountSample Sample() override {
    CountSample result = {
        .count = 0,
        .number = 0,
        .isSampleNew = false,
    };
    mozilla::Maybe<uint64_t> raplEventResult = ReadEventFd();

    if (raplEventResult.isNothing()) {
      return result;
    }

    // We need to return picowatthour to be consistent with the Windows
    // EMI API. As a result, the scale calculation should:
    //
    //  - Convert the returned value to nanojoules
    //  - Convert nanojoules to picowatthour
    double nanojoules = static_cast<double>(raplEventResult.value()) *
                        PERF_EVENT_SCALE_NANOJOULES;
    double picowatthours = nanojoules / SCALE_NANOJOULES_TO_PICOWATTHOUR;
    RAPL_LOG("Sample %s { count: %lu, last-result: %lu } = %lfJ", mLabel,
             raplEventResult.value(), mLastResult, nanojoules * 1e-9);

    result.count = static_cast<int64_t>(picowatthours);

    // If the tick count is the same as the returned value or if this is the
    // first sample, treat this sample as a duplicate.
    result.isSampleNew =
        (mLastResult != 0 && mLastResult != raplEventResult.value() &&
         result.count >= 0);
    mLastResult = raplEventResult.value();

    return result;
  }

  bool ValidPerfEventFd() { return mPerfEventFd >= 0; }

 private:
  mozilla::Maybe<uint64_t> ReadEventFd() {
    MOZ_ASSERT(ValidPerfEventFd());

    uint64_t eventResult;
    ssize_t readBytes = read(mPerfEventFd, &eventResult, sizeof(uint64_t));
    if (readBytes != sizeof(uint64_t)) {
      RAPL_LOG("Invalid RAPL event read size: %ld", readBytes);
      return mozilla::Nothing();
    }

    return mozilla::Some(eventResult);
  }

  uint64_t mLastResult;
  int mPerfEventFd;
};

static int GetRaplPerfEventType() {
  FILE* fp = fopen(SYSFS_PERF_POWER_TYPE_PATH, "r");
  if (!fp) {
    RAPL_LOG("Open of " SYSFS_PERF_POWER_TYPE_PATH " failed");
    return -1;
  }

  int readTypeValue = -1;
  if (fscanf(fp, "%d", &readTypeValue) != 1) {
    RAPL_LOG("Read of " SYSFS_PERF_POWER_TYPE_PATH " failed");
  }
  fclose(fp);

  return readTypeValue;
}

PowerCounters::PowerCounters() {
  if (!XRE_IsParentProcess()) {
    // Energy meters are global, so only sample them on the parent.
    return;
  }

  // Get the value perf_event_attr.type should be set to for RAPL
  // perf events.
  int perfEventType = GetRaplPerfEventType();
  if (perfEventType < 0) {
    RAPL_LOG("Failed to find the event type for RAPL perf events.");
    return;
  }

  for (const auto& raplEventDomain : kSupportedRaplDomains) {
    RaplProfilerCount* raplEvent = new RaplProfilerCount(
        perfEventType, raplEventDomain.mRaplEventType, raplEventDomain.mLabel,
        raplEventDomain.mDescription);
    if (!raplEvent->ValidPerfEventFd() || !mCounters.emplaceBack(raplEvent)) {
      delete raplEvent;
    }
  }
}

PowerCounters::~PowerCounters() {
  for (auto* raplEvent : mCounters) {
    delete raplEvent;
  }
  mCounters.clear();
}

void PowerCounters::Sample() {}
