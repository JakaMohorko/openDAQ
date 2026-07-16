#include <opendaq/read_coordinator.h>

#include <algorithm>
#include <limits>

BEGIN_NAMESPACE_OPENDAQ

ReadCoordinator::ReadCoordinator(const LoggerComponentPtr& logger)
    : loggerComponent(logger)
{
}

void ReadCoordinator::configure(const std::vector<QueueReader*>& /*inputs*/, const CommonModel& /*model*/)
{
    // Direct path: every input copies straight from its queue; nothing to build.
    // Phase 5 selects per-input pipelines here (direct copy iff the source grid equals
    // the output grid, otherwise a resampler freshly built via the injected builder).
    configured = true;
}

void ReadCoordinator::invalidate()
{
    configured = false;
}

bool ReadCoordinator::isConfigured() const
{
    return configured;
}

SizeT ReadCoordinator::effectiveMinimum(const CommonModel& model, SizeT minReadCount)
{
    const SizeT block = model.blockLcm > 0 ? model.blockLcm : 1;
    const SizeT minimum = minReadCount > block ? minReadCount : block;
    // Round the minimum up to whole blocks: a request below one block is never servable
    return (minimum + block - 1) / block * block;
}

SizeT ReadCoordinator::getAvailableCount(const std::vector<QueueReader*>& inputs, const CommonModel& model, SizeT minReadCount) const
{
    if (inputs.empty())
        return 0;

    const SizeT block = model.blockLcm > 0 ? model.blockLcm : 1;

    SizeT availableCommon = std::numeric_limits<SizeT>::max();
    for (auto* input : inputs)
    {
        // Common-rate equivalent, stopping at the input's earliest event boundary
        availableCommon = std::min(availableCommon, input->getAvailableSamplesUntilEvent());
    }

    SizeT count = availableCommon / block * block;
    if (count < effectiveMinimum(model, minReadCount))
        return 0;
    return count;
}

ReadPlan ReadCoordinator::createPlan(SizeT requestedCommonCount,
                                     const std::vector<QueueReader*>& inputs,
                                     const CommonModel& model,
                                     SizeT minReadCount,
                                     void* const* valueBuffers,
                                     void* const* domainBuffers) const
{
    ReadPlan plan;

    const SizeT block = model.blockLcm > 0 ? model.blockLcm : 1;
    const SizeT available = getAvailableCount(inputs, model, minReadCount);

    SizeT count = requestedCommonCount / block * block;  // requests round down to whole blocks
    count = std::min(count, available);
    if (count < effectiveMinimum(model, minReadCount))
        count = 0;

    plan.commonCount = count;
    plan.valueBuffers.resize(inputs.size(), nullptr);
    plan.domainBuffers.resize(inputs.size(), nullptr);
    for (SizeT i = 0; i < inputs.size(); ++i)
    {
        plan.valueBuffers[i] = valueBuffers ? valueBuffers[i] : nullptr;
        plan.domainBuffers[i] = domainBuffers ? domainBuffers[i] : nullptr;
    }

    return plan;
}

CommitResult ReadCoordinator::commit(const ReadPlan& plan, const std::vector<QueueReader*>& inputs, std::string& errorMessage)
{
    if (plan.commonCount == 0)
        return CommitResult::Ok;

    for (SizeT i = 0; i < inputs.size(); ++i)
    {
        SizeT count = plan.commonCount;
        const auto result = inputs[i]->read(plan.valueBuffers[i], plan.domainBuffers[i], &count);
        if (result != AdvanceResult::Success || count != plan.commonCount)
        {
            // Availability validated the whole plan; a failing input is an internal invariant break
            errorMessage = "Input at position " + std::to_string(i) + " failed to deliver a validated read plan (" +
                           std::to_string(count) + " of " + std::to_string(plan.commonCount) + " samples)";
            return CommitResult::InternalError;
        }
    }
    return CommitResult::Ok;
}

CommitResult ReadCoordinator::skip(const ReadPlan& plan, const std::vector<QueueReader*>& inputs, std::string& errorMessage)
{
    if (plan.commonCount == 0)
        return CommitResult::Ok;

    for (SizeT i = 0; i < inputs.size(); ++i)
    {
        SizeT count = plan.commonCount;
        const auto result = inputs[i]->skip(&count);
        if (result != AdvanceResult::Success || count != plan.commonCount)
        {
            errorMessage = "Input at position " + std::to_string(i) + " failed to skip a validated plan (" +
                           std::to_string(count) + " of " + std::to_string(plan.commonCount) + " samples)";
            return CommitResult::InternalError;
        }
    }
    return CommitResult::Ok;
}

std::vector<SizeT> ReadCoordinator::discardLeftoverSegments(const std::vector<QueueReader*>& inputs, const CommonModel& model)
{
    std::vector<SizeT> discarded;
    const SizeT block = model.blockLcm > 0 ? model.blockLcm : 1;

    for (SizeT i = 0; i < inputs.size(); ++i)
    {
        if (inputs[i]->discardLeftoverSegment(block))
            discarded.push_back(i);
    }
    return discarded;
}

END_NAMESPACE_OPENDAQ
