#include <opendaq/multi_reader/read_coordinator.h>

#include <algorithm>
#include <limits>

BEGIN_NAMESPACE_OPENDAQ

namespace multi_reader
{

ReadCoordinator::ReadCoordinator(const LoggerComponentPtr& logger)
    : loggerComponent(logger)
{
}

void ReadCoordinator::configure(const std::vector<QueueReader*>& /*inputs*/, const CommonModel& /*model*/)
{
    // Direct path: every input copies straight from its queue; nothing to build.
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
    // Round the minimum up to whole blocks: a request below one block is never servable.
    return (minimum + block - 1) / block * block;
}

SizeT ReadCoordinator::alignAvailable(SizeT rawAvailableCommon, const CommonModel& model, SizeT minReadCount)
{
    const SizeT block = model.blockLcm > 0 ? model.blockLcm : 1;
    const SizeT count = rawAvailableCommon / block * block;
    if (count < effectiveMinimum(model, minReadCount))
        return 0;
    return count;
}

SizeT ReadCoordinator::getAvailableCount(const std::vector<QueueReader*>& inputs, const CommonModel& model, SizeT minReadCount) const
{
    if (inputs.empty())
        return 0;

    SizeT availableCommon = std::numeric_limits<SizeT>::max();
    for (auto* input : inputs)
    {
        // Common-rate equivalent, stopping at the input's earliest event boundary.
        availableCommon = std::min(availableCommon, input->getAvailableSamplesUntilEvent());
    }

    return alignAvailable(availableCommon, model, minReadCount);
}

ReadPlan ReadCoordinator::createPlan(SizeT requestedCommonCount,
                                     const std::vector<QueueReader*>& inputs,
                                     const CommonModel& model,
                                     SizeT minReadCount,
                                     void* const* valueBuffers,
                                     void* const* domainBuffers) const
{
    // Availability is derived here from the inputs; the overload below reuses a count the owner
    // already computed during its data-plane pass, avoiding the second walk over every input.
    return createPlan(requestedCommonCount,
                      getAvailableCount(inputs, model, minReadCount),
                      model,
                      minReadCount,
                      valueBuffers,
                      domainBuffers);
}

ReadPlan ReadCoordinator::createPlan(SizeT requestedCommonCount,
                                     SizeT alignedAvailableCommon,
                                     const CommonModel& model,
                                     SizeT minReadCount,
                                     void* const* valueBuffers,
                                     void* const* domainBuffers) const
{
    ReadPlan plan;

    const SizeT block = model.blockLcm > 0 ? model.blockLcm : 1;

    SizeT count = requestedCommonCount / block * block;  // requests round down to whole blocks
    count = std::min(count, alignedAvailableCommon);
    if (count < effectiveMinimum(model, minReadCount))
        count = 0;

    plan.commonCount = count;
    // Non-owning: the owner's staged scratch outlives the plan, so no per-read allocation or copy.
    plan.valueBuffers = valueBuffers;
    plan.domainBuffers = domainBuffers;

    return plan;
}

CommitResult ReadCoordinator::commit(const ReadPlan& plan, const std::vector<QueueReader*>& inputs, std::string& errorMessage)
{
    if (plan.commonCount == 0)
        return CommitResult::Ok;

    for (SizeT i = 0; i < inputs.size(); ++i)
    {
        SizeT count = plan.commonCount;
        // A null buffer array means "no buffers" (every entry null) - matches passing an all-null
        // array; only index when the array is present.
        void* valueBuffer = plan.valueBuffers ? plan.valueBuffers[i] : nullptr;
        void* domainBuffer = plan.domainBuffers ? plan.domainBuffers[i] : nullptr;
        const auto result = inputs[i]->read(valueBuffer, domainBuffer, &count);
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

std::vector<SizeT> ReadCoordinator::discardLeftoverSegments(const std::vector<QueueReader*>& inputs,
                                                            const CommonModel& model,
                                                            SizeT minReadCount)
{
    std::vector<SizeT> discarded;
    // A segment shorter than the smallest servable request can never be read - use the
    // same aligned minimum the availability check enforces, not just one block
    const SizeT minimum = effectiveMinimum(model, minReadCount);

    for (SizeT i = 0; i < inputs.size(); ++i)
    {
        // An input already at an event boundary has nothing left to discard - the pending
        // event surfaces through the state evaluation instead.
        if (inputs[i]->hasPendingEvents())
            continue;
        if (inputs[i]->discardLeftoverSegment(minimum))
            discarded.push_back(i);
    }
    return discarded;
}

}  // namespace multi_reader

END_NAMESPACE_OPENDAQ
