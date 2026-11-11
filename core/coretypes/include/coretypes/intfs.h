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
#include <coretypes/arguments.h>
#include <coretypes/validation.h>
#include <coretypes/common.h>
#include <coretypes/errorinfo.h>
#include <coretypes/errors.h>
#include <coretypes/exceptions.h>
#include <coretypes/inspectable.h>
#include <coretypes/delegate.hpp>
#include <atomic>
#include <cassert>
#include <string>

#if defined(__GNUC__) && !defined(_MSC_VER)
#include <cxxabi.h>
#endif

extern "C" PUBLIC_EXPORT void daqTrackObject(daq::IBaseObject* obj);
extern "C" PUBLIC_EXPORT void daqUntrackObject(daq::IBaseObject* obj);
extern "C" PUBLIC_EXPORT size_t daqGetTrackedObjectCount();
extern "C" PUBLIC_EXPORT void daqPrintTrackedObjects();
extern "C" PUBLIC_EXPORT void daqClearTrackedObjects();
extern "C" PUBLIC_EXPORT daq::Bool daqIsTrackingObjects();
extern "C" PUBLIC_EXPORT void daqPrepareHeapAlloc();
extern "C" PUBLIC_EXPORT void daqDisableObjectTracking();
extern "C" PUBLIC_EXPORT void daqEnableObjectTracking();

BEGIN_NAMESPACE_OPENDAQ

#ifdef OPENDAQ_TRACK_SHARED_LIB_OBJECT_COUNT
extern std::atomic<std::size_t> daqSharedLibObjectCount;
#endif

struct MallocDeleter
{
    void operator()(void* ptr)
    {
        free(ptr);
    }
};

template <typename T>
struct DiscoverOnly
{
    using Base = IBaseObject;
};

template <typename, typename = std::void_t<>>
struct HasDeclaredBase : std::false_type
{
};

template <typename T>
struct HasDeclaredBase<T, std::void_t<typename T::Base>> : std::true_type
{
};

template <typename T>
struct BaseInterface;

template <typename T>
struct BaseType
{
    using Args = typename std::conditional_t<
        std::is_same_v<typename T::Base, IBaseObject>,
        daq::Args<>,
        typename Meta::PrependType<typename T::Base, typename BaseInterface<typename T::Base>::Interfaces>::Args
    >;
};

template <>
struct BaseType<IBaseObject>
{
    using Args = daq::Args<IBaseObject>;
};

// clang-format off

template <typename T>
struct BaseInterface
{
    // Have to provide an indirection type BaseType<T> otherwise std::conditional_t<> tries to evaluate T::Base even if it is not chosen.
    // This results in a compile error as it doesn't exist on most interfaces.

    using Interfaces = typename std::conditional_t<HasDeclaredBase<T>::value,
                                                   BaseType<T>,
                                                   BaseType<IBaseObject>
                                                  >::Args;

};

// clang-format on

template <typename TMainInterface, typename... TInterfaces>
struct ActualInterfaces
{
    using BaseInterfaces =
        typename Meta::Flatten<Args<
            Args<TMainInterface, TInterfaces..., IBaseObject>,
            typename BaseInterface<TMainInterface>::Interfaces,
            typename BaseInterface<TInterfaces>::Interfaces...>
        >::Args;
};

template <typename T>
struct SupportsInterface;

template <>
struct SupportsInterface<Details::EndTag>
{
    template <class TMainInterface, bool AddRef, class This>
    static bool Check(const IntfID& id, void** intf, This object)
    {
        if (IUnknown::Id == id)
        {
            using RawT = std::remove_const_t<std::remove_pointer_t<This>>;
            const auto obj = static_cast<IUnknown*>(static_cast<TMainInterface*>(const_cast<RawT*>(object)));
            if constexpr (AddRef)
            {
                obj->addRef();
            }

            *intf = static_cast<void*>(obj);
            return true;
        }

        return false;
    }

