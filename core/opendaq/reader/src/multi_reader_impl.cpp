#include <coreobjects/ownable_ptr.h>
#include <coreobjects/property_object_factory.h>
#include <coretypes/validation.h>
#include <opendaq/custom_log.h>
#include <opendaq/input_port_factory.h>
#include <opendaq/multi_reader_impl.h>
#include <opendaq/reader_errors.h>
#include <opendaq/reader_factory.h>
#include <opendaq/tags_private_ptr.h>

#include <fmt/format.h>

BEGIN_NAMESPACE_OPENDAQ

// The multi reader internals (Input, QueueReader, CallbackGate) live in daq::multi_reader
using namespace multi_reader;

// --- Construction -----------------------------------------------------------------------------

namespace
{

// Legacy list factories delegate through here so the builder constructor is the single wiring path.
MultiReaderBuilderPtr builderFromLegacyArgs(const ListPtr<IComponent>& list,
                                            SampleType valueReadType,
                                            SampleType domainReadType,
                                            ReadMode mode,
                                            Int requiredCommonSampleRate,
                                            Bool startOnFullUnitOfDomain,
                                            SizeT minReadCount)
{
    if (!list.assigned())
        DAQ_THROW_EXCEPTION(NotAssignedException, "List of inputs is not assigned");

    auto builder = MultiReaderBuilder();
    for (const auto& component : list)
    {
        if (auto signal = component.asPtrOrNull<ISignal>(); signal.assigned())
            builder.addSignal(signal);
        else if (auto port = component.asPtrOrNull<IInputPort>(); port.assigned())
            builder.addInputPort(port);
        else
            DAQ_THROW_EXCEPTION(InvalidParameterException, "One of the elements of input list is not signal or input port");
    }

    builder.setValueReadType(valueReadType);
    builder.setDomainReadType(domainReadType);
    builder.setReadMode(mode);
    builder.setRequiredCommonSampleRate(requiredCommonSampleRate);
    builder.setStartOnFullUnitOfDomain(startOnFullUnitOfDomain);
    builder.setMinReadCount(minReadCount);
    return builder;
}

}  // namespace

MultiReaderImpl::MultiReaderImpl(const ListPtr<IComponent>& list,
                                 SampleType valueReadType,
                                 SampleType domainReadType,
                                 ReadMode mode,
                                 ReadTimeoutType /*timeoutType*/,  // only All is honored
                                 Int requiredCommonSampleRate,
                                 Bool startOnFullUnitOfDomain,
                                 SizeT minReadCount)
    : MultiReaderImpl(builderFromLegacyArgs(list, valueReadType, domainReadType, mode, requiredCommonSampleRate, startOnFullUnitOfDomain, minReadCount))
{
}

MultiReaderImpl::MultiReaderImpl(const MultiReaderBuilderPtr& builder)
    : requiredCommonSampleRate(builder.getRequiredCommonSampleRate())
    , allowDifferentRates(builder.getAllowDifferentSamplingRates())
    , startOnFullUnitOfDomain(builder.getStartOnFullUnitOfDomain())
    , minReadCount(builder.getMinReadCount())
    , notificationMethod(builder.getInputPortNotificationMethod())
    , notificationMethodsList(builder.getInputPortNotificationMethods())
    , valueReadType(builder.getValueReadType())
    , domainReadType(builder.getDomainReadType())
    , readMode(builder.getReadMode())
{
    internalAddRef();
    try
    {
        auto sourceComponents = builder.getSourceComponents();
        normalizeSources(sourceComponents);

        loggerComponent = context.getLogger().getOrAddComponent("MultiReader");

        mainInputId = builder.getMainInput();
        if (mainInputId.assigned() && mainInputId.getLength() == 0)
            mainInputId = nullptr;
        maxSynchronizationDistance = builder.getMaxSynchronizationDistance();
        dataLossTimeout = builder.getDataLossTimeout();

        // Single source of truth: the multi reader always reads an integral common domain
        if (domainReadType == SampleType::Undefined)
            domainReadType = SampleType::Int64;

        callbackGate = std::make_shared<CallbackGate>();

        std::vector<SignalPtr> signalsToConnect;
        auto ports = createOrAdoptPorts(sourceComponents, signalsToConnect);
        createSlots(ports);

        // Connect only now that the slots listen, so every connection (and its initial
        // descriptor event) arrives through the normal notification path. No state lock held:
        // the callbacks re-enter through slotConnected/slotPacketReceived.
        for (SizeT i = 0; i < signalsToConnect.size(); ++i)
        {
            if (signalsToConnect[i].assigned())
                ports[i].connect(signalsToConnect[i]);
        }

        {
            std::lock_guard lock(mutex);
            if (mainInputId.assigned() && findSlotById(slots, mainInputId) == notFound)
                DAQ_THROW_EXCEPTION(NotFoundException, "The selected main input does not match any source component");
            for (auto* slot : slots)
                slot->setPortActive(isActive);
        }
    }
    catch (...)
    {
        this->releaseWeakRefOnException();
        throw;
    }
}

