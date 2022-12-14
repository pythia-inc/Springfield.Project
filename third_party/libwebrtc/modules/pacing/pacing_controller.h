/*
 *  Copyright (c) 2019 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#ifndef MODULES_PACING_PACING_CONTROLLER_H_
#define MODULES_PACING_PACING_CONTROLLER_H_

#include <stddef.h>
#include <stdint.h>

#include <atomic>
#include <memory>
#include <vector>

#include "absl/types/optional.h"
#include "api/field_trials_view.h"
#include "api/function_view.h"
#include "api/transport/field_trial_based_config.h"
#include "api/transport/network_types.h"
#include "modules/pacing/bitrate_prober.h"
#include "modules/pacing/interval_budget.h"
#include "modules/pacing/rtp_packet_pacer.h"
#include "modules/rtp_rtcp/include/rtp_packet_sender.h"
#include "modules/rtp_rtcp/include/rtp_rtcp_defines.h"
#include "modules/rtp_rtcp/source/rtp_packet_to_send.h"
#include "rtc_base/experiments/field_trial_parser.h"
#include "rtc_base/thread_annotations.h"

namespace webrtc {

// This class implements a leaky-bucket packet pacing algorithm. It handles the
// logic of determining which packets to send when, but the actual timing of
// the processing is done externally (e.g. RtpPacketPacer). Furthermore, the
// forwarding of packets when they are ready to be sent is also handled
// externally, via the PacingController::PacketSender interface.
class PacingController {
 public:
  // Periodic mode uses the IntervalBudget class for tracking bitrate
  // budgets, and expected ProcessPackets() to be called a fixed rate,
  // e.g. every 5ms as implemented by PacedSender.
  // Dynamic mode allows for arbitrary time delta between calls to
  // ProcessPackets.
  enum class ProcessMode { kPeriodic, kDynamic };

  class PacketSender {
   public:
    virtual ~PacketSender() = default;
    virtual void SendPacket(std::unique_ptr<RtpPacketToSend> packet,
                            const PacedPacketInfo& cluster_info) = 0;
    // Should be called after each call to SendPacket().
    virtual std::vector<std::unique_ptr<RtpPacketToSend>> FetchFec() = 0;
    virtual std::vector<std::unique_ptr<RtpPacketToSend>> GeneratePadding(
        DataSize size) = 0;
  };

  // Interface for class hanlding storage of and prioritization of packets
  // pending to be sent by the pacer.
  // Note that for the methods taking a Timestamp as parameter, the parameter
  // will never decrease between two subsequent calls.
  class PacketQueue {
   public:
    virtual ~PacketQueue() = default;

    virtual void Push(Timestamp enqueue_time,
                      std::unique_ptr<RtpPacketToSend> packet) = 0;
    virtual std::unique_ptr<RtpPacketToSend> Pop() = 0;

    virtual int SizeInPackets() const = 0;
    bool Empty() const { return SizeInPackets() == 0; }
    virtual DataSize SizeInPayloadBytes() const = 0;

    // If the next packet, that would be returned by Pop() if called
    // now, is an audio packet this method returns the enqueue time
    // of that packet. If queue is empty or top packet is not audio,
    // returns Timestamp::MinusInfinity().
    virtual Timestamp LeadingAudioPacketEnqueueTime() const = 0;

    // Enqueue time of the oldest packet in the queue,
    // Timestamp::MinusInfinity() if queue is empty.
    virtual Timestamp OldestEnqueueTime() const = 0;

    // Average queue time for the packets currently in the queue.
    // The queuing time is calculated from Push() to the last UpdateQueueTime()
    // call - with any time spent in a paused state subtracted.
    // Returns TimeDelta::Zero() for an empty queue.
    virtual TimeDelta AverageQueueTime() const = 0;

    // Called during packet processing or when pause stats changes. Since the
    // AverageQueueTime() method does not look at the wall time, this method
    // needs to be called before querying queue time.
    virtual void UpdateAverageQueueTime(Timestamp now) = 0;

    // Set the pause state, while `paused` is true queuing time is not counted.
    virtual void SetPauseState(bool paused, Timestamp now) = 0;
  };

  // Expected max pacer delay. If ExpectedQueueTime() is higher than
  // this value, the packet producers should wait (eg drop frames rather than
  // encoding them). Bitrate sent may temporarily exceed target set by
  // UpdateBitrate() so that this limit will be upheld.
  static const TimeDelta kMaxExpectedQueueLength;
  // Pacing-rate relative to our target send rate.
  // Multiplicative factor that is applied to the target bitrate to calculate
  // the number of bytes that can be transmitted per interval.
  // Increasing this factor will result in lower delays in cases of bitrate
  // overshoots from the encoder.
  static const float kDefaultPaceMultiplier;
  // If no media or paused, wake up at least every `kPausedProcessIntervalMs` in
  // order to send a keep-alive packet so we don't get stuck in a bad state due
  // to lack of feedback.
  static const TimeDelta kPausedProcessInterval;

  static const TimeDelta kMinSleepTime;

  // Allow probes to be processed slightly ahead of inteded send time. Currently
  // set to 1ms as this is intended to allow times be rounded down to the
  // nearest millisecond.
  static const TimeDelta kMaxEarlyProbeProcessing;

  PacingController(Clock* clock,
                   PacketSender* packet_sender,
                   const FieldTrialsView& field_trials,
                   ProcessMode mode);

  ~PacingController();

  // Adds the packet to the queue and calls PacketRouter::SendPacket() when
  // it's time to send.
  void EnqueuePacket(std::unique_ptr<RtpPacketToSend> packet);

  void CreateProbeCluster(DataRate bitrate, int cluster_id);

  void Pause();   // Temporarily pause all sending.
  void Resume();  // Resume sending packets.
  bool IsPaused() const;

  void SetCongested(bool congested);

  // Sets the pacing rates. Must be called once before packets can be sent.
  void SetPacingRates(DataRate pacing_rate, DataRate padding_rate);
  DataRate pacing_rate() const { return pacing_bitrate_; }

  // Currently audio traffic is not accounted by pacer and passed through.
  // With the introduction of audio BWE audio traffic will be accounted for
  // the pacer budget calculation. The audio traffic still will be injected
  // at high priority.
  void SetAccountForAudioPackets(bool account_for_audio);
  void SetIncludeOverhead();

  void SetTransportOverhead(DataSize overhead_per_packet);

  // Returns the time when the oldest packet was queued.
  Timestamp OldestPacketEnqueueTime() const;

  // Number of packets in the pacer queue.
  size_t QueueSizePackets() const;
  // Totals size of packets in the pacer queue.
  DataSize QueueSizeData() const;

  // Current buffer level, i.e. max of media and padding debt.
  DataSize CurrentBufferLevel() const;

  // Returns the time when the first packet was sent.
  absl::optional<Timestamp> FirstSentPacketTime() const;

  // Returns the number of milliseconds it will take to send the current
  // packets in the queue, given the current size and bitrate, ignoring prio.
  TimeDelta ExpectedQueueTime() const;

  void SetQueueTimeLimit(TimeDelta limit);

  // Enable bitrate probing. Enabled by default, mostly here to simplify
  // testing. Must be called before any packets are being sent to have an
  // effect.
  void SetProbingEnabled(bool enabled);

  // Returns the next time we expect ProcessPackets() to be called.
  Timestamp NextSendTime() const;

  // Check queue of pending packets and send them or padding packets, if budget
  // is available.
  void ProcessPackets();

  bool IsProbing() const;

 private:
  TimeDelta UpdateTimeAndGetElapsed(Timestamp now);
  bool ShouldSendKeepalive(Timestamp now) const;

  // Updates the number of bytes that can be sent for the next time interval.
  void UpdateBudgetWithElapsedTime(TimeDelta delta);
  void UpdateBudgetWithSentData(DataSize size);
  void UpdatePaddingBudgetWithSentData(DataSize size);

  DataSize PaddingToAdd(DataSize recommended_probe_size,
                        DataSize data_sent) const;

  std::unique_ptr<RtpPacketToSend> GetPendingPacket(
      const PacedPacketInfo& pacing_info,
      Timestamp target_send_time,
      Timestamp now);
  void OnPacketSent(RtpPacketMediaType packet_type,
                    DataSize packet_size,
                    Timestamp send_time);

  Timestamp CurrentTime() const;

  const ProcessMode mode_;
  Clock* const clock_;
  PacketSender* const packet_sender_;
  const FieldTrialsView& field_trials_;

  const bool drain_large_queues_;
  const bool send_padding_if_silent_;
  const bool pace_audio_;
  const bool ignore_transport_overhead_;
  // In dynamic mode, indicates the target size when requesting padding,
  // expressed as a duration in order to adjust for varying padding rate.
  const TimeDelta padding_target_duration_;

  TimeDelta min_packet_limit_;

  DataSize transport_overhead_per_packet_;

  // TODO(webrtc:9716): Remove this when we are certain clocks are monotonic.
  // The last millisecond timestamp returned by `clock_`.
  mutable Timestamp last_timestamp_;
  bool paused_;

  // In periodic mode, `media_budget_` and `padding_budget_` will be used to
  // track when packets can be sent.
  // In dynamic mode, `media_debt_` and `padding_debt_` will be used together
  // with the target rates.

  // This is the media budget, keeping track of how many bits of media
  // we can pace out during the current interval.
  IntervalBudget media_budget_;
  // This is the padding budget, keeping track of how many bits of padding we're
  // allowed to send out during the current interval. This budget will be
  // utilized when there's no media to send.
  IntervalBudget padding_budget_;

  DataSize media_debt_;
  DataSize padding_debt_;
  DataRate media_rate_;
  DataRate padding_rate_;

  BitrateProber prober_;
  bool probing_send_failure_;

  DataRate pacing_bitrate_;

  Timestamp last_process_time_;
  Timestamp last_send_time_;
  absl::optional<Timestamp> first_sent_packet_time_;
  bool seen_first_packet_;

  std::unique_ptr<PacketQueue> packet_queue_;

  bool congested_;

  TimeDelta queue_time_limit_;
  bool account_for_audio_;
  bool include_overhead_;
};
}  // namespace webrtc

#endif  // MODULES_PACING_PACING_CONTROLLER_H_