    static void AddInterfaceIds(IntfID* /*ids*/)
    {
    }

    static constexpr std::size_t Count()
    {
        return 0;
    }
};

template <typename TArgs>
struct SupportsInterface
{
    using Interface = typename TArgs::Head;

    template <class TMainInterface, bool AddRef, class This>
    static bool Check(const IntfID& id, void** intf, This object)
    {
        const auto intfId = Interface::Id;
        if (intfId == id)
        {
            using RawT = std::remove_const_t<std::remove_pointer_t<This>>;

            Interface* obj;
            if constexpr (std::is_base_of_v<Interface, TMainInterface>)
            {
                obj = static_cast<Interface*>(static_cast<TMainInterface*>(const_cast<RawT*>(object)));
            }
            else
            {
                obj = static_cast<Interface*>(const_cast<RawT*>(object));
            }

            if constexpr (AddRef)
            {
                obj->addRef();
            }

            *intf = static_cast<void*>(obj);
            return true;
        }
        return SupportsInterface<typename TArgs::Tail>::template Check<TMainInterface, AddRef, This>(id, intf, object);
    }

    static void AddInterfaceIds(IntfID* ids)
    {
        *ids = Interface::Id;
        ids++;

        SupportsInterface<typename TArgs::Tail>::AddInterfaceIds(ids);
    }

    static constexpr std::size_t Count()
    {
        return TArgs::Arity();
    }
};

// [api(module)]
template <typename MainInterface, typename... Interfaces>
class DAQ_EMPTY_BASES GenericObjInstance : public MainInterface, public Interfaces...
{
public:
    using InterfaceIds = SupportsInterface<typename ActualInterfaces<MainInterface, Interfaces...>::BaseInterfaces>;

    GenericObjInstance()
        : refAdded(false)
        , disposeCalled(false)
    {
#ifndef NDEBUG
        const auto thisBaseObject = getThisAsBaseObject();
        daqTrackObject(thisBaseObject);
#endif
#ifdef OPENDAQ_TRACK_SHARED_LIB_OBJECT_COUNT
        std::atomic_fetch_add_explicit(&daqSharedLibObjectCount, std::size_t{1}, std::memory_order_relaxed);
#endif
    }
    virtual ~GenericObjInstance()
    {
#ifndef NDEBUG
        const auto thisBaseObject = getThisAsBaseObject();
        daqUntrackObject(thisBaseObject);
#endif
#ifdef OPENDAQ_TRACK_SHARED_LIB_OBJECT_COUNT
        std::atomic_fetch_sub_explicit(&daqSharedLibObjectCount, std::size_t{1}, std::memory_order_acq_rel);
#endif
    }

    // [api(core)]
    virtual int INTERFACE_FUNC addRef() override
    {
        return 0;
    }

    // [api(core)]
    virtual int INTERFACE_FUNC releaseRef() override
    {
        return 0;
    }

    // [api(core)]
    ErrCode INTERFACE_FUNC dispose() override
    {
        if (!disposeCalled)
        {
            this->internalDispose(true);
            disposeCalled = true;
        }
        return OPENDAQ_SUCCESS;
    }

    // [api(core)]
    virtual ErrCode INTERFACE_FUNC queryInterface(const IntfID& id, void** intf) override
    {
        OPENDAQ_PARAM_NOT_NULL(intf);

        if (InterfaceIds::template Check<MainInterface, true>(id, intf, this))
        {
            return OPENDAQ_SUCCESS;
        }

        return OPENDAQ_ERR_NOINTERFACE;
    }

    // [api(core)]
    virtual ErrCode INTERFACE_FUNC borrowInterface(const IntfID& id, void** intf) const override
    {
        OPENDAQ_PARAM_NOT_NULL(intf);

        if (InterfaceIds::template Check<MainInterface, false>(id, intf, this))
        {
            return OPENDAQ_SUCCESS;
        }

        return OPENDAQ_ERR_NOINTERFACE;
    }