MultiReaderImpl::~MultiReaderImpl()
{
    for (auto* slot : slots)
        slot->detachListener();

    if (!portBinder.assigned())
    {
        for (auto* slot : slots)
            slot->getPort().remove();
    }
}

void MultiReaderImpl::normalizeSources(const ListPtr<IComponent>& list)
{
    if (!list.assigned())
        DAQ_THROW_EXCEPTION(NotAssignedException, "List of inputs is not assigned");
    if (list.getCount() == 0)
        DAQ_THROW_EXCEPTION(InvalidParameterException, "Need at least one signal.");

    if (!context.assigned())
        context = list[0].getContext();

    // Determined once and only ever narrows from Unknown; homogeneity is enforced by createOrAdoptPorts
    if (typeOfInputs == InputType::Unknown)
    {
        if (list[0].supportsInterface(IInputPort::Id))
            typeOfInputs = InputType::Ports;
        else if (list[0].supportsInterface(ISignal::Id))
            typeOfInputs = InputType::Signals;
        else
            DAQ_THROW_EXCEPTION(InvalidParameterException, "Invalid component type, only IInputPort and ISignal are supported.");
    }
}

ListPtr<IInputPortConfig> MultiReaderImpl::createOrAdoptPorts(const ListPtr<IComponent>& list, std::vector<SignalPtr>& signalsToConnect) const
{
    auto portList = List<IInputPortConfig>();
    for (const auto& el : list)
    {
        if (auto signal = el.asPtrOrNull<ISignal>(); signal.assigned())
        {
            if (typeOfInputs == InputType::Ports)
                DAQ_THROW_EXCEPTION(InvalidParameterException, "Cannot pass both input ports and signals as items");

            auto port = InputPort(context, nullptr, fmt::format("multi_reader_signal_{}", signal.getLocalId()));
            port.getTags().asPtr<ITagsPrivate>().add("MultiReaderInternalPort");

            // Connected by the caller once the slot listens, so no notification is missed
            portList.pushBack(port);
            signalsToConnect.push_back(signal);
        }
        else if (auto port = el.asPtrOrNull<IInputPortConfig>(); port.assigned())
        {
            if (typeOfInputs == InputType::Signals)
                DAQ_THROW_EXCEPTION(InvalidParameterException, "Cannot pass both input ports and signals as items");

            portList.pushBack(port);
            signalsToConnect.push_back(nullptr);
        }
        else
        {
            DAQ_THROW_EXCEPTION(InvalidParameterException, "One of the elements of input list is not signal or input port");
        }
    }

    return portList;
}

