#include <opendaq/synchronization_manager.h>

#include <coretypes/ratio_factory.h>
#include <opendaq/custom_log.h>

#include <algorithm>
#include <limits>
#include <numeric>
#include <set>

BEGIN_NAMESPACE_OPENDAQ

namespace
{

struct ReferenceDomainBin
{
    StringPtr id;
    TimeProtocol timeProtocol;

    bool operator<(const ReferenceDomainBin& rhs) const
    {
        if (id == rhs.id)
            return timeProtocol < rhs.timeProtocol;
        if (id.assigned() && rhs.id.assigned())
            return id < rhs.id;
        if (rhs.id.assigned())
            return true;
        return false;
    }
};

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

std::optional<RatioPtr> SynchronizationManager::rationalGcd(const std::vector<RatioPtr>& ratios)
{
    if (ratios.empty())
        return std::nullopt;

    // gcd identity element: gcd(0, x) = x, lcm(1, x) = x
    std::int64_t numerator = 0;
    std::int64_t denominator = 1;

    for (const auto& ratio : ratios)
    {
        if (!ratio.assigned())
            return std::nullopt;

        std::int64_t num = ratio.getNumerator();
        std::int64_t den = ratio.getDenominator();
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

    return Ratio(numerator, denominator);
}

SyncSetupResult SynchronizationManager::buildCommonModel(const std::vector<QueueReader*>& inputs,
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
        if (!inputs[i]->getDomainDescriptor().assigned() || !inputs[i]->getDomainInfo().resolution.assigned())
            missing.push_back(slotIndices[i]);
    }
    if (!missing.empty())
        return {SyncSetupIssue::MissingDomainDescriptor, missing, "Inputs [" + joinIndices(missing) + "] have no usable domain descriptor"};

    const auto referenceCheck = checkReferenceDomains(inputs, slotIndices);
    if (!referenceCheck.ok())
        return referenceCheck;

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
    std::vector<RatioPtr> resolutions;
    resolutions.reserve(count + 1);
    for (SizeT i = 0; i < count; ++i)
    {
        const auto& domainInfo = inputs[i]->getDomainInfo();
        commonEpoch = std::min(commonEpoch, domainInfo.epoch);
        resolutions.push_back(domainInfo.resolution);
    }
    resolutions.push_back(Ratio(1, commonRate));

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

SyncResult SynchronizationManager::synchronize(const std::vector<QueueReader*>& inputs, const std::vector<SizeT>& slotIndices)
{
    if (!modelValid || inputs.empty() || inputs.size() != slotIndices.size())
        return {SyncOutcome::Failed, SyncFailureReason::None, {}, "Synchronization requires a valid common model"};

    model.commonStart = nullptr;

    const SizeT count = inputs.size();
    constexpr int maxIterations = 8;
    int iteration = 0;

    while (true)  // re-entered only after an overshoot, bounded by the shared iteration counter
    {
        // Step 2: first samples, converted to the common domain (exact by construction)
        std::vector<std::unique_ptr<DomainValue>> firstsCommon(count);
        for (SizeT i = 0; i < count; ++i)
        {
            auto first = inputs[i]->getFirstSampleDomainValue();
            if (!first)
                return {SyncOutcome::NeedMoreData, SyncFailureReason::None, {slotIndices[i]}, "Input has no unread samples"};
            firstsCommon[i] = first->toCommonDomain(model.commonDomain);
        }

        // Step 3: synchronization distance
        if (maxSynchronizationDistance.count() > 0)
        {
            std::vector<std::chrono::system_clock::time_point> absoluteTimes(count);
            auto latest = std::chrono::system_clock::time_point::min();
            for (SizeT i = 0; i < count; ++i)
            {
                absoluteTimes[i] = firstsCommon[i]->toAbsoluteTime();
                latest = std::max(latest, absoluteTimes[i]);
            }

            std::vector<SizeT> tooFar;
            for (SizeT i = 0; i < count; ++i)
            {
                if (latest - absoluteTimes[i] > maxSynchronizationDistance)
                    tooFar.push_back(slotIndices[i]);
            }
            if (!tooFar.empty())
            {
                return {SyncOutcome::Failed,
                        SyncFailureReason::SyncDistanceExceeded,
                        tooFar,
                        "Inputs [" + joinIndices(tooFar) + "] start farther from the latest first sample than the configured "
                        "maximum synchronization distance"};
            }
        }

        // Step 4: candidate start = latest first sample rounded up on the start grid
        SizeT latestIndex = 0;
        for (SizeT i = 1; i < count; ++i)
        {
            if (*firstsCommon[i] > *firstsCommon[latestIndex])
                latestIndex = i;
        }
        auto candidate = std::move(firstsCommon[latestIndex]);
        try
        {
            candidate->roundUpOnDomainInterval(startInterval());
        }
        catch (const NotSupportedException& e)
        {
            return {SyncOutcome::Failed, SyncFailureReason::TargetNotRepresentable, {}, e.what()};
        }

        // Steps 5-6: advance every input to the candidate and verify the reached values
        bool overshoot = false;
        while (!overshoot)
        {
            if (++iteration > maxIterations)
            {
                return {SyncOutcome::Failed,
                        SyncFailureReason::NoCommonTick,
                        slotIndices,
                        "Inputs share no common tick on the aligned start grid (iteration bound exceeded)"};
            }

            std::vector<std::unique_ptr<DomainValue>> reached(count);
            std::vector<SizeT> eventInputs;
            std::vector<SizeT> needMoreInputs;

            for (SizeT i = 0; i < count; ++i)
            {
                const auto target = candidate->fromCommonDomain(inputs[i]->getDomainInfo());
                auto outcome = inputs[i]->advanceToDomainValue(target.get());
                switch (outcome.result)
                {
                    case AdvanceResult::Success:
                        reached[i] = outcome.reachedValue->toCommonDomain(model.commonDomain);
                        break;
                    case AdvanceResult::NeedMoreData:
                        needMoreInputs.push_back(slotIndices[i]);
                        break;
                    case AdvanceResult::DomainChanged:
                    case AdvanceResult::Error:  // pending events block advancing
                        eventInputs.push_back(slotIndices[i]);
                        break;
                    case AdvanceResult::OvershotError:
                        overshoot = true;
                        break;
                }
            }

            if (!eventInputs.empty())
                return {SyncOutcome::EventPending, SyncFailureReason::None, eventInputs, "Events must be handled before synchronization"};
            if (!needMoreInputs.empty())
                return {SyncOutcome::NeedMoreData, SyncFailureReason::None, needMoreInputs, "Waiting for data to reach the aligned start"};
            if (overshoot)
                break;  // first samples moved past the candidate - re-derive it from step 2

            bool allReachedCandidate = true;
            SizeT maxIndex = 0;
            for (SizeT i = 0; i < count; ++i)
            {
                if (*reached[i] != *candidate)
                    allReachedCandidate = false;
                if (*reached[i] > *reached[maxIndex])
                    maxIndex = i;
            }

            if (allReachedCandidate)
            {
                model.commonStart = std::move(candidate);
                return {SyncOutcome::Synchronized, SyncFailureReason::None, {}, {}};
            }

            // Re-target: the farthest reached value, rounded up on the start grid
            candidate = std::move(reached[maxIndex]);
            try
            {
                candidate->roundUpOnDomainInterval(startInterval());
            }
            catch (const NotSupportedException& e)
            {
                return {SyncOutcome::Failed, SyncFailureReason::TargetNotRepresentable, {}, e.what()};
            }
        }
    }
}

void SynchronizationManager::clearSynchronization()
{
    model.commonStart = nullptr;
}

void SynchronizationManager::invalidateModel()
{
    model = CommonModel{};
    modelValid = false;
}

const DomainValue* SynchronizationManager::getCommonStart() const
{
    return model.commonStart.get();
}

SyncSetupResult SynchronizationManager::checkReferenceDomains(const std::vector<QueueReader*>& inputs,
                                                              const std::vector<SizeT>& slotIndices) const
{
    TimeProtocol knownProtocol = TimeProtocol::Unknown;
    std::set<ReferenceDomainBin> bins;

    for (SizeT i = 0; i < inputs.size(); ++i)
    {
        const auto& descriptor = inputs[i]->getDomainDescriptor();
        if (!descriptor.assigned())
            continue;

        const auto referenceDomainInfo = descriptor.getReferenceDomainInfo();
        if (!referenceDomainInfo.assigned())
        {
            LOG_D("Input {} domain descriptor Reference Domain Info is not assigned.", slotIndices[i]);
            continue;
        }

        {
            const auto referenceDomainId = referenceDomainInfo.getReferenceDomainId();
            if (!referenceDomainId.assigned() || referenceDomainId.getLength() == 0)
                LOG_D("Input {} Reference Domain ID not assigned.", slotIndices[i]);
        }
        if (referenceDomainInfo.getReferenceTimeProtocol() == TimeProtocol::Unknown)
        {
            LOG_D("Input {} Reference Time Source is Unknown.", slotIndices[i]);
        }
        else
        {
            if (knownProtocol != TimeProtocol::Unknown && referenceDomainInfo.getReferenceTimeProtocol() != knownProtocol)
            {
                return {SyncSetupIssue::ReferenceDomainIncompatible,
                        {slotIndices[i]},
                        "Only one known reference time source is allowed per multi reader"};
            }
            knownProtocol = referenceDomainInfo.getReferenceTimeProtocol();
        }

        const ReferenceDomainBin bin{referenceDomainInfo.getReferenceDomainId(), referenceDomainInfo.getReferenceTimeProtocol()};
        auto element = bins.begin();
        while (element != bins.end())
        {
            // Traverse one group of bins sharing a reference domain id
            bool needsKnownTimeProtocol = false;
            bool hasKnownTimeProtocol = false;
            const auto groupDomainId = element->id;

            while (element != bins.end() && element->id == groupDomainId)
            {
                if (groupDomainId.assigned() && bin.id.assigned() && groupDomainId != bin.id)
                {
                    // Distinct assigned reference domain ids require a known time source to relate them
                    needsKnownTimeProtocol = true;
                }
                if (element->timeProtocol != TimeProtocol::Unknown)
                {
                    hasKnownTimeProtocol = true;
                }
                ++element;
            }

            if (needsKnownTimeProtocol && !hasKnownTimeProtocol)
            {
                return {SyncSetupIssue::ReferenceDomainIncompatible, {slotIndices[i]}, "Reference domain is incompatible"};
            }
        }

        bins.insert(bin);
    }

    return {};
}

RatioPtr SynchronizationManager::startInterval() const
{
    if (startOnFullUnitOfDomain)
        return Ratio(1, 1);

    // Direct path: the minimum aligned block. The resampled path (Phase 5) uses the
    // output sample period instead.
    return Ratio(static_cast<Int>(model.blockLcm), static_cast<Int>(model.commonSampleRate));
}

END_NAMESPACE_OPENDAQ