    // [api(core)]
    virtual ErrCode INTERFACE_FUNC getHashCode(SizeT* hashCode) override
    {
        OPENDAQ_PARAM_NOT_NULL(hashCode);

        *hashCode = reinterpret_cast<SizeT>(this);
        return OPENDAQ_SUCCESS;
    }

    // [api(core)]
    virtual ErrCode INTERFACE_FUNC equals(IBaseObject* other, Bool* equal) const override
    {
        if (equal == nullptr)
            return makeErrorInfo(OPENDAQ_ERR_ARGUMENT_NULL, "Equal output parameter must not be null.");

        if (other == nullptr)
        {
            *equal = false;
            return OPENDAQ_SUCCESS;
        }

        IBaseObject* lhsRaw = nullptr;

        // Have to call QI otherwise pointers may not match because of multiple inheritance
        this->borrowInterface(IBaseObject::Id, (void**) &lhsRaw);

        // Can't check for "other" ObjectPtr<T> T type so have to use QI anyway
        IBaseObject* rhsRaw = nullptr;
        other->borrowInterface(IBaseObject::Id, (void**) &rhsRaw);

        *equal = lhsRaw == rhsRaw;
        return OPENDAQ_SUCCESS;
    }

    // [api(core)]
    virtual ErrCode INTERFACE_FUNC toString(CharPtr* str) override
    {
        OPENDAQ_PARAM_NOT_NULL(str);

        return MainInterface::OpenDaqType(str);
    }

    // IInspectable

    // [api(core)]
    virtual ErrCode INTERFACE_FUNC getInterfaceIds(SizeT* idCount, IntfID** ids) override
    {
        OPENDAQ_PARAM_NOT_NULL(idCount);

        *idCount = InterfaceIds::Count();
        if (ids == nullptr)
        {
            return OPENDAQ_SUCCESS;
        }

        InterfaceIds::AddInterfaceIds(*ids);
        return OPENDAQ_SUCCESS;
    }

    // [api(core)]
    virtual ErrCode INTERFACE_FUNC getRuntimeClassName(IString** implementationName) override
    {
        OPENDAQ_PARAM_NOT_NULL(implementationName);

        auto id = typeid(*this).name();
#if defined(__GNUC__) && !defined(_MSC_VER)
        int status{};
        std::unique_ptr<char, MallocDeleter> demangled(abi::__cxa_demangle(id, nullptr, nullptr, &status));
        if (status == 0)
        {
            id = demangled.get();
        }
#endif
        if (strncmp(id, "class ", sizeof("class")) == 0)
        {
            id += sizeof("class");
        }
        else if (strncmp(id, "struct ", sizeof("struct")) == 0)
        {
            id += sizeof("struct");
        }

        return createString(implementationName, id);
    }

    // [api(core)]
    [[nodiscard]] bool getRefAdded() const noexcept
    {
        return refAdded;
    }

protected:
    // [api(core)]
    IBaseObject* getThisAsBaseObject()
    {
        return static_cast<IBaseObject*>(static_cast<MainInterface*>(this));
    }

    // [api(core)]
    IBaseObject* getThisAsBaseObject() const
    {
        return const_cast<IBaseObject*>(static_cast<const IBaseObject*>(static_cast<const MainInterface*>(this)));
    }

    // [api(core)]
    void checkAndCallDispose()
    {
        if (!disposeCalled)
            internalDispose(false);
    }

    // [api(core)]
    template <typename... Params>
    ErrCode makeErrorInfo(ErrCode errCode, const std::string& message, Params... params) const
    {
        IErrorInfo* errorInfo = nullptr;
        const ErrCode err = createErrorInfoObjectWithSource(&errorInfo, getThisAsBaseObject(), message, std::forward<Params>(params)...);
        if (OPENDAQ_SUCCEEDED(err))
        {
            errorInfo->setErrorCode(errCode);
            daqSetErrorInfo(errorInfo);
            errorInfo->releaseRef();
        }
        return errCode;
    }