void MultiReaderImpl::createSlots(const ListPtr<IInputPortConfig>& inputPorts)
{
    if (notificationMethodsList.assigned() && notificationMethodsList.getCount() > 0 &&
        notificationMethodsList.getCount() != inputPorts.getCount())
    {
        DAQ_THROW_EXCEPTION(InvalidParameterException,
                            "The list of source components is not of same size than the list of notification methods.");
    }

    const SizeT firstPosition = slots.size();
    SizeT position = firstPosition;
    for (const auto& port : inputPorts)
    {
        if (!port.getTags().contains("MultiReaderInternalPort"))
        {
            if (!portBinder.assigned())
                portBinder = PropertyObject();
            port.asPtr<IOwnable>().setOwner(portBinder);
        }

        const auto portNotificationMethod = notificationMethodsList.assigned() && notificationMethodsList.getCount() > 0
                                                ? notificationMethodsList[position - firstPosition]
                                                : notificationMethod;

        if (portNotificationMethod != PacketReadyNotification::Unspecified)
        {
            port.setNotificationMethod(portNotificationMethod);
        }
        else if (typeOfInputs == InputType::Signals)
        {
            // A reader built from signals owns its ports and must pick a concrete method
            DAQ_THROW_EXCEPTION(InvalidParameterException,
                                "Multi reader created from signals cannot have an unspecified input port notification method.");
        }

        auto slotObject = createWithImplementation<IInputPortNotifications, Input>(position,
                                                                                       port,
                                                                                       valueReadType,
                                                                                       domainReadType,
                                                                                       readMode,
                                                                                       loggerComponent,
                                                                                       static_cast<IInputListener*>(this),
                                                                                       typeOfInputs == InputType::Signals,
                                                                                       callbackGate);
        auto* slot = static_cast<Input*>(slotObject.getObject());
        // Slots default to used; the gate's used count follows the slot set
        callbackGate->adjustUsed(1);
        slot->listen(slotObject);

        slotObjects.push_back(std::move(slotObject));
        slots.push_back(slot);
        ++position;
    }
}

// --- IInputListener (blank stubs) --------------------------------------------------------------

bool MultiReaderImpl::slotAcceptsSignal(SizeT /*slotIndex*/, const SignalPtr& /*signal*/)
{
    return true;
}

void MultiReaderImpl::slotConnected(SizeT /*slotIndex*/)
{
}

void MultiReaderImpl::slotDisconnected(SizeT /*slotIndex*/)
{
}

void MultiReaderImpl::slotPacketReceived(SizeT /*slotIndex*/, bool /*forceEvaluation*/)
{
}

// --- IReader (blank stubs) ----------------------------------------------------------------------

ErrCode MultiReaderImpl::getAvailableCount(SizeT* count)
{
    OPENDAQ_PARAM_NOT_NULL(count);

    *count = 0;
    return OPENDAQ_SUCCESS;
}

ErrCode MultiReaderImpl::setOnDataAvailable(IProcedure* /*callback*/)
{
    return OPENDAQ_SUCCESS;
}

ErrCode MultiReaderImpl::setExternalListener(IInputPortNotifications* /*listener*/)
{
    return OPENDAQ_SUCCESS;
}

ErrCode MultiReaderImpl::getEmpty(Bool* empty)
{
    OPENDAQ_PARAM_NOT_NULL(empty);

    *empty = True;
    return OPENDAQ_SUCCESS;
}

// --- ISampleReader (blank stubs) ----------------------------------------------------------------

ErrCode MultiReaderImpl::getValueReadType(SampleType* sampleType)
{
    OPENDAQ_PARAM_NOT_NULL(sampleType);

    *sampleType = valueReadType;
    return OPENDAQ_SUCCESS;
}

ErrCode MultiReaderImpl::getDomainReadType(SampleType* sampleType)
{
    OPENDAQ_PARAM_NOT_NULL(sampleType);

    *sampleType = domainReadType;
    return OPENDAQ_SUCCESS;
}

ErrCode MultiReaderImpl::setValueTransformFunction(IFunction* /*transform*/)
{
    return OPENDAQ_SUCCESS;
}

ErrCode MultiReaderImpl::setDomainTransformFunction(IFunction* /*transform*/)
{
    return OPENDAQ_SUCCESS;
}

