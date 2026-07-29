#include <opendaq/multi_reader/synchronization_manager.h>

#include <coretypes/ratio_factory.h>
#include <opendaq/custom_log.h>

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <numeric>

BEGIN_NAMESPACE_OPENDAQ

namespace multi_reader
{

namespace
{

std::string joinIndices(const std::vector<SizeT>& indices)
{
    std::string result;
    for (const auto index : indices)
    {
        if (!result.empty())
            result += ", ";
        result += std::to_string(index);
    }
    return result;
}

// Non-negative remainder (the C++ % operator can return negative values for negative operands)
std::int64_t floorMod(std::int64_t value, std::int64_t modulus)
{
    return ((value % modulus) + modulus) % modulus;
}

// All QueueReaders of one reader share a single integral domain read type, so common-domain
// values are one of these instantiations; nullopt for anything else (falls back to exact match).
std::optional<std::int64_t> domainTickOf(const DomainValue& value)
{
    if (const auto* typed = dynamic_cast<const DomainValueImpl<std::int64_t>*>(&value))
        return typed->getValue();
    if (const auto* typed = dynamic_cast<const DomainValueImpl<std::uint64_t>*>(&value))
        return static_cast<std::int64_t>(typed->getValue());
    if (const auto* typed = dynamic_cast<const DomainValueImpl<std::int32_t>*>(&value))
        return typed->getValue();
    if (const auto* typed = dynamic_cast<const DomainValueImpl<std::uint32_t>*>(&value))
        return typed->getValue();
    return std::nullopt;
}

}  // namespace

SynchronizationManager::SynchronizationManager(const LoggerComponentPtr& logger)
    : loggerComponent(logger)
{
}

void SynchronizationManager::setRequiredCommonSampleRate(std::int64_t rate)
{
    requiredCommonSampleRate = rate;
}

void SynchronizationManager::setAllowDifferentRates(bool allow)
{
    allowDifferentRates = allow;
}

void SynchronizationManager::setStartOnFullUnitOfDomain(bool enabled)
{
    startOnFullUnitOfDomain = enabled;
}

void SynchronizationManager::setMaxSynchronizationDistance(std::chrono::system_clock::duration distance)
{
    maxSynchronizationDistance = distance;
}

std::optional<std::int64_t> SynchronizationManager::checkedMultiply(std::int64_t a, std::int64_t b)
{
    if (a <= 0 || b <= 0)
        return std::nullopt;
    if (a > std::numeric_limits<std::int64_t>::max() / b)
        return std::nullopt;
    return a * b;
}

std::optional<std::int64_t> SynchronizationManager::checkedLcm(std::int64_t a, std::int64_t b)
{
    if (a <= 0 || b <= 0)
        return std::nullopt;
    const std::int64_t gcd = std::gcd(a, b);
    return checkedMultiply(a / gcd, b);
}

std::optional<TickResolution> SynchronizationManager::rationalGcd(const std::vector<TickResolution>& ratios)
{
    if (ratios.empty())
        return std::nullopt;

    // gcd identity element: gcd(0, x) = x, lcm(1, x) = x
    std::int64_t numerator = 0;
    std::int64_t denominator = 1;

    for (const auto& ratio : ratios)
    {
        std::int64_t num = ratio.num;
        std::int64_t den = ratio.den;
        if (num <= 0 || den <= 0)
            return std::nullopt;

        // The gcd(numerators) / lcm(denominators) rule requires reduced fractions
        const std::int64_t gcd = std::gcd(num, den);
        num /= gcd;
        den /= gcd;

        numerator = std::gcd(numerator, num);
        const auto lcm = checkedLcm(denominator, den);
        if (!lcm)
            return std::nullopt;
        denominator = *lcm;
    }

    return TickResolution{numerator, denominator};
}

SyncSetupResult SynchronizationManager::buildCommonModel(const std::vector<QueueReader*>& inputs,
                                                         const std::vector<SizeT>& slotIndices,
                                                         SizeT mainPosition)
{
    auto result = buildCommonModelImpl(inputs, slotIndices, mainPosition);
    if (!result.ok())
    {
        LOG_D("{}", result.message);
    }
    return result;
}

SyncSetupResult SynchronizationManager::buildCommonModelImpl(const std::vector<QueueReader*>& inputs,
                                                             const std::vector<SizeT>& slotIndices,
                                                             SizeT mainPosition)
{
    modelValid = false;
    model = CommonModel{};

    const SizeT count = inputs.size();
    if (count == 0 || count != slotIndices.size() || mainPosition >= count)
        return {SyncSetupIssue::MissingDomainDescriptor, {}, "No used inputs to build a common model from"};

    // Domain descriptors and resolutions must be present
    std::vector<SizeT> missing;
    for (SizeT i = 0; i < count; ++i)
    {
        // resolution {0, 0} is TickResolution's unassigned sentinel
        if (!inputs[i]->getDomainDescriptor().assigned() || inputs[i]->getDomainInfo().resolution.den == 0)
            missing.push_back(slotIndices[i]);
    }
    if (!missing.empty())
        return {SyncSetupIssue::MissingDomainDescriptor, missing, "Inputs [" + joinIndices(missing) + "] have no usable domain descriptor"};

    // Sample rates
    std::vector<std::int64_t> rates(count);
    std::vector<SizeT> invalidRates;
    for (SizeT i = 0; i < count; ++i)
    {
        rates[i] = inputs[i]->getSampleRate();
        if (rates[i] <= 0)
            invalidRates.push_back(slotIndices[i]);
    }
    if (!invalidRates.empty())
        return {SyncSetupIssue::InvalidSampleRate, invalidRates, "Inputs [" + joinIndices(invalidRates) + "] have no valid integer sample rate"};

    if (!allowDifferentRates)
    {
        std::vector<SizeT> different;
        for (SizeT i = 0; i < count; ++i)
        {
            if (rates[i] != rates[0])
                different.push_back(slotIndices[i]);
        }
        if (!different.empty())
        {
            return {SyncSetupIssue::RatesNotEqual,
                    different,
                    "Different sample rates are not allowed; inputs [" + joinIndices(different) + "] differ from the first used input"};
        }
    }

    std::int64_t commonRate = -1;
    if (requiredCommonSampleRate > 0)
    {
        std::vector<SizeT> notDividing;
        for (SizeT i = 0; i < count; ++i)
        {
            if (requiredCommonSampleRate % rates[i] != 0)
                notDividing.push_back(slotIndices[i]);
        }
        if (!notDividing.empty())
        {
            return {SyncSetupIssue::RequiredRateNotDivisible,
                    notDividing,
                    "Sample rates of inputs [" + joinIndices(notDividing) + "] do not divide the required common sample rate " +
                        std::to_string(requiredCommonSampleRate)};
        }
        commonRate = requiredCommonSampleRate;
    }
    else
    {
        commonRate = rates[0];
        for (SizeT i = 1; i < count; ++i)
        {
            const auto lcm = checkedLcm(commonRate, rates[i]);
            if (!lcm)
                return {SyncSetupIssue::ArithmeticOverflow, {slotIndices[i]}, "Common sample rate computation overflowed"};
            commonRate = *lcm;
        }
    }

    // Dividers and the minimum aligned block
    std::vector<SizeT> dividers(count);
    std::int64_t blockLcm = 1;
    for (SizeT i = 0; i < count; ++i)
    {
        const std::int64_t divider = commonRate / rates[i];
        dividers[i] = static_cast<SizeT>(divider);

        const auto lcm = checkedLcm(blockLcm, divider);
        if (!lcm)
            return {SyncSetupIssue::ArithmeticOverflow, {slotIndices[i]}, "Aligned block size computation overflowed"};
        blockLcm = *lcm;
    }

    // Common domain: earliest epoch; rational GCD of all resolutions plus the output sample
    // period, so one output sample is always a whole number of common ticks
    auto commonEpoch = inputs[0]->getDomainInfo().epoch;
    std::vector<TickResolution> resolutions;
    resolutions.reserve(count + 1);
    for (SizeT i = 0; i < count; ++i)
    {
        const auto& domainInfo = inputs[i]->getDomainInfo();
        commonEpoch = std::min(commonEpoch, domainInfo.epoch);
        resolutions.push_back(domainInfo.resolution);
    }
    resolutions.push_back(TickResolution{1, commonRate});

    const auto commonResolution = rationalGcd(resolutions);
    if (!commonResolution)
        return {SyncSetupIssue::ArithmeticOverflow, {}, "Common resolution computation overflowed"};

    model.commonDomain = DomainInfo{commonEpoch, *commonResolution};
    model.commonSampleRate = commonRate;
    model.sampleRateDividers = dividers;
    model.blockLcm = static_cast<SizeT>(blockLcm);
    model.mainPosition = mainPosition;

    for (SizeT i = 0; i < count; ++i)
        inputs[i]->setSampleRateDivider(dividers[i]);

    modelValid = true;
    return {};
}

bool SynchronizationManager::hasModel() const
{
    return modelValid;
}

const CommonModel& SynchronizationManager::getModel() const
{
    return model;
}


std::optional<SyncResult> SynchronizationManager::collectFirstSamples(const std::vector<QueueReader*>& inputs,
                                                                      const std::vector<SizeT>& slotIndices,
                                                                      std::vector<std::unique_ptr<DomainValue>>& firstSamples) const
{
    for (SizeT i = 0; i < inputs.size(); ++i)
    {
        auto first = inputs[i]->getFirstSampleDomainValue();
        if (!first)
            return SyncResult{SyncOutcome::NeedMoreData, SyncFailureReason::None, {slotIndices[i]}, "Input has no unread samples"};
        // The common resolution divides every input resolution, so this conversion is exact
        firstSamples[i] = first->toDomain(model.commonDomain);
    }
    return std::nullopt;
}

std::optional<SyncResult> SynchronizationManager::checkSynchronizationDistance(
    const std::vector<std::unique_ptr<DomainValue>>& firstSamples, const std::vector<SizeT>& slotIndices) const
{
    if (maxSynchronizationDistance.count() <= 0)
        return std::nullopt;

    const SizeT count = firstSamples.size();
    std::vector<std::chrono::system_clock::time_point> absoluteTimes(count);
    auto latest = std::chrono::system_clock::time_point::min();
    for (SizeT i = 0; i < count; ++i)
    {
        absoluteTimes[i] = firstSamples[i]->toAbsoluteTime();
        latest = std::max(latest, absoluteTimes[i]);
    }

    std::vector<SizeT> tooFar;
    for (SizeT i = 0; i < count; ++i)
    {
        if (latest - absoluteTimes[i] > maxSynchronizationDistance)
            tooFar.push_back(slotIndices[i]);
    }
    if (tooFar.empty())
        return std::nullopt;

    return SyncResult{SyncOutcome::Failed,
                      SyncFailureReason::SyncDistanceExceeded,
                      tooFar,
                      "Inputs [" + joinIndices(tooFar) + "] start farther from the latest first sample than the configured "
                      "maximum synchronization distance"};
}

SynchronizationManager::CandidatePick SynchronizationManager::pickStartCandidate(
    const std::vector<std::unique_ptr<DomainValue>>& firstSamples, const std::vector<SizeT>& slotIndices) const
{
    const SizeT count = firstSamples.size();

    // The start cannot be earlier than the input whose data begins last
    SizeT latestIndex = 0;
    for (SizeT i = 1; i < count; ++i)
    {
        if (*firstSamples[i] > *firstSamples[latestIndex])
            latestIndex = i;
    }

    // The tick-arithmetic path needs an integral tick value for every first sample and a
    // usable grid model; it is skipped when a start on a full domain unit was requested
    const auto blockTicks = blockIntervalTicks();
    std::vector<std::int64_t> firstTicks(count);
    bool ticksKnown = blockTicks > 0 && !startOnFullUnitOfDomain && model.mainPosition < count &&
                      model.sampleRateDividers.size() == count && model.ticksPerCommonSample() > 0;
    if (ticksKnown)
    {
        for (SizeT i = 0; i < count; ++i)
        {
            const auto tick = domainTickOf(*firstSamples[i]);
            if (!tick.has_value())
            {
                ticksKnown = false;
                break;
            }
            firstTicks[i] = *tick;
        }
    }

    // An independent copy, not the element itself: the candidate is mutated below
    // (roundUpOnDomainInterval / shiftTicks) and is destroyed outright on the failure paths, so
    // owning it separately keeps firstSamples valid for the caller and removes the ordering
    // hazard that a stolen element would impose on anything added after this point. The elements
    // are already in the common domain (collectFirstSamples converted them), so this conversion
    // is the identity and costs one small allocation on a path that runs once per sync round.
    auto candidate = firstSamples[latestIndex]->toDomain(model.commonDomain);
    if (!ticksKnown)
    {
        // Fallback: tick values unavailable (unusual domain read type) or a full-unit start
        // was requested - round the latest first sample up onto the absolute start grid
        try
        {
            candidate->roundUpOnDomainInterval(startInterval());
        }
        catch (const NotSupportedException& e)
        {
            return {nullptr, SyncResult{SyncOutcome::Failed, SyncFailureReason::TargetNotRepresentable, {}, e.what()}};
        }
        return {std::move(candidate), std::nullopt};
    }

    // Each input can only deliver samples at a regular tick spacing: one sample every
    // (divider * ticksPerCommonSample) ticks, shifted by where its data actually starts.
    // The output grid follows the MAIN input, so the search walks the
    // main input's grid, beginning at the first grid point not before the latest input,
    // and inspects one aligned block's worth of grid points - the phase pattern repeats
    // after one block, so looking further cannot find anything new. Best case: a grid
    // point every input hits exactly. Fallback: the first grid point where every input's
    // next sample arrives strictly less than half a block later, so each sample still
    // unambiguously belongs to that block. Neither within one block:
    // the inputs share no common tick.
    const auto tickPeriodOf = [this](SizeT i)
    { return static_cast<std::int64_t>(model.sampleRateDividers[i]) * model.ticksPerCommonSample(); };

    const auto latestTick = firstTicks[latestIndex];
    const auto mainPeriod = tickPeriodOf(model.mainPosition);
    const auto mainPhase = floorMod(firstTicks[model.mainPosition], mainPeriod);

    // First main-grid point at or after the latest input's first sample
    const auto gridStart = latestTick + floorMod(mainPhase - latestTick, mainPeriod);

    // The step cap only guards pathological divider combinations (huge blockLcm)
    constexpr std::int64_t maxSearchSteps = 1024;
    std::optional<std::int64_t> exactTick;
    std::optional<std::int64_t> tolerableTick;
    std::vector<SizeT> misaligned;
    for (std::int64_t candidateTick = gridStart, steps = 0;
         candidateTick < gridStart + blockTicks && steps < maxSearchSteps;
         candidateTick += mainPeriod, ++steps)
    {
        bool everyInputExact = true;
        bool everyInputAttributable = true;
        misaligned.clear();
        for (SizeT i = 0; i < count; ++i)
        {
            // How many ticks after the candidate does input i's next sample fall?
            const auto offset = floorMod(firstTicks[i] - candidateTick, tickPeriodOf(i));
            if (offset == 0)
                continue;
            everyInputExact = false;
            if (2 * offset >= blockTicks)
            {
                everyInputAttributable = false;
                misaligned.push_back(slotIndices[i]);
            }
        }
        if (everyInputExact)
        {
            exactTick = candidateTick;
            break;
        }
        if (everyInputAttributable && !tolerableTick)
            tolerableTick = candidateTick;
    }

    if (!exactTick && !tolerableTick)
    {
        const auto& blamed = misaligned.empty() ? slotIndices : misaligned;
        return {nullptr,
                SyncResult{SyncOutcome::Failed,
                           SyncFailureReason::NoCommonTick,
                           blamed,
                           "Inputs [" + joinIndices(blamed) + "] share no common tick with the main input's aligned start grid"}};
    }

    candidate->shiftTicks((exactTick ? *exactTick : *tolerableTick) - latestTick);
    return {std::move(candidate), std::nullopt};
}

SynchronizationManager::AdvanceOutcomes SynchronizationManager::advanceAllInputs(const std::vector<QueueReader*>& inputs,
                                                                                 const std::vector<SizeT>& slotIndices,
                                                                                 DomainValue& candidate) const
{
    AdvanceOutcomes outcomes;
    outcomes.reached.resize(inputs.size());

    for (SizeT i = 0; i < inputs.size(); ++i)
    {
        const auto target = candidate.fromDomain(inputs[i]->getDomainInfo());
        auto advance = inputs[i]->advanceToDomainValue(target.get());
        switch (advance.result)
        {
            case AdvanceResult::Success:
                outcomes.reached[i] = advance.reachedValue->toDomain(model.commonDomain);
                break;
            case AdvanceResult::NeedMoreData:
                outcomes.needMoreDataInputs.push_back(slotIndices[i]);
                break;
            case AdvanceResult::DomainChanged:
            case AdvanceResult::Error:  // pending events block advancing
                outcomes.pendingEventInputs.push_back(slotIndices[i]);
                break;
            case AdvanceResult::OvershotError:
                outcomes.overshoot = true;
                break;
        }
    }
    return outcomes;
}

SyncResult SynchronizationManager::synchronize(const std::vector<QueueReader*>& inputs, const std::vector<SizeT>& slotIndices)
{
    if (!modelValid || inputs.empty() || inputs.size() != slotIndices.size())
    {
        pendingCandidate = nullptr;
        return {SyncOutcome::Failed, SyncFailureReason::None, {}, "Synchronization requires a valid common model"};
    }

    // Clears the ESTABLISHED start only - not the latched target (pendingCandidate), which
    // is what makes this function stateful across calls.
    model.commonStart = nullptr;

    // Alignment is a retry loop: each round inspects every input's first unread sample,
    // picks the tick all inputs should start on and tries to move every input onto it.
    // A round repeats only when the data moved underneath the plan (an advance overshot
    // the candidate, or samples were sparser than the grid predicted); the bound caps
    // pathological inputs.
    constexpr int maxIterations = 8;
    for (int iteration = 0; iteration < maxIterations; ++iteration)
    {
        // 1. Where does each input's unread data start, in the common domain?
        //    An input with nothing unread is the soft case: return without touching the latched
        //    target, so the decision already made survives until the data arrives.
        std::vector<std::unique_ptr<DomainValue>> firstSamples(inputs.size());
        if (auto waiting = collectFirstSamples(inputs, slotIndices, firstSamples))
            return *waiting;

        // 2. Do all inputs start close enough together to be synchronized at all?
        if (auto tooFar = checkSynchronizationDistance(firstSamples, slotIndices))
        {
            pendingCandidate = nullptr;
            return *tooFar;
        }

        // 3. The start tick: the one already decided on if there is one, otherwise chosen on the
        //    main input's grid. Taking the latch by move means every exit below except the
        //    NeedMoreData one leaves it cleared, which is exactly the intended lifetime - only
        //    "the target is right, the data has not arrived" re-latches it.
        auto candidate = std::move(pendingCandidate);
        if (!candidate)
        {
            auto pick = pickStartCandidate(firstSamples, slotIndices);
            if (pick.failure)
                return *pick.failure;
            candidate = std::move(pick.value);
        }

        // 4. Move every input's cursor forward onto the candidate.
        auto advanced = advanceAllInputs(inputs, slotIndices, *candidate);
        if (!advanced.pendingEventInputs.empty())
        {
            return {SyncOutcome::EventPending, SyncFailureReason::None, advanced.pendingEventInputs,
                    "Events must be handled before synchronization"};
        }
        if (!advanced.needMoreDataInputs.empty())
        {
            // Latch the target: the inputs that did reach it stay where they are, and the ones that
            // did not are waited for against THIS tick rather than a freshly derived, later one.
            pendingCandidate = std::move(candidate);
            return {SyncOutcome::NeedMoreData, SyncFailureReason::None, advanced.needMoreDataInputs,
                    "Waiting for data to reach the aligned start"};
        }
        if (advanced.overshoot)
            continue;  // a first sample moved past the candidate - retry with fresh first samples

        // 5. Synchronized only when every reached value is acceptably close to the candidate
        //    (the acceptance rule is documented on reachedAcceptable). Otherwise the data was
        //    sparser than the grid predicted (a jump without a gap event); the cursors already
        //    moved to the actually reached samples, so retry with fresh first samples.
        bool allReachedCandidate = true;
        for (SizeT i = 0; i < inputs.size() && allReachedCandidate; ++i)
            allReachedCandidate = reachedAcceptable(*advanced.reached[i], *candidate);

        if (allReachedCandidate)
        {
            model.commonStart = std::move(candidate);
            return {SyncOutcome::Synchronized, SyncFailureReason::None, {}, {}};
        }
    }

    return {SyncOutcome::Failed,
            SyncFailureReason::NoCommonTick,
            slotIndices,
            "Inputs share no common tick on the aligned start grid (iteration bound exceeded)"};
}

void SynchronizationManager::clearSynchronization()
{
    model.commonStart = nullptr;
    // Every hard event (disconnect, used-set change, pending event, data loss, config change)
    // reaches us through here, and each of them can move the data or the grid the latched target
    // was computed on - so the target goes with the established start. Only the soft case, an
    // input that has not yet received the packets to reach the target, keeps it (see
    // SynchronizationManager::pendingCandidate).
    pendingCandidate = nullptr;
}

void SynchronizationManager::invalidateModel()
{
    model = CommonModel{};
    modelValid = false;
    // The latched target was computed on the grid this model defines; a rebuild can move the grid.
    pendingCandidate = nullptr;
}

const DomainValue* SynchronizationManager::getCommonStart() const
{
    return model.commonStart.get();
}

RatioPtr SynchronizationManager::startInterval() const
{
    if (startOnFullUnitOfDomain)
        return Ratio(1, 1);

    // Direct path: the minimum aligned block. The resampled path uses the
    // output sample period instead.
    return Ratio(static_cast<Int>(model.blockLcm), static_cast<Int>(model.commonSampleRate));
}

std::int64_t SynchronizationManager::blockIntervalTicks() const
{
    const auto ticksPerSample = model.ticksPerCommonSample();
    if (ticksPerSample <= 0 ||
        static_cast<std::int64_t>(model.blockLcm) > std::numeric_limits<std::int64_t>::max() / ticksPerSample)
        return 0;  // callers fall back to the non-tick path
    return static_cast<std::int64_t>(model.blockLcm) * ticksPerSample;
}

bool SynchronizationManager::reachedAcceptable(const DomainValue& reached, const DomainValue& candidate) const
{
    const auto reachedTick = domainTickOf(reached);
    const auto candidateTick = domainTickOf(candidate);
    if (!reachedTick.has_value() || !candidateTick.has_value())
        return reached == candidate;

    const std::int64_t distance = std::abs(*reachedTick - *candidateTick);
    if (distance == 0)
        return true;
    return 2 * distance < blockIntervalTicks();
}

}  // namespace multi_reader

END_NAMESPACE_OPENDAQ