    // [api(core)]
    void clearErrorInfo() const
    {
        daqClearErrorInfo();
    }

    // [api(core)]
    template <template <typename> typename T, typename Interface>
    T<Interface> borrowThis() const
    {
        Interface* thisInterface;
        auto err = this->borrowInterface(Interface::Id, reinterpret_cast<void**>(&thisInterface));
        checkErrorInfo(err);

        return T<Interface>::Borrow(thisInterface);
    }

    // [api(module)]
    template <typename TPtr>
    TPtr borrowPtr() const
    {
        using Interface = typename TPtr::DeclaredInterface;

        Interface* thisInterface;
        auto err = this->borrowInterface(Interface::Id, reinterpret_cast<void**>(&thisInterface));
        checkErrorInfo(err);

        return TPtr::Borrow(thisInterface);
    }

    // [api(core)]
    template <typename Intf = MainInterface>
    Intf* thisInterface()
    {
        Intf* thisInterface;
        auto err = this->queryInterface(Intf::Id, reinterpret_cast<void**>(&thisInterface));
        checkErrorInfo(err);

        return thisInterface;
    }

    // [api(core)]
    template <typename Intf = MainInterface>
    Intf* borrowInterface()
    {
        Intf* thisInterface;
        auto err = this->borrowInterface(Intf::Id, reinterpret_cast<void**>(&thisInterface));
        checkErrorInfo(err);

        return thisInterface;
    }

    // [api(module)]
    template <typename TPtr>
    TPtr thisPtr()
    {
        using Interface = typename TPtr::DeclaredInterface;

        Interface* thisInterface;
        auto err = this->queryInterface(Interface::Id, reinterpret_cast<void**>(&thisInterface));
        checkErrorInfo(err);

        return thisInterface;
    }

    // [api(module)]
    template <typename TReturn, typename TInstance, typename... TArgs>
    auto event(TReturn (TInstance::*func)(TArgs...))
    {
        return delegate<TReturn(TArgs...)>(static_cast<TInstance* const>(this), func);
    }

    // [api(module)]
    template <typename TReturn, typename TInstance, typename... TArgs>
    auto event(TReturn (TInstance::*func)(TArgs...) const)
    {
        return delegate<TReturn(TArgs...)>(static_cast<TInstance* const>(this), func);
    }

    // [api(core)]
    virtual void internalDispose([[maybe_unused]] bool disposing)
    {
    }

    // [api(core)]
    bool refAdded;
    // [api(module, readOnly)]
    bool disposeCalled;
};

// [api(module)]
template <typename... Interfaces>
class ObjInstance : public GenericObjInstance<Interfaces...>
{
public:
    ObjInstance()
        : refCount(0)
    {
    }

    // [api(core)]
    int INTERFACE_FUNC addRef() override
    {
        return std::atomic_fetch_add_explicit(&refCount, 1, std::memory_order_relaxed) + 1;
    }

    // [api(core)]
    int INTERFACE_FUNC releaseRef() override
    {
        const auto newRefCount = std::atomic_fetch_sub_explicit(&refCount, 1, std::memory_order_acq_rel) - 1;
        assert(newRefCount >= 0);
        if (newRefCount == 0)
        {
            GenericObjInstance<Interfaces...>::checkAndCallDispose();
            delete this;
        }

        return newRefCount;
    }

protected:
    // [api(core)]
    int internalAddRef()
    {
        if (this->refAdded)
            return refCount;

        std::atomic_fetch_add_explicit(&refCount, 1, std::memory_order_relaxed);
        this->refAdded = true;
        return refCount;
    }

    // [api(core)]
    int internalAddRefNoCheck()
    {
        return std::atomic_fetch_add_explicit(&refCount, 1, std::memory_order_relaxed);
    }

    // [api(core)]
    int internalReleaseRef()
    {
        return std::atomic_fetch_sub_explicit(&refCount, 1, std::memory_order_acq_rel) - 1;
    }