ErrCode MultiReaderImpl::getReadMode(ReadMode* mode)
{
    OPENDAQ_PARAM_NOT_NULL(mode);

    *mode = readMode;
    return OPENDAQ_SUCCESS;
}

// --- IMultiReader (blank stubs) -----------------------------------------------------------------

ErrCode MultiReaderImpl::read(void* /*samples*/, SizeT* count, SizeT /*timeoutMs*/, IMultiReaderStatus** status)
{
    OPENDAQ_PARAM_NOT_NULL(count);

    *count = 0;
    if (status != nullptr)
        *status = nullptr;
    return OPENDAQ_SUCCESS;
}

ErrCode MultiReaderImpl::readWithDomain(void* /*samples*/, void* /*domain*/, SizeT* count, SizeT /*timeoutMs*/, IMultiReaderStatus** status)
{
    OPENDAQ_PARAM_NOT_NULL(count);

    *count = 0;
    if (status != nullptr)
        *status = nullptr;
    return OPENDAQ_SUCCESS;
}

ErrCode MultiReaderImpl::skipSamples(SizeT* count, IMultiReaderStatus** status)
{
    OPENDAQ_PARAM_NOT_NULL(count);

    *count = 0;
    if (status != nullptr)
        *status = nullptr;
    return OPENDAQ_SUCCESS;
}

ErrCode MultiReaderImpl::getTickResolution(IRatio** resolution)
{
    OPENDAQ_PARAM_NOT_NULL(resolution);

    *resolution = nullptr;
    return OPENDAQ_SUCCESS;
}

ErrCode MultiReaderImpl::getOrigin(IString** origin)
{
    OPENDAQ_PARAM_NOT_NULL(origin);

    *origin = nullptr;
    return OPENDAQ_SUCCESS;
}

ErrCode MultiReaderImpl::getOffset(void* /*domainStart*/)
{
    return OPENDAQ_SUCCESS;
}

ErrCode MultiReaderImpl::getIsSynchronized(Bool* isSynchronized)
{
    OPENDAQ_PARAM_NOT_NULL(isSynchronized);

    *isSynchronized = False;
    return OPENDAQ_SUCCESS;
}

ErrCode MultiReaderImpl::getCommonSampleRate(Int* commonSampleRate)
{
    OPENDAQ_PARAM_NOT_NULL(commonSampleRate);

    *commonSampleRate = requiredCommonSampleRate;
    return OPENDAQ_SUCCESS;
}

ErrCode MultiReaderImpl::setActive(Bool isActive)
{
    std::lock_guard lock(mutex);
    this->isActive = isActive;
    return OPENDAQ_SUCCESS;
}

ErrCode MultiReaderImpl::getActive(Bool* isActive)
{
    OPENDAQ_PARAM_NOT_NULL(isActive);

    std::lock_guard lock(mutex);
    *isActive = this->isActive;
    return OPENDAQ_SUCCESS;
}

ErrCode MultiReaderImpl::addInput(IComponent* port)
{
    OPENDAQ_PARAM_NOT_NULL(port);

    return daqTry([&]
    {
        // D1: the reader only accepts unconnected ports
        auto inputPort = ComponentPtr::Borrow(port).asPtrOrNull<IInputPort>();
        if (inputPort.assigned() && inputPort.getConnection().assigned())
            return DAQ_MAKE_ERROR_INFO(OPENDAQ_ERR_INVALIDPARAMETER, "Multi reader inputs must be unconnected ports");
        return OPENDAQ_SUCCESS;
    });
}

ErrCode MultiReaderImpl::removeInput(IString* /*id*/)
{
    return OPENDAQ_SUCCESS;
}

ErrCode MultiReaderImpl::setInputUsed(IString* /*id*/, Bool /*isUsed*/)
{
    return OPENDAQ_SUCCESS;
}

