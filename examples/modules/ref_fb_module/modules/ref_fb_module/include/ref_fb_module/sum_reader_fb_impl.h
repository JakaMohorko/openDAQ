/*
 * Copyright 2022-2026 openDAQ d.o.o.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once
#include <ref_fb_module/common.h>
#include <opendaq/function_block_ptr.h>
#include <opendaq/function_block_type_factory.h>
#include <opendaq/function_block_impl.h>
#include <opendaq/signal_config_ptr.h>
#include <opendaq/data_packet_ptr.h>
#include <opendaq/multi_reader_ptr.h>

#include <atomic>
#include <chrono>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

BEGIN_NAMESPACE_REF_FB_MODULE

namespace SumReader
{

/*!
 * Reference consumer of the reworked multi reader. Sums any number of input signals into one
 * output signal, driven entirely by the reader's public status surface:
 *
 * - Two modes: `EqualRates` (all inputs must share one rate; one output sample per input
 *   sample) and `MultiRate` (inputs may differ in rate; one output sample per aligned block,
 *   i.e. the output runs at the GCD of the input rates and sums the coinciding-tick samples).
 * - Recoverable per-input failures (`Incompatible`, `SynchronizationFailed`, `DataLost`) park
 *   the affected ports (`setInputUsed(false)`) so the remaining inputs keep summing; parked
 *   ports are probed back automatically - immediately when the status reports an event on a
 *   parked input (unused-input events fire onDataAvailable, review Q5), and periodically
 *   every `RecoveryRetryInterval` seconds.
 * - `IgnoreFaultyInputs` (default true) selects the parking behavior; when false, failing
 *   inputs are never excluded - the FB reports them and waits for every input to work.
 * - Unrecoverable reader failure (`Fail`) is reported as `ComponentStatus::Error` and stops
 *   reads; there is no silent reader re-creation.
 */
class SumReaderFbImpl final : public FunctionBlock
{
public:
    explicit SumReaderFbImpl(const ContextPtr& ctx, const ComponentPtr& parent, const StringPtr& localId, const PropertyObjectPtr& config);
    ~SumReaderFbImpl() override = default;

    static FunctionBlockTypePtr CreateType();

private:
    enum class SumMode : Int
    {
        EqualRates = 0,
        MultiRate = 1
    };

    struct ParkedInfo
    {
        std::string reason;
        std::chrono::steady_clock::time_point since;
    };

    std::string getNextPortID() const;

    void initProperties();
    void createSignals();
    void createDisconnectedPort();
    bool updateInputPortsLocked();
    void createReaderLocked();
    void refreshReaderConfigLocked();
    void modeChanged();
    void readerConfigChanged();
    void ignoreFaultyInputsChanged();

    void onConnected(const InputPortPtr& inputPort) override;
    void onDisconnected(const InputPortPtr& inputPort) override;
    void onPacketReceived(const InputPortPtr& inputPort) override;
    void onDataReceived();
    void scheduleDeferredCheck();
    void deferredCheck();

    void processReaderLocked();
    void emitSumLocked(const std::vector<double*>& buffers, const std::vector<SizeT>& strides, SizeT commonCount, const MultiReaderStatusPtr& status);
    bool handleStateLocked(const MultiReaderStatusPtr& status);
    void handleEventsLocked(const MultiReaderStatusPtr& status);
    void configureValueDescriptorLocked();
    bool ensureRateModelLocked();

    void parkPortLocked(const InputPortPtr& port, const std::string& reason);
    void unparkLocked(const std::string& portId);
    void probePortLocked(const std::string& portId);
    void maybeProbeLocked();
    /// Q5 recovery path: probes a parked port whose per-input state reports Event.
    bool probeEventfulParkedLocked(const MultiReaderStatusPtr& status);
    std::string describeFailedInputsLocked(const MultiReaderStatusPtr& status) const;
    void updateComponentStatusLocked();

    bool isActivePortLocked(const std::string& portId) const;
    InputPortPtr findPortByIdLocked(const std::string& portId) const;

    std::vector<InputPortPtr> connectedPorts;
    InputPortPtr disconnectedPort;

    // Mirrors the reader's slot order (IReaderConfig::getInputPorts): read buffers are jagged
    // arrays ordered by slot, and status affected-input indices are slot indices
    std::vector<InputPortPtr> readerPorts;

    std::unordered_map<std::string, DataDescriptorPtr> cachedValueDescriptors;
    std::unordered_map<std::string, DataDescriptorPtr> cachedDomainDescriptors;
    DataDescriptorPtr commonDomainDescriptor;

    // std::map: parked-port warnings enumerate in a deterministic order
    std::map<std::string, ParkedInfo> parkedPorts;
    std::string probingPortId;
    std::atomic<bool> deferredCheckScheduled{false};
    std::chrono::steady_clock::time_point lastProbeTime{};
    std::chrono::steady_clock::time_point lastReaderCheck{};
    bool readerErrored = false;

    SumMode mode = SumMode::EqualRates;
    double dataLossTimeoutSeconds = 5.0;
    double maxSyncDistanceSeconds = 5.0;
    double recoveryRetryIntervalSeconds = 5.0;
    bool ignoreFaultyInputs = true;
    /// Non-empty while IgnoreFaultyInputs=false and inputs are failing (component status text)
    std::string failedInputsMessage;

    // Derived from public reader data (getCommonSampleRate + per-input domain descriptors):
    // per-slot sample-rate dividers and the aligned-block quantum they imply
    bool rateModelDirty = true;
    std::unordered_map<std::string, SizeT> portDividers;
    SizeT blockLcm = 1;

    DataDescriptorPtr sumDataDescriptor;
    DataDescriptorPtr sumDomainDataDescriptor;

    SignalConfigPtr sumSignal;
    SignalConfigPtr sumDomainSignal;

    PacketReadyNotification notificationMode;
    MultiReaderPtr reader;
};
}

END_NAMESPACE_REF_FB_MODULE