    // [api(core)]
    int getReferenceCount()
    {
        return std::atomic_load_explicit(&refCount, std::memory_order_acquire);
    }

private:
    std::atomic<int> refCount;
};

class RefCount
{
public:
    RefCount()
        : strong(0)
        , weak(1)
    {
    }

    std::atomic<int> strong;
    std::atomic<int> weak;
};

// [api(module)]
template <typename... Interfaces>
class ObjInstanceSupportsWeakRef : public GenericObjInstance<Interfaces...>
{
public:
    ObjInstanceSupportsWeakRef()
        : refCount(std::make_unique<RefCount>())
    {
    }

    // [api(module)]
    int INTERFACE_FUNC addRef() override
    {
        return std::atomic_fetch_add_explicit(&refCount->strong, 1, std::memory_order_relaxed) + 1;
    }

    // [api(module)]
    int INTERFACE_FUNC releaseRef() override
    {
        const auto newRefCount = std::atomic_fetch_sub_explicit(&refCount->strong, 1, std::memory_order_acq_rel) - 1;
        assert(newRefCount >= 0);

        if (newRefCount == 0)
        {
            // GenericObjInstance<Intfs>::checkAndCallDispose();

            const auto newWeakRefCount = std::atomic_fetch_sub_explicit(&refCount->weak, 1, std::memory_order_acq_rel) - 1;
            if (newWeakRefCount != 0)
            {
                refCount.release();
            }
            delete this;
        }

        return newRefCount;
    }

protected:
    // [api(core)]
    int internalAddRef()
    {
        if (this->refAdded)
            return refCount->strong;

        std::atomic_fetch_add_explicit(&refCount->strong, 1, std::memory_order_relaxed);
        this->refAdded = true;
        return refCount->strong;
    }

    // [api(core)]
    int releaseWeakRefOnException()
    {
        const auto newRefCount = std::atomic_fetch_sub_explicit(&refCount->strong, 1, std::memory_order_acq_rel) - 1;
        assert(newRefCount >= 0);

        if (newRefCount == 0)
        {
            const auto newWeakRefCount = std::atomic_fetch_sub_explicit(&refCount->weak, 1, std::memory_order_acq_rel) - 1;
            if (newWeakRefCount != 0)
            {
                refCount.release();
            }
        }

        return newRefCount;
    }

    // [api(core)]
    int internalAddRefNoCheck()
    {
        return std::atomic_fetch_add_explicit(&refCount->strong, 1, std::memory_order_relaxed);
    }

    // [api(core)]
    int internalReleaseRef()
    {
        return std::atomic_fetch_sub_explicit(&refCount->strong, 1, std::memory_order_acq_rel) - 1;
    }

    // [api(core)]
    int getReferenceCount()
    {
        return std::atomic_load_explicit(&refCount->strong, std::memory_order_acquire);
    }

    // [api(core)]
    std::unique_ptr<RefCount> refCount{};
};

// [api(module)]
template <typename... Interfaces>
class IntfObjectImpl : public ObjInstance<Interfaces...>
{
};

// [api(module)]
template <typename... Interfaces>
class IntfObjectSupportsWeakRefImpl : public ObjInstanceSupportsWeakRef<Interfaces...>
{
};

template <typename... Interfaces>
using ImplementationOf = IntfObjectImpl<Interfaces..., IInspectable>;

template <typename TInstance, typename TReturn, typename... TArgs>
auto event(TInstance* const obj, TReturn (TInstance::*func)(TArgs...) const)
{
    return delegate<TReturn(TArgs...)>(obj, func);
}

template <typename TInstance, typename TReturn, typename... TArgs>
auto event(TInstance* const obj, TReturn (TInstance::*func)(TArgs...))
{
    return delegate<TReturn(TArgs...)>(obj, func);
}

END_NAMESPACE_OPENDAQ