ErrCode MultiReaderImpl::getInputUsed(IString* /*id*/, Bool* isUsed)
{
    OPENDAQ_PARAM_NOT_NULL(isUsed);

    *isUsed = True;
    return OPENDAQ_SUCCESS;
}

ErrCode MultiReaderImpl::setMainInput(IString* /*id*/)
{
    return OPENDAQ_SUCCESS;
}

ErrCode MultiReaderImpl::getMainInput(IString** id)
{
    OPENDAQ_PARAM_NOT_NULL(id);

    std::lock_guard lock(mutex);
    *id = mainInputId.addRefAndReturn();
    return OPENDAQ_SUCCESS;
}

// --- IReaderConfig (blank stubs) ----------------------------------------------------------------

ErrCode MultiReaderImpl::getValueTransformFunction(IFunction** transform)
{
    OPENDAQ_PARAM_NOT_NULL(transform);

    *transform = nullptr;
    return OPENDAQ_SUCCESS;
}

ErrCode MultiReaderImpl::getDomainTransformFunction(IFunction** transform)
{
    OPENDAQ_PARAM_NOT_NULL(transform);

    *transform = nullptr;
    return OPENDAQ_SUCCESS;
}

ErrCode MultiReaderImpl::getInputPorts(IList** ports)
{
    OPENDAQ_PARAM_NOT_NULL(ports);

    std::lock_guard lock(mutex);

    auto list = List<IInputPortConfig>();
    for (auto* slot : slots)
        list.pushBack(slot->getPort());

    *ports = list.detach();
    return OPENDAQ_SUCCESS;
}

ErrCode MultiReaderImpl::getReadTimeoutType(ReadTimeoutType* timeoutType)
{
    OPENDAQ_PARAM_NOT_NULL(timeoutType);

    *timeoutType = ReadTimeoutType::All;
    return OPENDAQ_SUCCESS;
}

ErrCode MultiReaderImpl::markAsInvalid()
{
    std::lock_guard lock(mutex);
    invalid = true;
    return OPENDAQ_SUCCESS;
}

ErrCode MultiReaderImpl::getIsValid(Bool* isValid)
{
    OPENDAQ_PARAM_NOT_NULL(isValid);

    std::lock_guard lock(mutex);
    *isValid = !invalid;
    return OPENDAQ_SUCCESS;
}

void MultiReaderImpl::internalDispose(bool)
{
    for (auto* slot : slots)
        slot->detachListener();

    // portBinder is deliberately kept: the destructor must not remove() adopted ports of a
    // disposed reader (dispose-and-rebuild is the documented consumer pattern).
    invalid = true;
    isActive = false;
}

// --- Factories --------------------------------------------------------------------------------

OPENDAQ_DEFINE_CLASS_FACTORY(LIBRARY_FACTORY,
                             MultiReader,
                             IList*,
                             signals,
                             SampleType,
                             valueReadType,
                             SampleType,
                             domainReadType,
                             ReadMode,
                             mode,
                             ReadTimeoutType,
                             timeoutType)

OPENDAQ_DEFINE_CLASS_FACTORY_WITH_INTERFACE_AND_CREATEFUNC_OBJ(LIBRARY_FACTORY,
                                                               MultiReaderImpl,
                                                               IMultiReader,
                                                               createMultiReaderEx,
                                                               IList*,
                                                               signals,
                                                               SampleType,
                                                               valueReadType,
                                                               SampleType,
                                                               domainReadType,
                                                               ReadMode,
                                                               mode,
                                                               ReadTimeoutType,
                                                               timeoutType,
                                                               Int,
                                                               requiredCommonSampleRate,
                                                               Bool,
                                                               startOnFullUnitOfDomain,
                                                               SizeT,
                                                               minReadCount)

extern "C" daq::ErrCode PUBLIC_EXPORT createMultiReaderFromBuilder(IMultiReader** objTmp, IMultiReaderBuilder* builder)
{
    return daq::createObject<IMultiReader, MultiReaderImpl>(objTmp, builder);
}

END_NAMESPACE_OPENDAQ
