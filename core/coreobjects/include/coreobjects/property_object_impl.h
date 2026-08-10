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
#include <coreobjects/core_event_args_factory.h>
#include <coreobjects/end_update_event_args_factory.h>
#include <coreobjects/eval_value_ptr.h>
#include <coreobjects/exceptions.h>
#include <coreobjects/object_keys.h>
#include <coreobjects/ownable_ptr.h>
#include <coreobjects/permission_manager_factory.h>
#include <coreobjects/permission_manager_internal_ptr.h>
#include <coreobjects/permission_mask_builder_factory.h>
#include <coreobjects/permissions_builder_factory.h>
#include <coreobjects/property_factory.h>
#include <coreobjects/property_internal_ptr.h>
#include <coreobjects/property_object.h>
#include <coreobjects/property_object_class_ptr.h>
#include <coreobjects/property_object_factory.h>
#include <coreobjects/property_object_internal_ptr.h>
#include <coreobjects/property_object_protected.h>
#include <coreobjects/property_object_protected_ptr.h>
#include <coreobjects/property_object_ptr.h>
#include <coreobjects/property_ptr.h>
#include <coreobjects/property_value_event_args_factory.h>
#include <coreobjects/object_lock_guard_ptr.h>
#include <coretypes/cloneable.h>
#include <coretypes/coretypes.h>
#include <coretypes/enumeration_factory.h>
#include <coretypes/type_manager_ptr.h>
#include <coretypes/updatable.h>
#include <coretypes/validation.h>
#include <tsl/ordered_map.h>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <utility>
#include <coretypes/recursive_search_ptr.h>
#include <coreobjects/property_object_core.h>
#include <coreobjects/mutex_factory.h>
#include <coreobjects/mutex_impl.h>
#include <coreobjects/property_object_utils.h>
#include <coreobjects/property_object_helpers.h>

BEGIN_NAMESPACE_OPENDAQ

using PropertyOrderedMap = tsl::ordered_map<StringPtr, PropertyPtr, StringHash, StringEqualTo>;

namespace config_protocol
{
    class ConfigClientDeviceInfoImpl;
    
    template <typename Impl>
    class ConfigClientPropertyObjectBaseImpl;
}

template <typename PropObjInterface, typename... Interfaces>
class GenericPropertyObjectImpl : public ImplementationOfWeak<PropObjInterface,
                                                              IOwnable,
                                                              IFreezable,
                                                              ISerializable,
                                                              IUpdatable,
                                                              IPropertyObjectProtected,
                                                              IPropertyObjectInternal,
                                                              Interfaces...>
{
public:
    explicit GenericPropertyObjectImpl();
    explicit GenericPropertyObjectImpl(const TypeManagerPtr& manager,
                                       const StringPtr& className,
                                       const ProcedurePtr& triggerCoreEvent = nullptr);

    virtual ErrCode INTERFACE_FUNC getClassName(IString** className) override;

    virtual ErrCode INTERFACE_FUNC setPropertyValue(IString* propertyName, IBaseObject* value) override;
    virtual ErrCode INTERFACE_FUNC setPropertyValueNoLock(IString* propertyName, IBaseObject* value) override;
    virtual ErrCode INTERFACE_FUNC getPropertyValue(IString* propertyName, IBaseObject** value) override;
    virtual ErrCode INTERFACE_FUNC getPropertyValueNoLock(IString* propertyName, IBaseObject** value) override;
    virtual ErrCode INTERFACE_FUNC getPropertySelectionValue(IString* propertyName, IBaseObject** value) override;
    virtual ErrCode INTERFACE_FUNC getPropertySelectionValueNoLock(IString* propertyName, IBaseObject** value) override;
    virtual ErrCode INTERFACE_FUNC setPropertySelectionValue(IString* propertyName, IBaseObject* value) override;
    virtual ErrCode INTERFACE_FUNC clearPropertyValue(IString* propertyName) override;
    virtual ErrCode INTERFACE_FUNC clearPropertyValueNoLock(IString* propertyName) override;
    virtual ErrCode INTERFACE_FUNC clearPropertyValues() override;

    virtual ErrCode INTERFACE_FUNC hasProperty(IString* propertyName, Bool* hasProperty) override;
    virtual ErrCode INTERFACE_FUNC getProperty(IString* propertyName, IProperty** property) override;
    virtual ErrCode INTERFACE_FUNC addProperty(IProperty* property) override;
    virtual ErrCode INTERFACE_FUNC removeProperty(IString* propertyName) override;

    virtual ErrCode INTERFACE_FUNC getVisibleProperties(IList** properties) override;
    virtual ErrCode INTERFACE_FUNC getAllProperties(IList** properties) override;

    virtual ErrCode INTERFACE_FUNC setPropertyOrder(IList* orderedPropertyNames) override;

    virtual ErrCode INTERFACE_FUNC getOnPropertyValueWrite(IString* propertyName, IEvent** event) override;
    virtual ErrCode INTERFACE_FUNC getOnPropertyValueRead(IString* propertyName, IEvent** event) override;
    virtual ErrCode INTERFACE_FUNC getOnAnyPropertyValueWrite(IEvent** event) override;
    virtual ErrCode INTERFACE_FUNC getOnAnyPropertyValueRead(IEvent** event) override;

    virtual ErrCode INTERFACE_FUNC beginUpdate() override;
    virtual ErrCode INTERFACE_FUNC endUpdate() override;
    virtual ErrCode INTERFACE_FUNC getUpdating(Bool* updating) override;

    virtual ErrCode INTERFACE_FUNC getOnEndUpdate(IEvent** event) override;
    virtual ErrCode INTERFACE_FUNC getPermissionManager(IPermissionManager** permissionManager) override;
    virtual ErrCode INTERFACE_FUNC findProperties(IList** properties, ISearchFilter* propertyFilter, ISearchFilter* componentFilter = nullptr) override;

    // IPropertyObjectInternal
    virtual ErrCode INTERFACE_FUNC checkForReferences(IProperty* property, Bool* isReferenced) override;
    virtual ErrCode INTERFACE_FUNC checkForReferencesNoLock(IProperty* property, Bool* isReferenced) override;
    virtual ErrCode INTERFACE_FUNC enableCoreEventTrigger() override;
    virtual ErrCode INTERFACE_FUNC disableCoreEventTrigger() override;
    virtual ErrCode INTERFACE_FUNC getCoreEventTrigger(IProcedure** trigger) override;
    virtual ErrCode INTERFACE_FUNC setCoreEventTrigger(IProcedure* trigger) override;
    virtual ErrCode INTERFACE_FUNC clone(IPropertyObject** cloned) override;
    virtual ErrCode INTERFACE_FUNC setPath(IString* path) override;
    virtual ErrCode INTERFACE_FUNC getPath(IString** path) override;
    virtual ErrCode INTERFACE_FUNC isUpdating(Bool* updating) override;
    virtual ErrCode INTERFACE_FUNC hasUserReadAccess(IBaseObject* userContext, Bool* hasAccessOut) override;
    virtual ErrCode INTERFACE_FUNC getLockGuard(ILockGuard** lockGuard) override;
    virtual ErrCode INTERFACE_FUNC getRecursiveLockGuard(ILockGuard** lockGuard) override;
    virtual ErrCode INTERFACE_FUNC setLockingStrategy(LockingStrategy strategy) override;
    virtual ErrCode INTERFACE_FUNC getLockingStrategy(LockingStrategy* strategy) override;
    virtual ErrCode INTERFACE_FUNC getMutex(IMutex** mutex) override;
    virtual ErrCode INTERFACE_FUNC getMutexOwner(IPropertyObjectInternal** owner) override;

    // IUpdatable
    virtual ErrCode INTERFACE_FUNC updateInternal(ISerializedObject* obj, IBaseObject* context) override;
    virtual ErrCode INTERFACE_FUNC update(ISerializedObject* obj, IBaseObject* config) override;
    virtual ErrCode INTERFACE_FUNC serializeForUpdate(ISerializer* serializer) override;
    virtual ErrCode INTERFACE_FUNC updateEnded(IBaseObject* context) override;

    // ISerializable
    virtual ErrCode INTERFACE_FUNC serialize(ISerializer* serializer) override;
    virtual ErrCode INTERFACE_FUNC getSerializeId(ConstCharPtr* id) const override;

    virtual ErrCode INTERFACE_FUNC toString(CharPtr* str) override;

    static ConstCharPtr SerializeId();
    static ErrCode Deserialize(ISerializedObject* serialized, IBaseObject* context, IFunction* factoryCallback, IBaseObject** obj);

    // IOwnable
    virtual ErrCode INTERFACE_FUNC setOwner(IPropertyObject* newOwner) override;

    [[nodiscard]]
    WeakRefPtr<IPropertyObject> getOwner() const;

    // IFreezable
    virtual ErrCode INTERFACE_FUNC freeze() override;
    virtual ErrCode INTERFACE_FUNC isFrozen(Bool* isFrozen) const override;

    // IPropertyObjectProtected
    virtual ErrCode INTERFACE_FUNC setProtectedPropertyValue(IString* propertyName, IBaseObject* value) override;
    virtual ErrCode INTERFACE_FUNC setProtectedPropertyValueNoLock(IString* propertyName, IBaseObject* value) override;
    virtual ErrCode INTERFACE_FUNC setProtectedPropertySelectionValue(IString* propertyName, IBaseObject* value) override;
    virtual ErrCode INTERFACE_FUNC clearProtectedPropertyValue(IString* propertyName) override;
    virtual ErrCode INTERFACE_FUNC clearProtectedPropertyValues() override;
    
    using PropertyValueEventEmitter = EventEmitter<PropertyObjectPtr, PropertyValueEventArgsPtr>;
    using EndUpdateEventEmitter = EventEmitter<PropertyObjectPtr, EndUpdateEventArgsPtr>;

    struct CloneParameters
    {
        const std::unordered_map<StringPtr, PropertyValueEventEmitter>& valueWriteEvents;
        const std::unordered_map<StringPtr, PropertyValueEventEmitter>& valueReadEvents;
        const EndUpdateEventEmitter& endUpdateEvent;
        const ProcedurePtr& triggerCoreEvent;
        const PropertyOrderedMap& localProperties;
        const std::unordered_map<StringPtr, BaseObjectPtr, StringHash, StringEqualTo>& propValues;
        const std::vector<StringPtr>& customOrder;
        const PermissionManagerPtr& permissionManager;
        const std::set<StringPtr>& corePropertyNames;
    };

    void configureClonedMembers(const CloneParameters& parameters);
    void configureClonedMembers(const std::unordered_map<StringPtr, PropertyValueEventEmitter>& valueWriteEvents,
                                const std::unordered_map<StringPtr, PropertyValueEventEmitter>& valueReadEvents,
                                const EndUpdateEventEmitter& endUpdateEvent,
                                const ProcedurePtr& triggerCoreEvent,
                                const PropertyOrderedMap& localProperties,
                                const std::unordered_map<StringPtr, BaseObjectPtr, StringHash, StringEqualTo>& propValues,
                                const std::vector<StringPtr>& customOrder,
                                const PermissionManagerPtr& permissionManager,
                                const std::set<StringPtr>& corePropertyNames);
      
    // TODO: Make remove friend classes once private methods are properly exposed in protected scope.
    template <typename TInterface, typename... TInterfaces>
    friend class DeviceInfoConfigImpl;
    friend class config_protocol::ConfigClientDeviceInfoImpl;

    template <class Impl>
    friend class config_protocol::ConfigClientPropertyObjectBaseImpl;

protected:
    struct UpdatingAction
    {
        bool setValue;
        bool protectedAccess;
        BaseObjectPtr value;
    };

    // Using vector to preserve write order when the same property is changed twice within an update
    using UpdatingActions = std::vector<std::pair<std::string, UpdatingAction>>;

    /**
     * Gets a lock for the configuration of the object. Can be used to lock the sync mutex in a function
     * that is called during a property value read/write event to prevent deadlocks. The lock behaves
     * similarly to a lock guard created with recursive mutex.
     */
    std::unique_ptr<RecursiveConfigLockGuard> getRecursiveConfigLock();
    
    /**
     * Gets a lock to be used in the data acquisition loop, or in other performance-critical parts of
     * a module implementation. The lock is not recursive in comparison to the config lock and should
     * be used with caution to prevent deadlocks.
     *
     * WARNING: Only usable if locking strategy is set to `OwnLock` or `ForwardOwnerLockOwn`.
     */
    std::lock_guard<std::mutex> getAcquisitionLock();
    
    /**
     * Gets a unique lock wrapping the object's mutex.
     *
     * WARNING: Only usable if locking strategy is set to `OwnLock` or `ForwardOwnerLockOwn`.
     */
    std::unique_lock<std::mutex> getUniqueLock();

    /**
     * Gets a lock for the configuration of the object. Can be used to lock the sync mutex in a function
     * that is called during a property value read/write event to prevent deadlocks. The lock behaves
     * similarly to a lock guard created with recursive mutex.
     *
     * Wraps the mutex of the closest owner/parent if the locking strategy is set to `InheritLock`.
     */
    LockGuardPtr getRecursiveConfigLock2();
    
    /**
     * Gets a lock to be used in the data acquisition loop, or in other performance-critical parts of
     * a module implementation. The lock is not recursive in comparison to the config lock and should
     * be used with caution to prevent deadlocks.
     *
     * Wraps the mutex of the closest owner/parent if the locking strategy is set to `InheritLock`.
     */
    std::lock_guard<MutexPtr> getAcquisitionLock2();
    
    /**
     * Gets a unique lock wrapping the object's mutex.
     *
     * Wraps the mutex of the closest owner/parent if the locking strategy is set to `InheritLock`.
     */
    std::unique_lock<MutexPtr> getUniqueLock2();
    
    PropertyObjectPtr objPtr;
    std::atomic<bool> coreEventMuted;

    bool isFrozen();
    void unfreeze();
    StringPtr getPath() const;
    TypeManagerPtr getTypeManager();
    CloneParameters getCloneParameters();

    void setMutex(const MutexPtr& mutex);
    void setLockOwner(const PropertyObjectInternalPtr& owner);
    void internalDispose(bool) override;
    ErrCode setPropertyOrderInternal(IList* orderedPropertyNames, bool isUpdating = false);

    // Serialization

    virtual ErrCode serializeCustomValues(ISerializer* serializer, bool forUpdate);
    virtual ErrCode serializePropertyValue(const StringPtr& name, const ObjectPtr<IBaseObject>& value, ISerializer* serializer, bool forUpdate = false);
    virtual ErrCode serializeProperty(const PropertyPtr& property, ISerializer* serializer);

    // Update
    virtual void endApplyUpdate();
    virtual void beginApplyUpdate();
    virtual void beginApplyProperties(const UpdatingActions& propsAndValues, bool parentUpdating);
    virtual void endApplyProperties(const UpdatingActions& propsAndValues, bool parentUpdating);
    bool isParentUpdating();
    virtual void onUpdatableUpdateEnd(const BaseObjectPtr& context);

    template <class F>
    static PropertyObjectPtr DeserializePropertyObject(const SerializedObjectPtr& serialized,
                                                       const BaseObjectPtr& context,
                                                       const FunctionPtr& factoryCallback,
                                                       F&& f);

    virtual void callBeginUpdateOnChildren();
    virtual void callEndUpdateOnChildren();

    // Invokes `handler` on every non-frozen child property object stored in `propValues`
    template <typename Handler>
    void forEachUnfrozenChildObject(Handler&& handler)
    {
        for (const auto& [_, propValue] : propValues)
        {
            const auto propObj = propValue.template asPtrOrNull<IPropertyObject>(true);
            if (!propObj.assigned())
                continue;

            const auto freezable = propObj.template asPtrOrNull<IFreezable>(true);
            if (freezable.assigned() && freezable.isFrozen())
                continue;

            handler(propObj);
        }
    }

    virtual PropertyObjectPtr getPropertyObjectParent();
    virtual PropertyObjectPtr cloneChildPropertyObject(const PropertyPtr& prop);

    ErrCode addCoreProperty(IProperty* property);

private:
    ObjectPtr<IPropertyObjectCore> propObjCore;

    // Mutex that is locked in the getRecursiveConfigLock and getAcquisitionLock methods. Those should
    // be used instead of locking this mutex directly unless a different type of lock is needed.
    MutexPtr sync;
    WeakRefPtr<IPropertyObjectInternal> lockOwner;
    std::mutex* getLocalMutex();
    LockingStrategy lockingStrategy;

    StringPtr className;
    PropertyObjectClassPtr objectClass;
    
    const std::string AnyReadEventName = "DAQ_AnyReadEvent";
    const std::string AnyWriteEventName = "DAQ_AnyWriteEvent";

    std::unordered_map<StringPtr, PropertyValueEventEmitter> valueWriteEvents;
    std::unordered_map<StringPtr, PropertyValueEventEmitter> valueReadEvents;
    EndUpdateEventEmitter endUpdateEvent;
    ProcedurePtr triggerCoreEvent;

    PropertyUpdateStack updatePropertyStack;

    std::unordered_map<StringPtr, BaseObjectPtr, StringHash, StringEqualTo> propValues;
    PropertyOrderedMap localProperties;
    std::set<StringPtr> corePropertyNames;

    WeakRefPtr<IPropertyObject> owner;
    int updateCount;
    UpdatingActions batchedUpdates;
    WeakRefPtr<ITypeManager> manager;
    std::vector<StringPtr> customOrder;
    StringPtr path;
    PermissionManagerPtr permissionManager;
    bool frozen;

    ErrCode setPropertyValueInternal(IString* name, IBaseObject* value, bool triggerEvent, bool protectedAccess, bool batch, bool isUpdating = false);
    ErrCode setPropertySelectionValueInternal(IString* propertyName, IBaseObject* value, bool protectedAccess);
    ErrCode clearPropertyValueInternal(IString* name, bool protectedAccess, bool batch, bool isUpdating = false);
    ErrCode clearPropertyValuesInternal(bool protectedAccess);
    ErrCode getPropertyValueInternal(IString* propertyName, IBaseObject** value, Bool retrieveUpdatingValue = false);
    ErrCode getPropertySelectionValueInternal(IString* propertyName, IBaseObject** value, Bool retrieveUpdatingValue = false);
    ErrCode checkForReferencesInternal(IProperty* property, Bool* isReferenced);

    static void DeserializePropertyValues(const SerializedObjectPtr& serialized,
                                          const BaseObjectPtr& context,
                                          const FunctionPtr& factoryCallback,
                                          PropertyObjectPtr& propObjPtr);

    static void DeserializeLocalProperties(const SerializedObjectPtr& serialized,
                                           const BaseObjectPtr& context,
                                           const FunctionPtr& factoryCallback,
                                           PropertyObjectPtr& propObjPtr);

    static void DeserializePropertyOrder(const SerializedObjectPtr& serialized,
                                         const BaseObjectPtr& context,
                                         const FunctionPtr& factoryCallback,
                                         PropertyObjectPtr& propObjPtr);

    static ErrCode setPropertyFromSerialized(const StringPtr& propName,
                                             const PropertyObjectPtr& propObj,
                                             const SerializedObjectPtr& serialized,
                                             const TypeManagerPtr& typeManager);

    ErrCode serializePropertyValues(ISerializer* serializer, bool forUpdate = false);
    ErrCode serializeLocalProperties(ISerializer* serializer);

    // Does not bind property to object and does not look up reference property
    PropertyPtr getUnboundPropertyOrNull(const StringPtr& name) const;

    // True if `value` differs from the property's default value (or the property cannot be resolved)
    bool differsFromDefaultValue(const StringPtr& name, const BaseObjectPtr& value) const;
    bool shouldWriteLocalValue(const StringPtr& name, const BaseObjectPtr& value) const;
    // Adds the value to the local list of values (`propValues`)
    bool writeLocalValue(const StringPtr& name, const BaseObjectPtr& value, bool forceWrite = false);


    static bool checkIsChildObjectProperty(const PropertyPtr& prop);
    void setChildPropertyObject(const StringPtr& propName, const PropertyObjectPtr& cloned);
    void configureClonedObj(const StringPtr& objPropName, const PropertyObjectPtr& obj);

    void triggerCoreEventInternal(const CoreEventArgsPtr& args);

    // Looks up the property and resolves it if it is a reference property. Outputs the bound property,
    // the effective name under which its value is stored, and the bracket ("[N]") suffix of `name`, if any.
    // `bracket` points into the buffer of `name` and is only valid while `name` is alive.
    ErrCode getBoundPropertyInternal(const StringPtr& name, PropertyPtr& property, StringPtr& effectiveName, ConstCharPtr& bracket);
    // Reads the current value of a property bound via `getBoundPropertyInternal`: the in-progress updating
    // value, the locally stored value, or the property default. Does not trigger read events.
    ErrCode readPropertyValueInternal(const PropertyPtr& property, const StringPtr& effectiveName, ConstCharPtr bracket, bool retrieveUpdatingValue, BaseObjectPtr& value);
    // Convenience overload: binds the property by name, then reads its value
    ErrCode readPropertyValueInternal(const StringPtr& name, bool retrieveUpdatingValue, PropertyPtr& property, BaseObjectPtr& value);
    ErrCode getPropertiesInternal(Bool includeInvisible, Bool bind, IList** list, Bool includeCoreProperties = false);

    // Gets the property value, if stored in local value dictionary (propValues)
    // Parses brackets, if the property is a list
    ErrCode readLocalValue(const StringPtr& name, BaseObjectPtr& value) const;

    // Called when `setPropertyValue` successfully sets a new value
    [[maybe_unused]]
    ErrCode callPropertyValueWrite(const PropertyPtr& prop, 
                                   BaseObjectPtr& newValue, 
                                   PropertyEventType changeType, 
                                   bool isUpdating);

    // Called at the end of `getPropertyValue`
    BaseObjectPtr callPropertyValueRead(const PropertyPtr& prop, const BaseObjectPtr& readValue);

    // Shared implementation of getOnPropertyValueWrite/getOnPropertyValueRead
    ErrCode getPropertyValueEventInternal(IString* propertyName, IEvent** event, bool valueWrite);

    // Sets `this` as owner of `value`, if `value` is ownable
    void setOwnerToPropertyValue(const BaseObjectPtr& value);

    // Child property handling - Used when a property is queried in the "parent.child" format.
    // Iteratively walks the dot-separated path and returns the object that owns the leaf property.
    // For "a.b.c" returns the object at "a.b" and sets leafName to "c". Only valid for child paths.
    ErrCode getParentObject(const StringPtr& path, PropertyObjectPtr& parentObj, StringPtr& leafName);


    // Update
    ErrCode updateObjectProperties(const PropertyObjectPtr& propObj,
                                   const SerializedObjectPtr& serialized,
                                   const ListPtr<IProperty>& props);

    ErrCode beginUpdateInternal(bool deep);
    ErrCode endUpdateInternal(bool deep);
    ErrCode getUpdatingInternal(Bool* updating);

    static bool hasUserReadAccess(const BaseObjectPtr& userContext, const BaseObjectPtr& obj);

    ErrCode addPropertyInternal(IProperty* property, bool isCoreProperty = false);

};

using PropertyObjectImpl = GenericPropertyObjectImpl<IPropertyObject>;

template <class PropObjInterface, class... Interfaces>
GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::GenericPropertyObjectImpl()
    : coreEventMuted(true)
    , sync(Mutex())
    , lockingStrategy(LockingStrategy::OwnLock)
    , className(nullptr)
    , objectClass(nullptr)
    , updateCount(0)
    , path("")
    , frozen(false)
{
    this->internalAddRef();
    objPtr = this->template borrowPtr<PropertyObjectPtr>();
    propObjCore = PropertyObjectCore_Create();

    setLockOwner(owner);
    this->permissionManager = PermissionManager();
    this->permissionManager.setPermissions(object_utils::UnrestrictedPermissions);

    PropertyValueEventEmitter readEmitter;
    PropertyValueEventEmitter writeEmitter;
    valueReadEvents.emplace(AnyReadEventName, readEmitter);
    valueWriteEvents.emplace(AnyWriteEventName, writeEmitter);
}

template <typename PropObjInterface, typename... Interfaces>
GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::GenericPropertyObjectImpl(const TypeManagerPtr& manager,
                                                                                      const StringPtr& className,
                                                                                      const ProcedurePtr& triggerCoreEvent)
    : GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::GenericPropertyObjectImpl()
{
    this->triggerCoreEvent = triggerCoreEvent;
    this->manager = manager;

    if (className.assigned() && className != "")
    {
        this->className = className;
        if (!manager.assigned())
            DAQ_THROW_EXCEPTION(ManagerNotAssignedException);

        const TypePtr type = manager.getType(className);

        if (!type.assigned())
            DAQ_THROW_EXCEPTION(NotFoundException, "Class with name {} is not available in module manager", className);

        const auto objClass = type.asPtrOrNull<IPropertyObjectClass>();
        if (!objClass.assigned())
            DAQ_THROW_EXCEPTION(InvalidTypeException, "Type with name {} is not a property object class", className);

        objectClass = objClass;

        for (const auto& prop : objectClass.getProperties(true))
        {
            if (checkIsChildObjectProperty(prop))
            {
                setChildPropertyObject(prop.getName(), cloneChildPropertyObject(prop));
            }
        }
    }
}

template <typename PropObjInterface, typename ... Interfaces>
bool GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::isFrozen()
{
    return frozen;
}

template <typename PropObjInterface, typename ... Interfaces>
void GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::unfreeze()
{
    frozen = false;
}

template <typename PropObjInterface, typename ... Interfaces>
StringPtr GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::getPath() const
{
    return path;
}

template <typename PropObjInterface, typename ... Interfaces>
TypeManagerPtr GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::getTypeManager()
{
    return manager.getRef();
}

template <typename PropObjInterface, typename... Interfaces>
typename GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::CloneParameters GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::getCloneParameters()
{
    return CloneParameters{
        valueWriteEvents,
        valueReadEvents,
        endUpdateEvent,
        triggerCoreEvent,
        localProperties,
        propValues,
        customOrder,
        permissionManager,
        corePropertyNames
    };
}

template <typename PropObjInterface, typename ... Interfaces>
void GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::setMutex(const MutexPtr& mutex)
{
    this->sync = mutex;
}

template <typename PropObjInterface, typename ... Interfaces>
void GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::setLockOwner(const PropertyObjectInternalPtr& owner)
{
    this->lockOwner = owner;
}

template <class PropObjInterface, class... Interfaces>
void GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::internalDispose(bool)
{
    for (auto& [_, value] : propValues)
    {
        if (value.assigned())
        {
            OwnablePtr ownablePtr = value.template asPtrOrNull<IOwnable>(true);
            if (ownablePtr.assigned())
                ownablePtr.setOwner(nullptr);
        }
    }
    propValues.clear();

    owner.release();
    className.release();
    objectClass.release();
}

template <class PropObjInterface, class... Interfaces>
ErrCode GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::getClassName(IString** className)
{
    OPENDAQ_PARAM_NOT_NULL(className);

    if (this->className.assigned())
    {
        *className = this->className.addRefAndReturn();
    }
    else
    {
        *className = String("").detach();
    }

    return OPENDAQ_SUCCESS;
}

template <typename PropObjInterface, typename... Interfaces>
ErrCode GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::getParentObject(const StringPtr& path,
                                                                                    PropertyObjectPtr& parentObj,
                                                                                    StringPtr& leafName)
{
    StringPtr parentPath;
    details::splitOnLastDot(path, parentPath, leafName);

    const std::string parentPathStr = parentPath;
    PropertyObjectPtr current;
    size_t start = 0;

    while (true)
    {
        const size_t pos = parentPathStr.find('.', start);
        const SizeT segmentLength = (pos == std::string::npos ? parentPathStr.size() : pos) - start;
        const StringPtr segment = String(parentPathStr.c_str() + start, segmentLength);

        BaseObjectPtr childValue;
        const ErrCode err = current.assigned() ? current->getPropertyValue(segment, &childValue)
                                               : getPropertyValueInternal(segment, &childValue);
        OPENDAQ_RETURN_IF_FAILED(err);

        current = childValue.template asPtrOrNull<IPropertyObject>(true);
        if (!current.assigned())
            return DAQ_MAKE_ERROR_INFO(OPENDAQ_ERR_NOINTERFACE, fmt::format(R"(Property "{}" is not a property object)", segment));

        if (pos == std::string::npos)
            break;
        start = pos + 1;
    }

    parentObj = current;
    return OPENDAQ_SUCCESS;
}

template <class PropObjInterface, class... Interfaces>
ErrCode GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::callPropertyValueWrite(const PropertyPtr& prop,
                                                                                           BaseObjectPtr& newValue,
                                                                                           PropertyEventType changeType,
                                                                                           bool isUpdating)
{
    const auto name = prop.getName();
    const auto defaultValue = prop.getDefaultValue();

    if (!updatePropertyStack.registerPropertyUpdating(name, newValue))
        return OPENDAQ_IGNORED;

    const bool isBaseStackLevel = updatePropertyStack.isBaseStackLevel(name);
    if (isBaseStackLevel)
    {
        if (newValue.assigned() && !shouldWriteLocalValue(name, newValue))
        {
            updatePropertyStack.unregisterPropertyUpdating(name);
            return OPENDAQ_IGNORED;
        }
    }

    BaseObjectPtr oldValue;
    ErrCode errCode = readLocalValue(name, oldValue);
    if (errCode == OPENDAQ_ERR_NOTFOUND)
    {
        daqClearErrorInfo();
        oldValue = defaultValue;
    }
    errCode = OPENDAQ_SUCCESS;

    PropertyValueEventArgsPtr args;
    if (changeType == PropertyEventType::Clear)
        args = PropertyValueEventArgs(prop, defaultValue, oldValue, changeType, isUpdating);
    else
        args = PropertyValueEventArgs(prop, newValue, oldValue, changeType, isUpdating);

    if (!localProperties.count(name))
    {
        const PropertyValueEventEmitter propEvent{prop.asPtr<IPropertyInternal>(true).getClassOnPropertyValueWrite()};
        if (propEvent.hasListeners())
            propEvent(objPtr, args);
    }

    if (valueWriteEvents.find(name) != valueWriteEvents.end())
    {
        if (valueWriteEvents[name].hasListeners())
            errCode = daqTry([&] { valueWriteEvents[name](objPtr, args); });
    }

    if (valueWriteEvents[AnyWriteEventName].hasListeners())
    {
        valueWriteEvents[AnyWriteEventName](objPtr, args);
    }

    bool shouldUpdate = updatePropertyStack.unregisterPropertyUpdating(name);
    // If the event execution failed, forward the error code
    OPENDAQ_RETURN_IF_FAILED(errCode);

    if (shouldUpdate)
    {
        // setting the final value is only done in the top level of the stack
        if (changeType == PropertyEventType::Clear && args.getValue() == defaultValue)
            return OPENDAQ_SUCCESS;

        if (newValue == args.getValue())
            return OPENDAQ_SUCCESS;
        
        // if the value changed, we have to validate new value before setting it
        newValue = args.getValue();
        return setPropertyValueInternal(name, newValue, false, true, false);
    }
    return OPENDAQ_IGNORED;
}

template <typename PropObjInterface, typename... Interfaces>
BaseObjectPtr GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::callPropertyValueRead(const PropertyPtr& prop,
                                                                                                const BaseObjectPtr& readValue)
{
    if (!prop.assigned())
    {
        return readValue;
    }

    auto args = PropertyValueEventArgs(prop, readValue, readValue, PropertyEventType::Read, False);

    if (!localProperties.count(prop.getName()))
    {
        const PropertyValueEventEmitter propEvent{prop.asPtr<IPropertyInternal>().getClassOnPropertyValueRead()};
        if (propEvent.hasListeners())
        {
            propEvent(objPtr, args);
        }
    }

    const auto name = prop.getName();
    if (valueReadEvents.find(name) != valueReadEvents.end())
    {
        if (valueReadEvents[name].hasListeners())
        {
            valueReadEvents[name](objPtr, args);
        }
    }

    if (valueReadEvents[AnyReadEventName].hasListeners())
    {
        valueReadEvents[AnyReadEventName](objPtr, args);
    }

    return args.getValue();
}

template <class PropObjInterface, typename... Interfaces>
ErrCode GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::setProtectedPropertyValue(IString* propertyName, IBaseObject* value)
{
    auto lock = getRecursiveConfigLock2();
    return setPropertyValueInternal(propertyName, value, true, true, updateCount > 0);
}

template <typename PropObjInterface, typename ... Interfaces>
ErrCode GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::setProtectedPropertyValueNoLock(IString* propertyName, IBaseObject* value)
{
    return setPropertyValueInternal(propertyName, value, true, true, updateCount > 0);
}

template <class PropObjInterface, class... Interfaces>
ErrCode GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::setPropertyValue(IString* propertyName, IBaseObject* value)
{
    auto lock = getRecursiveConfigLock2();
    return setPropertyValueNoLock(propertyName, value);
}

template <typename PropObjInterface, typename... Interfaces>
ErrCode GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::setPropertyValueNoLock(IString* propertyName, IBaseObject* value)
{
    return setPropertyValueInternal(propertyName, value, true, false, updateCount > 0);
}

template <class PropObjInterface, typename... Interfaces>
ErrCode GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::setPropertyValueInternal(IString* name,
                                                                                             IBaseObject* value,
                                                                                             bool triggerEvent,
                                                                                             bool protectedAccess,
                                                                                             bool batch,
                                                                                             bool isUpdating)
{
    OPENDAQ_PARAM_NOT_NULL(name);
    OPENDAQ_PARAM_NOT_NULL(value);

    if (frozen)
        return DAQ_MAKE_ERROR_INFO(OPENDAQ_ERR_FROZEN);

    auto propName = StringPtr::Borrow(name);
    auto valuePtr = BaseObjectPtr::Borrow(value);

    const ErrCode errCode = daqTry([&]()
    {
        const auto isChildProp = details::isChildProperty(propName);

        if (batch && !isChildProp)
        {
            batchedUpdates.emplace_back(std::make_pair(propName, UpdatingAction{true, protectedAccess, valuePtr}));
            return OPENDAQ_SUCCESS;
        }

        if (isChildProp)
        {
            PropertyObjectPtr parentObj;
            StringPtr leafName;
            OPENDAQ_RETURN_IF_FAILED(getParentObject(propName, parentObj, leafName));

            if (protectedAccess)
            {
                const auto parentObjProtected = parentObj.template asPtr<IPropertyObjectProtected>(true);
                parentObjProtected.setProtectedPropertyValue(leafName, valuePtr);
            }
            else
            {
                parentObj.setPropertyValue(leafName, valuePtr);
            }

            return OPENDAQ_SUCCESS;
        }

        PropertyPtr prop = getUnboundPropertyOrNull(propName);
        prop = details::checkForRefPropAndGetBoundProp(prop, objPtr);

        if (!prop.assigned())
            return DAQ_MAKE_ERROR_INFO(OPENDAQ_ERR_NOTFOUND, fmt::format(R"(Property "{}" does not exist)", propName));

        propName = prop.getName();

        const auto propInternal = prop.asPtr<IPropertyInternal>();
        // TODO: If function type, check if return value is correct type.
        if (!protectedAccess)
        {
            if (propInternal.getReadOnlyNoLock() || propInternal.getValueTypeNoLock() == ctObject)
            {
                return DAQ_MAKE_ERROR_INFO(OPENDAQ_ERR_ACCESSDENIED, fmt::format(R"(Property "{}" is read only)", propName));
            }
        }

        OPENDAQ_RETURN_IF_FAILED(details::checkPropertyTypeAndConvert(prop, valuePtr));
        OPENDAQ_RETURN_IF_FAILED(details::checkContainerType(prop, valuePtr));
        OPENDAQ_RETURN_IF_FAILED(details::checkSelectionValues(prop, valuePtr));
        OPENDAQ_RETURN_IF_FAILED(details::checkStructType(prop, valuePtr));
        OPENDAQ_RETURN_IF_FAILED(details::checkEnumerationType(prop, valuePtr));

        details::coercePropertyWrite(prop, valuePtr, objPtr);
        details::validatePropertyWrite(prop, valuePtr, objPtr);
        details::coerceMinMax(prop, valuePtr);

        const auto ct = propInternal.getValueTypeNoLock();
        if (ct == ctList || ct == ctDict)
        {
            BaseObjectPtr clonedValue;
            OPENDAQ_RETURN_IF_FAILED(valuePtr.asPtr<ICloneable>()->clone(&clonedValue));

            valuePtr = clonedValue.detach();
        }
        else if (ct == ctObject)
        {
            configureClonedObj(propName, valuePtr);
        }

        if (triggerEvent)
        {
            BaseObjectPtr newValue = valuePtr;
            ErrCode err = callPropertyValueWrite(prop, newValue, PropertyEventType::Update, isUpdating);
            OPENDAQ_RETURN_IF_FAILED(err);

            if (err == OPENDAQ_IGNORED)
                return OPENDAQ_SUCCESS;

            if (valuePtr == newValue)
            {
                writeLocalValue(propName, newValue);
                setOwnerToPropertyValue(newValue);
            }

            if (!isUpdating)
                triggerCoreEventInternal(CoreEventArgsPropertyValueChanged(objPtr, propName, newValue, path));
        }
        else
        {
            if (!writeLocalValue(propName, valuePtr))
                return OPENDAQ_IGNORED;
            setOwnerToPropertyValue(valuePtr);
        }

        return OPENDAQ_SUCCESS;
    });

    OPENDAQ_RETURN_IF_FAILED(errCode, fmt::format(R"(Failed to set property value "{}")", propName));
    return errCode;
}


template <class PropObjInterface, class... Interfaces>
bool GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::differsFromDefaultValue(const StringPtr& name, const BaseObjectPtr& value) const
{
    try
    {
        return objPtr.getProperty(name).template asPtr<IPropertyInternal>().getDefaultValueNoLock() != value;
    }
    catch (...)
    {
    }
    return true;
}

template <class PropObjInterface, class... Interfaces>
bool GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::shouldWriteLocalValue(const StringPtr& name, const BaseObjectPtr& value) const
{
    const auto it = propValues.find(name);
    if (it != propValues.end())
        return it->second != value;

    return differsFromDefaultValue(name, value);
}

template <class PropObjInterface, class... Interfaces>
bool GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::writeLocalValue(const StringPtr& name, const BaseObjectPtr& value, bool forceWrite)
{
    const auto it = propValues.find(name);
    if (it != propValues.end())
    {
        if (it->second == value)
            return false;
        it->second = value;
        return true;
    }

    if (forceWrite || differsFromDefaultValue(name, value))
    {
        propValues.emplace(name, value);
        return true;
    }

    return false;
}

template <class PropObjInterface, class... Interfaces>
void GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::setOwnerToPropertyValue(const BaseObjectPtr& value)
{
    if (!value.assigned())
        return;

    auto ownablePtr = value.asPtrOrNull<IOwnable>(true);
    if (ownablePtr.assigned())
    {
        const ErrCode errCode = ownablePtr->setOwner(this->template borrowThis<GenericPropertyObjectPtr, IPropertyObject>());
        if (OPENDAQ_FAILED(errCode))
        {
            DAQ_EXTEND_ERROR_INFO(errCode, "Failed to set owner to property value");
            checkErrorInfo(errCode);
        }
    }
}

template <class PropObjInterface, class... Interfaces>
PropertyPtr GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::getUnboundPropertyOrNull(const StringPtr& name) const
{
    const auto res = localProperties.find(name);
    if (res != localProperties.cend())
        return res->second;

    if (objectClass == nullptr)
        return nullptr;

    PropertyPtr property;
    const auto errCode = objectClass->getProperty(name, &property);
    if (errCode == OPENDAQ_ERR_NOTFOUND)
    {
        daqClearErrorInfo();
        return nullptr;
    }

    checkErrorInfo(errCode);
    return property;
}

template <typename PropObjInterface, typename... Interfaces>
PropertyObjectPtr GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::cloneChildPropertyObject(const PropertyPtr& prop)
{
    const auto cloneable = prop.getDefaultValue().asPtrOrNull<IPropertyObjectInternal>();

    if (!cloneable.assigned())
        return nullptr;

    return cloneable.clone();
}

template <typename PropObjInterface, typename ... Interfaces>
bool GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::checkIsChildObjectProperty(const PropertyPtr& prop)
{
    const auto propPtrInternal = prop.asPtr<IPropertyInternal>();
    if (propPtrInternal.assigned() && propPtrInternal.getValueTypeUnresolved() == ctObject && prop.getDefaultValue().assigned())
    {
        const auto defaultValue = prop.getDefaultValue();
        const auto inspect = defaultValue.asPtrOrNull<IInspectable>();
        if (inspect.assigned() && !inspect.getInterfaceIds().empty() && !(inspect.getInterfaceIds()[0] == IPropertyObject::Id))
            DAQ_THROW_EXCEPTION(InvalidTypeException, "Only base Property Object object-type values are allowed");

        return true;
    }

    return false;
}

template <typename PropObjInterface, typename ... Interfaces>
void GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::setChildPropertyObject(const StringPtr& propName, const PropertyObjectPtr& cloned)
{
    writeLocalValue(propName, cloned, true);
    setOwnerToPropertyValue(cloned);
    configureClonedObj(propName, cloned);
}

template <typename PropObjInterface, typename... Interfaces>
void GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::configureClonedObj(const StringPtr& objPropName,
                                                                                    const PropertyObjectPtr& obj)
{
    if (!coreEventMuted)
    {
        if (const auto objInternal = obj.asPtrOrNull<IPropertyObjectInternal>(true); objInternal.assigned())
        {
            const auto childPath = path != "" ? path + "." + objPropName : objPropName;
            objInternal.setPath(childPath);
            objInternal.setCoreEventTrigger(triggerCoreEvent);
            objInternal.enableCoreEventTrigger();
        }
    }
}

template <class PropObjInterface, class... Interfaces>
ErrCode GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::readLocalValue(const StringPtr& name, BaseObjectPtr& value) const
{
    details::PropertyNameInfo info = details::getPropertyNameInfo(name);

    const auto it = propValues.find(info.name);
    if (it != propValues.cend())
    {
        if (info.index != -1)
        {
            if (it->second.getCoreType() != ctList)
            {
                return DAQ_MAKE_ERROR_INFO(OPENDAQ_ERR_INVALIDPARAMETER, fmt::format(R"(Could not access the index as the value is not a list for property "{}")", name));
            }

            ListPtr<IBaseObject> list = it->second;
            if (info.index >= (int) list.getCount())
            {
                return DAQ_MAKE_ERROR_INFO(OPENDAQ_ERR_OUTOFRANGE, fmt::format(R"(The index parameter is out of bounds of the list for property "{}")", name));
            }
            value = list[std::size_t(info.index)];
        }
        else
        {
            value = it->second;
        }
        return OPENDAQ_SUCCESS;
    }

    return DAQ_MAKE_ERROR_INFO(OPENDAQ_ERR_NOTFOUND, fmt::format(R"(Property value "{}" not found)", name));
}

#if defined(__GNUC__) && __GNUC__ >= 12
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wdangling-pointer"
#endif

template <typename PropObjInterface, typename... Interfaces>
void GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::triggerCoreEventInternal(const CoreEventArgsPtr& args)
{
    if (!coreEventMuted && triggerCoreEvent.assigned())
        triggerCoreEvent(args);
}

template <class PropObjInterface, class... Interfaces>
ErrCode GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::getBoundPropertyInternal(const StringPtr& name,
                                                                                             PropertyPtr& property,
                                                                                             StringPtr& effectiveName,
                                                                                             ConstCharPtr& bracket)
{
    StringPtr propName;
    bracket = details::getPropNameWithoutIndex(name, propName);

    property = getUnboundPropertyOrNull(propName);

    if (!property.assigned())
    {
        return DAQ_MAKE_ERROR_INFO(OPENDAQ_ERR_NOTFOUND, fmt::format(R"(Property "{}" does not exist)", propName));
    }

    bool isRef;
    property = details::checkForRefPropAndGetBoundProp(property, objPtr, &isRef);
    effectiveName = details::buildEffectivePropertyName(propName, name, property, isRef, bracket);
    return OPENDAQ_SUCCESS;
}

template <class PropObjInterface, class... Interfaces>
ErrCode GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::readPropertyValueInternal(const PropertyPtr& property,
                                                                                              const StringPtr& effectiveName,
                                                                                              ConstCharPtr bracket,
                                                                                              bool retrieveUpdatingValue,
                                                                                              BaseObjectPtr& value)
{
    ErrCode res = OPENDAQ_SUCCESS;

    if (retrieveUpdatingValue && updatePropertyStack.getPropertyValue(effectiveName, value))
    {
        if (!value.assigned())
            value = property.getDefaultValue();
    }
    else
    {
        res = readLocalValue(effectiveName, value);
    }

    OPENDAQ_RETURN_IF_FAILED_EXCEPT(res, OPENDAQ_ERR_NOTFOUND);
    if (res == OPENDAQ_ERR_NOTFOUND)
    {
        daqClearErrorInfo();
        OPENDAQ_RETURN_IF_FAILED(details::readDefaultPropertyValue(property, effectiveName, bracket, value));

        if (!value.assigned())
            return OPENDAQ_SUCCESS;
    }

    value = details::cloneIfContainerValue(value);
    return OPENDAQ_SUCCESS;
}

template <class PropObjInterface, class... Interfaces>
ErrCode GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::readPropertyValueInternal(const StringPtr& name,
                                                                                              bool retrieveUpdatingValue,
                                                                                              PropertyPtr& property,
                                                                                              BaseObjectPtr& value)
{
    StringPtr effectiveName;
    ConstCharPtr bracket;
    OPENDAQ_RETURN_IF_FAILED(getBoundPropertyInternal(name, property, effectiveName, bracket));
    return readPropertyValueInternal(property, effectiveName, bracket, retrieveUpdatingValue, value);
}

#if defined(__GNUC__) && __GNUC__ >= 12
#pragma GCC diagnostic pop
#endif

template <class PropObjInterface, class... Interfaces>
ErrCode GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::getPropertyValue(IString* propertyName, IBaseObject** value)
{
    auto lock = getRecursiveConfigLock2();
    return getPropertyValueNoLock(propertyName, value);
}

template <typename PropObjInterface, typename ... Interfaces>
ErrCode GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::getPropertyValueNoLock(IString* propertyName, IBaseObject** value)
{
    return getPropertyValueInternal(propertyName, value, true);
}

template <class PropObjInterface, class... Interfaces>
ErrCode GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::getPropertySelectionValue(IString* propertyName, IBaseObject** value)
{
    auto lock = getRecursiveConfigLock2();
    return getPropertySelectionValueNoLock(propertyName, value);
}

template <typename PropObjInterface, typename... Interfaces>
ErrCode GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::getPropertySelectionValueNoLock(IString* propertyName,
                                                                                                    IBaseObject** value)
{
    return getPropertySelectionValueInternal(propertyName, value, true);
}

template <class PropObjInterface, typename... Interfaces>
ErrCode GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::setPropertySelectionValue(IString* propertyName, IBaseObject* value)
{
    auto lock = getRecursiveConfigLock2();
    return setPropertySelectionValueInternal(propertyName, value, false);
}

template <class PropObjInterface, typename... Interfaces>
ErrCode GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::setProtectedPropertySelectionValue(IString* propertyName, IBaseObject* value)
{
    auto lock = getRecursiveConfigLock2();
    return setPropertySelectionValueInternal(propertyName, value, true);
}

template <typename PropObjInterface, typename... Interfaces>
ErrCode GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::setPropertySelectionValueInternal(IString* propertyName,
                                                                                                      IBaseObject* value,
                                                                                                      bool protectedAccess)
{
    OPENDAQ_PARAM_NOT_NULL(propertyName);
    OPENDAQ_PARAM_NOT_NULL(value);

    const ErrCode errCode = daqTry([&]()
    {
        const auto propName = StringPtr::Borrow(propertyName);
        const auto valuePtr = BaseObjectPtr::Borrow(value);

        if (details::isChildProperty(propName))
        {
            PropertyObjectPtr parentObj;
            StringPtr leafName;
            OPENDAQ_RETURN_IF_FAILED(getParentObject(propName, parentObj, leafName));

            if (protectedAccess)
            {
                const auto parentObjProtected = parentObj.template asPtr<IPropertyObjectProtected>(true);
                return parentObjProtected->setProtectedPropertySelectionValue(leafName, value);
            }
            else
            {
                return parentObj->setPropertySelectionValue(leafName, value);
            }
        }

        PropertyPtr prop = getUnboundPropertyOrNull(propName);
        prop = details::checkForRefPropAndGetBoundProp(prop, objPtr);

        if (!prop.assigned())
            return DAQ_MAKE_ERROR_INFO(OPENDAQ_ERR_NOTFOUND, fmt::format(R"(Property "{}" does not exist)", propName));

        BaseObjectPtr indexOrKey;
        OPENDAQ_RETURN_IF_FAILED(details::selectionValueToKey(prop, valuePtr, indexOrKey));

        return setPropertyValueInternal(propertyName, indexOrKey, true, protectedAccess, updateCount > 0);
    });
    OPENDAQ_RETURN_IF_FAILED(errCode, "Failed to set property selection value");
    return errCode;
}

template <class PropObjInterface, typename... Interfaces>
ErrCode GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::clearProtectedPropertyValue(IString* propertyName)
{
    auto lock = getRecursiveConfigLock2();
    return clearPropertyValueInternal(propertyName, true, updateCount > 0);
}

template <typename PropObjInterface, typename ... Interfaces>
void GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::configureClonedMembers(const CloneParameters& parameters)
{
    configureClonedMembers(parameters.valueWriteEvents,
                           parameters.valueReadEvents,
                           parameters.endUpdateEvent,
                           parameters.triggerCoreEvent,
                           parameters.localProperties,
                           parameters.propValues,
                           parameters.customOrder,
                           parameters.permissionManager,
                           parameters.corePropertyNames);
}

template <typename PropObjInterface, typename... Interfaces>
void GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::configureClonedMembers(
    const std::unordered_map<StringPtr, PropertyValueEventEmitter>& valueWriteEvents,
    const std::unordered_map<StringPtr, PropertyValueEventEmitter>& valueReadEvents,
    const EndUpdateEventEmitter& endUpdateEvent,
    const ProcedurePtr& triggerCoreEvent,
    const PropertyOrderedMap& localProperties,
    const std::unordered_map<StringPtr, BaseObjectPtr, StringHash, StringEqualTo>& propValues,
    const std::vector<StringPtr>& customOrder,
    const PermissionManagerPtr& permissionManager,
    const std::set<StringPtr>& corePropertyNames)
{
    this->valueWriteEvents.clear();
    for (const auto& [name, srcEmitter] : valueWriteEvents)
    {
        BaseObjectPtr cloned;
        srcEmitter.template asPtr<ICloneable>(true)->clone(&cloned);
        this->valueWriteEvents.emplace(name, cloned);
    }
        
    this->valueReadEvents.clear();
    for (const auto& [name, srcEmitter] : valueReadEvents)
    {
        BaseObjectPtr cloned;
        srcEmitter.template asPtr<ICloneable>(true)->clone(&cloned);
        this->valueReadEvents.emplace(name, cloned);
    }

    BaseObjectPtr cloned;
    endUpdateEvent.template asPtr<ICloneable>(true)->clone(&cloned);

    this->endUpdateEvent = cloned;
    this->triggerCoreEvent = triggerCoreEvent;
    this->localProperties = localProperties;
    this->customOrder = customOrder;
    this->corePropertyNames = corePropertyNames;

    BaseObjectPtr permissionManagerClone;
    permissionManager.template asPtr<ICloneable>()->clone(&permissionManagerClone);
    this->permissionManager = permissionManagerClone;

    for (const auto& val : propValues)
    {
        const auto& propName = val.first;
        const auto& prop = val.second;
        const auto ct = prop.getCoreType();
        if (ct == ctList || ct == ctDict)
        {
            if (const auto cloneable = prop.asPtrOrNull<ICloneable>(); cloneable.assigned())
            {
                BaseObjectPtr obj;
                const ErrCode err = cloneable->clone(&obj);
                if (OPENDAQ_FAILED(err))
                    daqClearErrorInfo();
                if (!obj.assigned())
                    continue;

                this->propValues.insert(std::make_pair(propName, obj));
            }
        }
        else if (ct == ctObject)
        {
            if (const auto cloneable = prop.asPtrOrNull<IPropertyObjectInternal>(); cloneable.assigned())
            {
                PropertyObjectPtr obj;
                const ErrCode err = cloneable->clone(&obj);
                if (OPENDAQ_FAILED(err))
                    daqClearErrorInfo();
                if (!obj.assigned())
                    continue;

                auto it = this->propValues.find(propName);
                if (it != this->propValues.end())
                    it->second = obj;
                else
                    this->propValues.insert(std::make_pair(propName, obj));
            }
        }
        else
        {
            this->propValues.insert(val);
        }
    }
}

template <typename PropObjInterface, typename... Interfaces>
std::unique_ptr<RecursiveConfigLockGuard> GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::getRecursiveConfigLock()
{
    LockGuardPtr lockGuard;
    checkErrorInfo(getRecursiveLockGuard(&lockGuard));
    return std::make_unique<RecursiveConfigLockGuard>(lockGuard);
}

template <typename PropObjInterface, typename... Interfaces>
std::lock_guard<std::mutex> GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::getAcquisitionLock()
{
    std::mutex* mutexPtr = getLocalMutex();
    return std::lock_guard(*mutexPtr);
}

template <typename PropObjInterface, typename ... Interfaces>
std::unique_lock<std::mutex> GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::getUniqueLock()
{
    std::mutex* mutexPtr = getLocalMutex();
    return std::unique_lock(*mutexPtr);
}

template <typename PropObjInterface, typename ... Interfaces>
LockGuardPtr GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::getRecursiveConfigLock2()
{
    LockGuardPtr lg;
    checkErrorInfo(getRecursiveLockGuard(&lg));
    return lg;
}

template <typename PropObjInterface, typename ... Interfaces>
std::lock_guard<MutexPtr> GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::getAcquisitionLock2()
{
    return std::lock_guard(sync);
}

template <typename PropObjInterface, typename ... Interfaces>
std::unique_lock<MutexPtr> GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::getUniqueLock2()
{
    return std::unique_lock(sync);
}

template <class PropObjInterface, class... Interfaces>
ErrCode GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::clearPropertyValue(IString* propertyName)
{
    auto lock = getRecursiveConfigLock2();
    return clearPropertyValueNoLock(propertyName);
}

template <typename PropObjInterface, typename... Interfaces>
ErrCode GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::clearPropertyValueNoLock(IString* propertyName)
{
    return clearPropertyValueInternal(propertyName, false, updateCount > 0);
}

template <class PropObjInterface, class... Interfaces>
ErrCode GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::clearPropertyValues()
{
    auto lock = getRecursiveConfigLock2();
    const ErrCode errCode = clearPropertyValuesInternal(false);
    OPENDAQ_RETURN_IF_FAILED(errCode);
    return errCode;
}

template <class PropObjInterface, class... Interfaces>
ErrCode GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::clearProtectedPropertyValues()
{
    auto lock = getRecursiveConfigLock2();
    const ErrCode errCode = clearPropertyValuesInternal(true);
    OPENDAQ_RETURN_IF_FAILED(errCode);
    return errCode;
}

template <class PropObjInterface, class... Interfaces>
ErrCode GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::clearPropertyValuesInternal(bool protectedAccess)
{
    if (frozen)
        return DAQ_MAKE_ERROR_INFO(OPENDAQ_ERR_FROZEN);

    ListPtr<IProperty> properties;
    OPENDAQ_RETURN_IF_FAILED(getAllProperties(&properties), "Failed to get all properties");

    for (const auto& prop : properties)
    {
        if (!protectedAccess && prop.asPtrOrNull<IPropertyInternal>(true).getReadOnlyNoLock())
            continue;

        if (prop.getPropertyType() == PropertyType::Reference)
            continue;

        if (prop.getValueType() == ctObject)
        {
            // `prop` is already bound and cannot be a reference here; read the value directly
            BaseObjectPtr valuePtr;
            const ErrCode err = readPropertyValueInternal(prop, prop.getName(), nullptr, false, valuePtr);
            OPENDAQ_RETURN_IF_FAILED(err);

            if (const auto freezable = valuePtr.asPtrOrNull<IFreezable>(true); freezable.assigned() && freezable.isFrozen())
                continue;

            if (protectedAccess)
            {
                auto nested = valuePtr.asPtr<IPropertyObjectProtected>(true);
                OPENDAQ_RETURN_IF_FAILED(nested->clearProtectedPropertyValues());
            }
            else
            {
                auto nested = valuePtr.asPtr<IPropertyObject>(true);
                OPENDAQ_RETURN_IF_FAILED(nested->clearPropertyValues());
            }
        }
        else
        {
            if (localProperties.find(prop.getName()) == localProperties.end())
                continue;

            // For non-object properties, use the internal clear function and let begin/endUpdate apply the changes.
            const ErrCode errCode = clearPropertyValueInternal(prop.getName(), protectedAccess, updateCount > 0);
            OPENDAQ_RETURN_IF_FAILED(errCode);
        }
    }

    return OPENDAQ_SUCCESS;
}

template <class PropObjInterface, class... Interfaces>
ErrCode GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::clearPropertyValueInternal(IString* name,
                                                                                               bool protectedAccess,
                                                                                               bool batch,
                                                                                               bool isUpdating)
{
    OPENDAQ_PARAM_NOT_NULL(name);

    if (frozen)
        return DAQ_MAKE_ERROR_INFO(OPENDAQ_ERR_FROZEN);

    const ErrCode errCode = daqTry([&]()
    {
        auto propName = StringPtr::Borrow(name);

        if (batch)
        {
            batchedUpdates.emplace_back(std::make_pair(propName, UpdatingAction{false, protectedAccess, nullptr}));
            return OPENDAQ_SUCCESS;
        }

        if (details::isChildProperty(propName))
        {
            PropertyObjectPtr parentObj;
            StringPtr leafName;
            OPENDAQ_RETURN_IF_FAILED(getParentObject(propName, parentObj, leafName));

            if (protectedAccess)
            {
                const auto parentObjProtected = parentObj.template asPtr<IPropertyObjectProtected>(true);
                parentObjProtected.clearProtectedPropertyValue(leafName);
            }
            else
            {
                parentObj.clearPropertyValue(leafName);
            }

            return OPENDAQ_SUCCESS;
        }

        PropertyPtr prop = getUnboundPropertyOrNull(propName);
        prop = details::checkForRefPropAndGetBoundProp(prop, objPtr);

        if (!prop.assigned())
        {
            return DAQ_MAKE_ERROR_INFO(OPENDAQ_ERR_NOTFOUND, fmt::format(R"(Property "{}" does not exist)", propName));
        }

        propName = prop.getName();
        const auto propInternal = prop.asPtr<IPropertyInternal>();

        if (!protectedAccess)
        {
            if (propInternal.getReadOnlyNoLock())
            {
                return DAQ_MAKE_ERROR_INFO(OPENDAQ_ERR_ACCESSDENIED, fmt::format(R"(Property "{}" is read only)", propName));
            }
        }

        {
            if (propValues.find(prop.getName()) == propValues.end())
                return OPENDAQ_IGNORED;

            if (prop.getValueType() == ctObject)
            {
                if (auto it = propValues.find(prop.getName()); it->second.assigned())
                {
                    // Match clearPropertyValuesInternal: leave frozen nested objects intact.
                    if (const auto freezable = it->second.template asPtrOrNull<IFreezable>(true);
                        freezable.assigned() && freezable.isFrozen())
                        return OPENDAQ_IGNORED;

                    if (protectedAccess)
                    {
                        auto objProtected = it->second.template asPtr<IPropertyObjectProtected>(true);
                        auto obj = it->second.template asPtr<IPropertyObject>(true);
                        for (const auto& childProp: obj.getAllProperties())
                        {
                            objProtected.clearProtectedPropertyValue(childProp.getName());
                        }
                    }
                    else
                    {
                        auto obj = it->second.template asPtr<IPropertyObject>(true);
                        for (const auto& childProp: obj.getAllProperties())
                        {
                            obj.clearPropertyValue(childProp.getName());
                        }
                    }
                    
                }
            }
            else
            {
                BaseObjectPtr newVal;
                const ErrCode err = callPropertyValueWrite(prop, newVal, PropertyEventType::Clear, isUpdating);

                OPENDAQ_RETURN_IF_FAILED(err);
                
                if (err == OPENDAQ_IGNORED)
                    return OPENDAQ_SUCCESS;
                
                if (!newVal.assigned())
                {
                    auto it = propValues.find(prop.getName());
                    propValues.erase(it);
                }

                if (!isUpdating)
                    triggerCoreEventInternal(CoreEventArgsPropertyValueChanged(objPtr, propName, newVal, path));
            }
        }
        return OPENDAQ_SUCCESS;
    });
    OPENDAQ_RETURN_IF_FAILED(errCode, "Failed to clear property value");
    return errCode;
}

template <typename PropObjInterface, typename... Interfaces>
ErrCode GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::getPropertyValueInternal(IString* propertyName, IBaseObject** value, Bool retrieveUpdatingValue)
{
    OPENDAQ_PARAM_NOT_NULL(propertyName);
    OPENDAQ_PARAM_NOT_NULL(value);

    const ErrCode errCode = daqTry([&]()
    {
        auto propName = StringPtr::Borrow(propertyName);
        BaseObjectPtr valuePtr;
        ErrCode err;

        if (details::isChildProperty(propName))
        {
            PropertyObjectPtr parentObj;
            StringPtr leafName;
            err = getParentObject(propName, parentObj, leafName);
            OPENDAQ_RETURN_IF_FAILED(err);
            err = parentObj->getPropertyValue(leafName, &valuePtr);
        }
        else
        {
            PropertyPtr prop;
            err = readPropertyValueInternal(propName, retrieveUpdatingValue, prop, valuePtr);
            OPENDAQ_RETURN_IF_FAILED(err);
            if (valuePtr.assigned())
                valuePtr = callPropertyValueRead(prop, valuePtr);
        }
        OPENDAQ_RETURN_IF_FAILED(err);

        *value = valuePtr.detach();
        return err;
    });
    OPENDAQ_RETURN_IF_FAILED(errCode, "Failed to get property value");
    return errCode;
}

template <typename PropObjInterface, typename... Interfaces>
ErrCode GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::getPropertySelectionValueInternal(IString* propertyName,
                                                                                                      IBaseObject** value,
                                                                                                      Bool retrieveUpdatingValue)
{
    OPENDAQ_PARAM_NOT_NULL(propertyName);
    OPENDAQ_PARAM_NOT_NULL(value);

    const auto propName = StringPtr::Borrow(propertyName);

    const ErrCode errCode = daqTry([&]()
    {
        BaseObjectPtr valuePtr;
        PropertyPtr prop;

        if (details::isChildProperty(propName))
        {
            const ErrCode errCode = getProperty(propName, &prop);
            OPENDAQ_RETURN_IF_FAILED(errCode, OPENDAQ_ERR_NOTFOUND, fmt::format(R"(Selection property "{}" not found)", propName));
            valuePtr = prop.getValue();
        }
        else
        {
            const ErrCode errCode = readPropertyValueInternal(propName, retrieveUpdatingValue, prop, valuePtr);
            OPENDAQ_RETURN_IF_FAILED(errCode, OPENDAQ_ERR_NOTFOUND, fmt::format(R"(Selection property "{}" not found)", propName));
            if (valuePtr.assigned())
                valuePtr = callPropertyValueRead(prop, valuePtr);
        }

        OPENDAQ_RETURN_IF_FAILED(details::selectionKeyToValue(prop, valuePtr));

        *value = valuePtr.detach();
        return OPENDAQ_SUCCESS;
    });
    OPENDAQ_RETURN_IF_FAILED(errCode, fmt::format(R"(Failed to get property selection value for property "{}")", propName));
    return errCode;
}

template <class PropObjInterface, class... Interfaces>
ErrCode GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::getProperty(IString* propertyName, IProperty** property)
{
    OPENDAQ_PARAM_NOT_NULL(propertyName);
    OPENDAQ_PARAM_NOT_NULL(property);

    const ErrCode errCode = daqTry([&]() -> auto
    {
        StringPtr propName = propertyName;
        PropertyPtr prop;

        if (details::isChildProperty(propName))
        {
            PropertyObjectPtr parentObj;
            StringPtr leafName;
            OPENDAQ_RETURN_IF_FAILED(getParentObject(propName, parentObj, leafName));

            prop = parentObj.getProperty(leafName);
        }
        else
        {
            prop = getUnboundPropertyOrNull(propName);
            if (!prop.assigned())
                return DAQ_MAKE_ERROR_INFO(OPENDAQ_ERR_NOTFOUND, fmt::format(R"(Property "{}" does not exist)", propName));

            prop = prop.asPtr<IPropertyInternal>().cloneWithOwner(objPtr);
        }

        if (const auto freezable = prop.template asPtrOrNull<IFreezable>(true); freezable.assigned())
        {
            OPENDAQ_RETURN_IF_FAILED(freezable->freeze());
        }

        *property = prop.detach();
        return OPENDAQ_SUCCESS;
    });
    OPENDAQ_RETURN_IF_FAILED(errCode);
    return errCode;
}

template <typename PropObjInterface, typename... Interfaces>
ErrCode GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::addPropertyInternal(IProperty* property, bool isCoreProperty)
{
    OPENDAQ_PARAM_NOT_NULL(property);

    if (frozen)
        return DAQ_MAKE_ERROR_INFO(OPENDAQ_ERR_FROZEN);

    auto lock = getRecursiveConfigLock2();

    const ErrCode errCode = daqTry([&]() -> ErrCode 
    {
        const PropertyPtr propPtr = property;
        const PropertyInternalPtr propInternalPtr = propPtr.asPtr<IPropertyInternal>(true);
        const StringPtr propName = propPtr.getName();

        if (!propName.assigned())
            return DAQ_MAKE_ERROR_INFO(OPENDAQ_ERR_INVALIDVALUE, fmt::format(R"(Property "{}" does not have an assigned name.)", propName));

        if (details::hasDuplicateReferences(propPtr, objPtr))
            return DAQ_MAKE_ERROR_INFO(OPENDAQ_ERR_INVALIDVALUE,
                                       fmt::format(R"(Reference property "{}" references a property that is already referenced by another.)", propName));

        propPtr.asPtr<IOwnable>(true).setOwner(objPtr);

        const auto res = localProperties.insert(std::make_pair(propName, propPtr));
        if (!res.second)
            return DAQ_MAKE_ERROR_INFO(OPENDAQ_ERR_ALREADYEXISTS, fmt::format(R"(Property "{}" already exists.)", propName));

        auto readEvent = propInternalPtr.getClassOnPropertyValueRead();
        if (readEvent.getListenerCount())
        {
            PropertyValueEventEmitter emitter;
            valueReadEvents.emplace(propName, emitter);
            for (const auto& listener : readEvent.getListeners())
                emitter.addHandler(listener);
        }

        auto writeEvent = propInternalPtr.getClassOnPropertyValueWrite();
        if (writeEvent.getListenerCount())
        {
            PropertyValueEventEmitter emitter;
            valueWriteEvents.emplace(propName, emitter);
            for (const auto& listener : writeEvent.getListeners())
                emitter.addHandler(listener);
        }

        if (checkIsChildObjectProperty(propPtr))
        {
            auto defaultValue = propPtr.getDefaultValue();
            setChildPropertyObject(propName, defaultValue);

            if (!isCoreProperty)
            {
                const auto cloneable = defaultValue.asPtrOrNull<IPropertyObjectInternal>();
                PropertyObjectPtr clone;
                ErrCode err = cloneable->clone(&clone);
                OPENDAQ_RETURN_IF_FAILED(err);

                if (defaultValue.getObject() != clone.getObject())
                    propInternalPtr.overrideDefaultValue(clone);
            }
        }

        if (isCoreProperty)
            corePropertyNames.insert(propName);
        
        triggerCoreEventInternal(CoreEventArgsPropertyAdded(objPtr, propPtr, path));

        return OPENDAQ_SUCCESS;
    });
    OPENDAQ_RETURN_IF_FAILED(errCode);
    return errCode;
}

template <class PropObjInterface, class... Interfaces>
ErrCode GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::addProperty(IProperty* property)
{
    const ErrCode errCode = addPropertyInternal(property);
    OPENDAQ_RETURN_IF_FAILED(errCode);
    return errCode;
}

template <typename PropObjInterface, typename... Interfaces>
ErrCode GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::addCoreProperty(IProperty* property)
{
    const ErrCode errCode = addPropertyInternal(property, true);
    OPENDAQ_RETURN_IF_FAILED(errCode);
    return errCode;
}

template <typename PropObjInterface, typename... Interfaces>
ErrCode GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::removeProperty(IString* propertyName)
{
    OPENDAQ_PARAM_NOT_NULL(propertyName);

    if (frozen)
    {
        return DAQ_MAKE_ERROR_INFO(OPENDAQ_ERR_FROZEN);
    }

    auto lock = getRecursiveConfigLock2();

    auto namePtr = StringPtr::Borrow(propertyName);
    if (localProperties.find(propertyName) == localProperties.cend())
    {
        return DAQ_MAKE_ERROR_INFO(OPENDAQ_ERR_NOTFOUND, fmt::format(R"(Property "{}" does not exist)", namePtr));
    }

    localProperties.erase(propertyName);
    if (propValues.find(propertyName) != propValues.cend())
    {
        propValues.erase(propertyName);
    }

    if (auto it = corePropertyNames.find(namePtr); it != corePropertyNames.end())
        corePropertyNames.erase(it);

    triggerCoreEventInternal(CoreEventArgsPropertyRemoved(objPtr, propertyName, path));

    return OPENDAQ_SUCCESS;
}

template <class PropObjInterface, class... Interfaces>
ErrCode GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::getVisibleProperties(IList** properties)
{
    return getPropertiesInternal(false, true, properties, false);
}

template <class PropObjInterface, class... Interfaces>
ErrCode GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::getAllProperties(IList** properties)
{
    return getPropertiesInternal(true, true, properties, false);
}

template <class PropObjInterface, typename... Interfaces>
ErrCode GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::setPropertyOrderInternal(IList* orderedPropertyNames, bool isUpdating)
{
    auto lock = getRecursiveConfigLock2();
    if (frozen)
        return DAQ_MAKE_ERROR_INFO(OPENDAQ_ERR_FROZEN);

    if (orderedPropertyNames != nullptr)
        customOrder = ListPtr<IString>::Borrow(orderedPropertyNames).toVector();
    else
        customOrder.clear();

    if (!isUpdating)
        triggerCoreEventInternal(CoreEventArgsPropertyOrderChanged(objPtr, orderedPropertyNames, path));

    return OPENDAQ_SUCCESS;
}

template <class PropObjInterface, typename... Interfaces>
ErrCode GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::setPropertyOrder(IList* orderedPropertyNames)
{
    return setPropertyOrderInternal(orderedPropertyNames);
}

template <class PropObjInterface, class... Interfaces>
ErrCode GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::getPropertiesInternal(Bool includeInvisible,
                                                                                          Bool bind,
                                                                                          IList** list,
                                                                                          Bool includeCoreProperties)
{
    OPENDAQ_PARAM_NOT_NULL(list);

    if (!includeInvisible && !bind)
        return DAQ_MAKE_ERROR_INFO(OPENDAQ_ERR_INVALIDPARAMETER);

    std::vector<PropertyPtr> allProperties;
    if (objectClass.assigned())
    {
        auto propList = objectClass.getProperties(True);
        allProperties.reserve(propList.getCount() + localProperties.size());
        for (const auto& prop : propList)
            allProperties.push_back(prop);
    }
    else
    {
        allProperties.reserve(localProperties.size());
    }

    for (const auto& [propName, prop] : localProperties)
    {
        if (includeCoreProperties || corePropertyNames.count(propName) == 0)
            allProperties.push_back(prop);
    }

    PropertyOrderedMap lookup;
    for (auto& prop : allProperties)
    {
        if (!bind)
        {
            lookup.insert({prop.getName(), prop});
            continue;
        }

        auto boundProp = prop.asPtr<IPropertyInternal>(true).cloneWithOwner(objPtr);
        if (!includeInvisible && boundProp.getIsReferenced())
        {
            continue;
        }

        try
        {
            if (!includeInvisible && !boundProp.getVisible())
            {
                continue;
            }

            auto freezable = boundProp.template asPtrOrNull<IFreezable>(true);
            if (freezable.assigned())
            {
                freezable.freeze();
            }

            lookup.insert_or_assign(boundProp.getName(), boundProp);
        }
        catch (const NotFoundException& e)
        {
            return errorFromException(e);
        }
        catch (const CalcFailedException&)
        {
        }
        catch (const NoInterfaceException&)
        {
        }
    }

    auto properties = List<IProperty>();
    if (!customOrder.empty())
    {
        // Add properties with explicit order
        for (auto& propName : customOrder)
        {
            const auto iter = lookup.find(propName);
            if (iter != lookup.cend())
            {
                properties.unsafePushBack(iter->second);
                lookup.erase(iter);
            }
        }

        // Add the rest of without set order
        for (auto& prop : lookup)
        {
            properties.unsafePushBack(prop.second);
        }
    }
    else
    {
        for (auto& prop : lookup)
        {
            properties.unsafePushBack(prop.second);
        }
    }

    *list = properties.detach();
    return OPENDAQ_SUCCESS;
}

template <class PropObjInterface, class... Interfaces>
ErrCode GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::getPropertyValueEventInternal(IString* propertyName, IEvent** event, bool valueWrite)
{
    OPENDAQ_PARAM_NOT_NULL(propertyName);
    OPENDAQ_PARAM_NOT_NULL(event);

    StringPtr name = StringPtr::Borrow(propertyName);

    if (details::isChildProperty(name))
    {
        PropertyObjectPtr parentObj;
        StringPtr leafName;
        const ErrCode errCode = getParentObject(name, parentObj, leafName);
        OPENDAQ_RETURN_IF_FAILED(errCode);

        return valueWrite ? parentObj->getOnPropertyValueWrite(leafName, event)
                          : parentObj->getOnPropertyValueRead(leafName, event);
    }

    Bool hasProp;
    const ErrCode err = this->hasProperty(name, &hasProp);
    OPENDAQ_RETURN_IF_FAILED(err);

    if (!hasProp)
        return DAQ_MAKE_ERROR_INFO(OPENDAQ_ERR_NOTFOUND, fmt::format(R"(Property "{}" does not exist)", name));

    PropertyInternalPtr prop = getUnboundPropertyOrNull(name);
    if (!prop.assigned())
        return DAQ_MAKE_ERROR_INFO(OPENDAQ_ERR_NOTFOUND, fmt::format(R"(Property "{}" does not exist)", name));

    if (prop.getReferencedPropertyUnresolved().assigned())
        return DAQ_MAKE_ERROR_INFO(OPENDAQ_ERR_INVALID_OPERATION,
                                   fmt::format(R"({} is not allowed for the reference properties "{}")",
                                               valueWrite ? "getOnPropertyValueWrite" : "getOnPropertyValueRead",
                                               name));

    auto& events = valueWrite ? valueWriteEvents : valueReadEvents;
    auto [it, _] = events.try_emplace(name);
    *event = it->second.addRefAndReturn();
    return OPENDAQ_SUCCESS;
}

template <typename PropObjInterface, typename... Interfaces>
ErrCode GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::getOnPropertyValueWrite(IString* propertyName, IEvent** event)
{
    return getPropertyValueEventInternal(propertyName, event, true);
}

template <typename PropObjInterface, typename... Interfaces>
ErrCode GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::getOnPropertyValueRead(IString* propertyName, IEvent** event)
{
    return getPropertyValueEventInternal(propertyName, event, false);
}

template <typename PropObjInterface, typename ... Interfaces>
ErrCode GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::getOnAnyPropertyValueWrite(IEvent** event)
{
    OPENDAQ_PARAM_NOT_NULL(event);
    
    *event = valueWriteEvents[AnyWriteEventName].addRefAndReturn();
    return OPENDAQ_SUCCESS;
}

template <typename PropObjInterface, typename ... Interfaces>
ErrCode GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::getOnAnyPropertyValueRead(IEvent** event)
{
    OPENDAQ_PARAM_NOT_NULL(event);
    
    *event = valueReadEvents[AnyReadEventName].addRefAndReturn();
    return OPENDAQ_SUCCESS;
}

template <typename PropObjInterface, typename... Interfaces>
ErrCode GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::beginUpdate()
{
    auto lock = getRecursiveConfigLock2();
    return beginUpdateInternal(true);
}

template <typename PropObjInterface, typename... Interfaces>
void GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::callBeginUpdateOnChildren()
{
    forEachUnfrozenChildObject([](const PropertyObjectPtr& propObj) { propObj.beginUpdate(); });
}

template <typename PropObjInterface, typename... Interfaces>
void GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::callEndUpdateOnChildren()
{
    forEachUnfrozenChildObject([](const PropertyObjectPtr& propObj) { propObj.endUpdate(); });
}

template <typename PropObjInterface, typename... Interfaces>
PropertyObjectPtr GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::getPropertyObjectParent()
{
    if (owner.assigned())
        return owner.getRef();

    return nullptr;
}

template <typename PropObjInterface, typename... Interfaces>
ErrCode GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::beginUpdateInternal(bool deep)
{
    if (frozen)
        return DAQ_MAKE_ERROR_INFO(OPENDAQ_ERR_FROZEN);

    updateCount++;

    if (!deep)
        return OPENDAQ_SUCCESS;

    const ErrCode errCode = daqTry([this] { callBeginUpdateOnChildren(); });
    OPENDAQ_RETURN_IF_FAILED(errCode);
    return errCode;
}

template <typename PropObjInterface, typename... Interfaces>
ErrCode GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::endUpdateInternal(bool deep)
{
    if (updateCount == 0)
        return DAQ_MAKE_ERROR_INFO(OPENDAQ_ERR_INVALIDSTATE, "The object is not in updating state");

    const auto newUpdateCount = --updateCount;

    if (newUpdateCount == 0)
    {
        const auto errCode = daqTry([this]
        {
            beginApplyUpdate();
            return OPENDAQ_SUCCESS;
        });

        OPENDAQ_RETURN_IF_FAILED(errCode);
    }

    if (deep)
    {
        const auto errCode = daqTry([this]
        {
            callEndUpdateOnChildren();
        });

        OPENDAQ_RETURN_IF_FAILED(errCode);
    }

    if (newUpdateCount == 0)
    {
        const ErrCode errCode = daqTry([this] 
        {
            endApplyUpdate();
            return OPENDAQ_SUCCESS;
        });
        OPENDAQ_RETURN_IF_FAILED(errCode);
        return errCode;
    }

    return OPENDAQ_SUCCESS;
}

template <typename PropObjInterface, typename... Interfaces>
ErrCode GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::getUpdatingInternal(Bool* updating)
{
    OPENDAQ_PARAM_NOT_NULL(updating);

    *updating = updateCount > 0 ? True : False;
    return OPENDAQ_SUCCESS;
}

template <typename PropObjInterface, typename ... Interfaces>
std::mutex* GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::getLocalMutex()
{
    if (this->lockingStrategy == LockingStrategy::InheritLock)
    {
        DAQ_THROW_EXCEPTION(daq::InvalidStateException, "Can't acquire local mutex if locking strategy is set to inherit");
    }

    MutexImpl* mutexImpl = dynamic_cast<MutexImpl*>(this->sync.getObject());
    return &mutexImpl->mutex;
}

template <typename PropObjInterface, typename... Interfaces>
void GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::beginApplyUpdate()
{
    beginApplyProperties(batchedUpdates, isParentUpdating());
}

template <typename PropObjInterface, typename... Interfaces>
void GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::endApplyUpdate()
{
    UpdatingActions localUpdates = std::move(batchedUpdates);
    UpdatingActions appliedUpdates;
    appliedUpdates.reserve(localUpdates.size());

    for (auto& [propName, action] : localUpdates)
    {
        StringPtr name = propName;
        ErrCode err;
        if (action.setValue)
        {
            err = setPropertyValueInternal(name, action.value, true, action.protectedAccess, false, true);
            checkErrorInfo(err);
        }
        else
        {
            err = clearPropertyValueInternal(name, action.protectedAccess, false, true);
            checkErrorInfo(err);
        }

        if (err != OPENDAQ_IGNORED)
        {
            PropertyPtr prop;
            if (OPENDAQ_SUCCEEDED(readPropertyValueInternal(name, false, prop, action.value)) && action.value.assigned())
            {
                // TODO: firing read events while applying updates is likely unintended; kept for behavior parity
                action.value = callPropertyValueRead(prop, action.value);
            }
            appliedUpdates.emplace_back(name, action);
        }
    }

    endApplyProperties(appliedUpdates, isParentUpdating());
}

template <typename PropObjInterface, typename... Interfaces>
bool GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::isParentUpdating()
{
    bool parentUpdating;
    const auto parent = getPropertyObjectParent();
    if (!parent.assigned())
        return false;
    return parentUpdating = parent.template asPtr<IPropertyObjectInternal>(true).isUpdating();
}

template <typename PropObjInterface, typename... Interfaces>
void GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::onUpdatableUpdateEnd(const BaseObjectPtr& /* context */)
{
}

template <typename PropObjInterface, typename... Interfaces>
ErrCode GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::endUpdate()
{
    auto lock = getRecursiveConfigLock2();
    return endUpdateInternal(true);
}

template <typename PropObjInterface, typename... Interfaces>
ErrCode GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::getUpdating(Bool* updating)
{
    auto lock = getRecursiveConfigLock2();
    return getUpdatingInternal(updating);
}

template <typename PropObjInterface, typename... Interfaces>
ErrCode GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::getOnEndUpdate(IEvent** event)
{
    OPENDAQ_PARAM_NOT_NULL(event);

    *event = endUpdateEvent.addRefAndReturn();
    return OPENDAQ_SUCCESS;
}

template <typename PropObjInterface, typename... Interfaces>
ErrCode GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::getPermissionManager(IPermissionManager** permissionManager)
{
    OPENDAQ_PARAM_NOT_NULL(permissionManager);

    *permissionManager = this->permissionManager.addRefAndReturn();
    return OPENDAQ_SUCCESS;
}

template <typename PropObjInterface, typename... Interfaces>
ErrCode GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::findProperties(IList** properties, ISearchFilter* propertyFilter, ISearchFilter* /*componentFilter*/)
{
    OPENDAQ_PARAM_NOT_NULL(properties);

    // If no filter is provided, only visible properties directly belonging to the current object are returned.
    if (!propertyFilter)
        return getPropertiesInternal(false, true, properties, true);

    const ErrCode errCode = daqTry([&]
    {
        auto filterPtr = SearchFilterPtr::Borrow(propertyFilter);
        ListPtr<IProperty> allProperties;
        auto foundProperties = List<IProperty>();

        ErrCode errCode = getPropertiesInternal(true, true, &allProperties, true);
        OPENDAQ_RETURN_IF_FAILED(errCode);

        for (const auto& property : allProperties)
        {
            if (filterPtr.acceptsObject(property))
                foundProperties.pushBack(property);

            if (checkIsChildObjectProperty(property))
            {
                if (auto childPropertyObject = property.getValue().asPtrOrNull<IPropertyObject>();
                            childPropertyObject.assigned() && filterPtr.supportsInterface<IRecursiveSearch>())
                {
                    for (const auto& foundChildProperty : childPropertyObject.findProperties(filterPtr))
                        foundProperties.pushBack(foundChildProperty);
                }
            }
        }

        *properties = foundProperties.detach();
        return OPENDAQ_SUCCESS;
    });
    OPENDAQ_RETURN_IF_FAILED(errCode);
    return errCode;
}

template <typename PropObjInterface, typename... Interfaces>
ErrCode GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::checkForReferencesInternal(IProperty* property, Bool* isReferenced)
{
    OPENDAQ_PARAM_NOT_NULL(isReferenced);
    *isReferenced = false;

    const ErrCode errCode = daqTry([&]()
    {
        const auto propPtr = PropertyPtr::Borrow(property);
        const auto name = propPtr.getName();

        if (objectClass.assigned())
        {
            for (const auto& prop : objectClass.getProperties(True))
            {
                if (*isReferenced = details::checkIsReferenced(name, prop); *isReferenced)
                    return OPENDAQ_SUCCESS;
            }
        }

        for (const auto& prop : localProperties)
        {
            if (*isReferenced = details::checkIsReferenced(name, prop.second); *isReferenced)
                return OPENDAQ_SUCCESS;
        }
        return OPENDAQ_SUCCESS;
    });
    OPENDAQ_RETURN_IF_FAILED(errCode, "Failed to check for references");
    return errCode;
}

template <typename PropObjInterface, typename... Interfaces>
ErrCode GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::checkForReferencesNoLock(IProperty* property, Bool* isReferenced)
{
    return checkForReferencesInternal(property, isReferenced);
}

template <typename PropObjInterface, typename... Interfaces>
ErrCode GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::checkForReferences(IProperty* property, Bool* isReferenced)
{
    auto lock = getRecursiveConfigLock2();
    return checkForReferencesNoLock(property, isReferenced);
}

template <typename PropObjInterface, typename... Interfaces>
ErrCode GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::enableCoreEventTrigger()
{
    auto lock = getRecursiveConfigLock2();

    coreEventMuted = false;

    for (auto& [propName, propValue] : propValues)
    {
        if (const auto & propObj = propValue.template asPtrOrNull<IPropertyObject>(true); propObj.assigned())
            configureClonedObj(propName, propObj);
    }

    return OPENDAQ_SUCCESS;
}

template <typename PropObjInterface, typename... Interfaces>
ErrCode GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::disableCoreEventTrigger()
{
    auto lock = getRecursiveConfigLock2();

    coreEventMuted = true;

    for (auto& item : propValues)
    {
        if (item.second.assigned())
        {
            const auto objInternal = item.second.template asPtrOrNull<IPropertyObjectInternal>();
            if (objInternal.assigned())
                objInternal.disableCoreEventTrigger();
        }
    }

    for (const auto& item : localProperties)
    {
        if (item.second.assigned())
        {
            const auto propInternal = item.second.template asPtr<IPropertyInternal>();
            if (propInternal.getValueTypeUnresolved() == ctObject)
            {
                const auto defaultVal = item.second.getDefaultValue();
                if (defaultVal.assigned())
                {
                    const auto objInternal = defaultVal.template asPtrOrNull<IPropertyObjectInternal>();
                    if (objInternal.assigned())
                        objInternal.disableCoreEventTrigger();
                }
            }
        }
    }

    return OPENDAQ_SUCCESS;
}

template <typename PropObjInterface, typename... Interfaces>
ErrCode GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::getCoreEventTrigger(IProcedure** trigger)
{
    OPENDAQ_PARAM_NOT_NULL(trigger);
    
    auto lock = getRecursiveConfigLock2();
    *trigger = this->triggerCoreEvent.addRefAndReturn();
    return OPENDAQ_SUCCESS;
}

template <typename PropObjInterface, typename... Interfaces>
ErrCode GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::setCoreEventTrigger(IProcedure* trigger)
{
    auto lock = getRecursiveConfigLock2();
    this->triggerCoreEvent = trigger;
    return OPENDAQ_SUCCESS;
}

template <typename PropObjInterface, typename... Interfaces>
ErrCode GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::clone(IPropertyObject** cloned)
{
    OPENDAQ_PARAM_NOT_NULL(cloned);

    const auto managerRef = manager.assigned() ? manager.getRef() : nullptr;
    PropertyObjectPtr obj = createWithImplementation<IPropertyObject, PropertyObjectImpl>(managerRef, this->className);

    const ErrCode errCode = daqTry([this, &obj, &cloned]()
    {
        auto implPtr = static_cast<PropertyObjectImpl*>(obj.getObject());
        implPtr->configureClonedMembers(valueWriteEvents,
                                        valueReadEvents,
                                        endUpdateEvent,
                                        triggerCoreEvent,
                                        localProperties,
                                        propValues,
                                        customOrder,
                                        permissionManager,
                                        corePropertyNames);

        *cloned = obj.detach();
        return OPENDAQ_SUCCESS;
    });
    OPENDAQ_RETURN_IF_FAILED(errCode);
    return errCode;
}

template <typename PropObjInterface, typename... Interfaces>
ErrCode GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::setPath(IString* path)
{
    OPENDAQ_PARAM_NOT_NULL(path);

    auto lock = getRecursiveConfigLock2();
    if (this->path.getLength())
        return OPENDAQ_IGNORED;

    this->path = path;
    return OPENDAQ_SUCCESS;
}

template <typename PropObjInterface, typename ... Interfaces>
ErrCode GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::getPath(IString** path)
{
    OPENDAQ_PARAM_NOT_NULL(path);

    auto lock = getRecursiveConfigLock2();

    *path = this->path.addRefAndReturn();
    return OPENDAQ_SUCCESS;
}

template <typename PropObjInterface, typename... Interfaces>
ErrCode GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::isUpdating(Bool* updating)
{
    OPENDAQ_PARAM_NOT_NULL(updating);

    *updating = updateCount > 0 ? True : False;
    return OPENDAQ_SUCCESS;
}

template <typename PropObjInterface, typename... Interfaces>
ErrCode GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::hasUserReadAccess(IBaseObject* userContext, Bool* hasAccessOut)
{
    OPENDAQ_PARAM_NOT_NULL(hasAccessOut);
    *hasAccessOut = hasUserReadAccess(userContext, this->objPtr);
    return OPENDAQ_SUCCESS;
}

template <typename PropObjInterface, typename... Interfaces>
ErrCode GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::getLockGuard(ILockGuard** lockGuard)
{
    return propObjCore->getLockGuard(lockGuard, sync);
}

template <typename PropObjInterface, typename... Interfaces>
ErrCode GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::getRecursiveLockGuard(ILockGuard** lockGuard)
{
    auto lockOwnerPtr = lockOwner.assigned() ? lockOwner.getRef() : nullptr;
    if (lockOwnerPtr.assigned() && lockingStrategy == LockingStrategy::InheritLock)
        return lockOwnerPtr->getRecursiveLockGuard(lockGuard);

    return propObjCore->getRecursiveLockGuard(lockGuard, sync);
}

template <typename PropObjInterface, typename ... Interfaces>
ErrCode GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::setLockingStrategy(LockingStrategy strategy)
{
    if (this->owner.assigned() && this->owner.getRef().assigned())
        return DAQ_MAKE_ERROR_INFO(OPENDAQ_ERR_INVALID_OPERATION, "Locking strategy can not be changed after owner is already assigned!");

    this->lockingStrategy = strategy;
    IntegerPtr strategyIntPtr = static_cast<Int>(lockingStrategy);
    return OPENDAQ_SUCCESS;
}

template <typename PropObjInterface, typename ... Interfaces>
ErrCode GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::getLockingStrategy(LockingStrategy* strategy)
{
    OPENDAQ_PARAM_NOT_NULL(strategy);

    *strategy = this->lockingStrategy;
    return OPENDAQ_SUCCESS;
}

template <typename PropObjInterface, typename ... Interfaces>
ErrCode GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::getMutex(IMutex** mutex)
{
    OPENDAQ_PARAM_NOT_NULL(mutex);

    *mutex = this->sync.addRefAndReturn();
    return OPENDAQ_SUCCESS;
}

template <typename PropObjInterface, typename ... Interfaces>
ErrCode GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::getMutexOwner(IPropertyObjectInternal** owner)
{
    OPENDAQ_PARAM_NOT_NULL(owner);
    
    PropertyObjectInternalPtr ownerPtr = getPropertyObjectParent();
    if (lockingStrategy == LockingStrategy::OwnLock || !ownerPtr.assigned())
    {
        *owner = objPtr.asPtr<IPropertyObjectInternal>().addRefAndReturn();
        return OPENDAQ_SUCCESS;
    }

    return ownerPtr->getMutexOwner(owner);
}

template <class PropObjInterface, class... Interfaces>
ErrCode GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::serializeCustomValues(ISerializer* /*serializer*/, bool /*forUpdate*/)
{
    return OPENDAQ_SUCCESS;
}

template <class PropObjInterface, class... Interfaces>
ErrCode GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::serializePropertyValue(const StringPtr& name,
                                                                                           const ObjectPtr<IBaseObject>& value,
                                                                                           ISerializer* serializer,
                                                                                           bool forUpdate)
{
    if (value.assigned())
    {
        if (forUpdate)
        {
            if (const auto updatable = value.asPtrOrNull<IUpdatable>(true); updatable.assigned())
            {
                OPENDAQ_RETURN_IF_FAILED(serializer->keyStr(name));
                OPENDAQ_RETURN_IF_FAILED(updatable->serializeForUpdate(serializer));
                return OPENDAQ_SUCCESS;
            }
        }

        if (const auto serializable = value.asPtrOrNull<ISerializable>(true); serializable.assigned())
        {
            OPENDAQ_RETURN_IF_FAILED(serializer->keyStr(name));
            OPENDAQ_RETURN_IF_FAILED(serializable->serialize(serializer));
        }
    }
    else
    {
        OPENDAQ_RETURN_IF_FAILED(serializer->keyStr(name));
        OPENDAQ_RETURN_IF_FAILED(serializer->writeNull());
    }

    return OPENDAQ_SUCCESS;
}

template <class PropObjInterface, class... Interfaces>
ErrCode GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::serializeProperty(const PropertyPtr& property, ISerializer* serializer)
{
    const ErrCode errCode = daqTry([&property, &serializer]
    {
        ISerializable* serializable = property.as<ISerializable>(true);
        return serializable->serialize(serializer);
    });
    OPENDAQ_RETURN_IF_FAILED(errCode);
    return errCode;
}

template <class PropObjInterface, class... Interfaces>
ErrCode GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::serializePropertyValues(ISerializer* serializer, bool forUpdate)
{
    auto serializerPtr = SerializerPtr::Borrow(serializer);

    const auto numOfSerializablePropertyValues =
        std::count_if(propValues.begin(), propValues.end(), [](const std::pair<StringPtr, BaseObjectPtr>& keyValue) {
            return keyValue.second.supportsInterface<ISerializable>();
        });

    if (numOfSerializablePropertyValues == 0)
        return OPENDAQ_SUCCESS;

    serializer->key("propValues");
    serializer->startObject();
    {
        std::map<StringPtr, BaseObjectPtr> sorted(propValues.begin(), propValues.end());

        // Serialize properties with explicit order
        for (auto&& propName : customOrder)
        {
            auto propValue = sorted.find(propName);
            if (propValue != sorted.cend())
            {
                if (!hasUserReadAccess(serializerPtr.getUser(), propValue->second))
                    continue;

                ErrCode err = serializePropertyValue(propValue->first, propValue->second, serializer, forUpdate);
                OPENDAQ_RETURN_IF_FAILED(err);
                sorted.erase(propValue);
            }
        }

        // Serialize the rest of without set order
        for (auto&& propValue : sorted)
        {
            if (!hasUserReadAccess(serializerPtr.getUser(), propValue.second))
                continue;

            ErrCode err = serializePropertyValue(propValue.first, propValue.second, serializer, forUpdate);
            OPENDAQ_RETURN_IF_FAILED(err);
        }
    }

    serializer->endObject();

    return OPENDAQ_SUCCESS;
}

template <typename PropObjInterface, typename... Interfaces>
ErrCode GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::serializeLocalProperties(ISerializer* serializer)
{
    const ErrCode errCode = daqTry([&serializer, this]
    {
        if (localProperties.size() == 0)
            return OPENDAQ_NOTFOUND;

        auto serializerPtr = SerializerPtr::Borrow(serializer);

        if (!customOrder.empty())
        {
            serializerPtr.key("propertyOrder");
            serializerPtr.startList();
            for (const auto& group : customOrder)
                group.serialize(serializer);
            serializerPtr.endList();
        }

        serializerPtr.key("properties");
        serializerPtr.startList();
        for (const auto& prop : localProperties)
        {
            bool isObjectProp = prop.second.template asPtr<IPropertyInternal>().getValueTypeUnresolved() == ctObject;
            if (isObjectProp && !hasUserReadAccess(serializerPtr.getUser(), prop.second.getDefaultValue()))
                continue;

            const ErrCode errCode = serializeProperty(prop.second, serializer);
            OPENDAQ_RETURN_IF_FAILED(errCode);
        }
        serializerPtr.endList();

        return OPENDAQ_SUCCESS;
    });
    OPENDAQ_RETURN_IF_FAILED(errCode);
    return errCode;
}

template <class PropObjInterface, class... Interfaces>
ErrCode GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::serialize(ISerializer* serializer)
{
    auto serializerPtr = SerializerPtr::Borrow(serializer);
    Bool hasAccess = false;
    ErrCode serializeErrCode = hasUserReadAccess(serializerPtr.getUser(), &hasAccess);

    OPENDAQ_RETURN_IF_FAILED(serializeErrCode);
    if (!hasAccess)
        return DAQ_MAKE_ERROR_INFO(OPENDAQ_ERR_ACCESSDENIED);

    serializer->startTaggedObject(this);

    SERIALIZE_PROP_PTR(className)

    if (frozen)
    {
        serializer->key("frozen");
        serializer->writeBool(frozen);
    }

    serializeErrCode = serializeCustomValues(serializer, false);
    OPENDAQ_RETURN_IF_FAILED(serializeErrCode);

    serializeErrCode = serializePropertyValues(serializer, false);
    OPENDAQ_RETURN_IF_FAILED(serializeErrCode);

    serializeErrCode = serializeLocalProperties(serializer);
    OPENDAQ_RETURN_IF_FAILED(serializeErrCode);

    serializer->endObject();
    return OPENDAQ_SUCCESS;
}

template <typename PropObjInterface, typename... Interfaces>
template <class F>
PropertyObjectPtr GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::DeserializePropertyObject(
    const SerializedObjectPtr& serialized, const BaseObjectPtr& context, const FunctionPtr& factoryCallback, F&& f)
{
    StringPtr className;
    if (serialized.hasKey("className"))
        className = serialized.readString("className");

    Bool isFrozen{};
    if (serialized.hasKey("frozen"))
        isFrozen = serialized.readBool("frozen");

    PropertyObjectPtr propObjPtr = f(serialized, context, className);

    DeserializePropertyOrder(serialized, context, factoryCallback, propObjPtr);

    DeserializeLocalProperties(serialized, context, factoryCallback, propObjPtr);

    DeserializePropertyValues(serialized, context, factoryCallback, propObjPtr);

    if (isFrozen)
    {
        const auto freezable = propObjPtr.asPtrOrNull<IFreezable>(true);
        if (freezable.assigned())
            freezable.freeze();
    }

    return propObjPtr;
}

// static
template <class PropObjInterface, class... Interfaces>
ErrCode GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::Deserialize(ISerializedObject* serialized,
                                                                                IBaseObject* context,
                                                                                IFunction* factoryCallback,
                                                                                IBaseObject** obj)
{
    OPENDAQ_PARAM_NOT_NULL(serialized);
    OPENDAQ_PARAM_NOT_NULL(obj);

    const ErrCode errCode = daqTry([&serialized, &context, &factoryCallback, &obj]
    {
        *obj = DeserializePropertyObject(serialized,
                                         context,
                                         factoryCallback,
                                         [](const SerializedObjectPtr&, const BaseObjectPtr& context, const StringPtr& className) 
                                            {
                                                const TypeManagerPtr objManager = context.asOrNull<ITypeManager>();
                                                if (objManager.assigned())
                                                    return PropertyObject(objManager, className);
                                                return PropertyObject();
                                            }).detach();
    });
    OPENDAQ_RETURN_IF_FAILED(errCode);
    return errCode;
}

template <class PropObjInterface, class... Interfaces>
void GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::DeserializePropertyValues(const SerializedObjectPtr& serialized,
                                                                                           const BaseObjectPtr& context,
                                                                                           const FunctionPtr& factoryCallback,
                                                                                           PropertyObjectPtr& propObjPtr)
{
    const auto hasKeyStr = String("propValues");

    if (!serialized.hasKey(hasKeyStr))
        return;

    const auto propValues = serialized.readSerializedObject("propValues");

    const auto keys = propValues.getKeys();

    if (keys.getCount() == 0)
        return;

    const auto protectedPropObjPtr = propObjPtr.asPtr<IPropertyObjectProtected>(true);

    for (const auto& key : keys)
    {
        const auto propValue = propValues.readObject(key, context, factoryCallback);
        protectedPropObjPtr.setProtectedPropertyValue(key, propValue);
    }
}

template <class PropObjInterface, class... Interfaces>
void GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::DeserializePropertyOrder(const SerializedObjectPtr& serialized,
                                                                                          const BaseObjectPtr& context,
                                                                                          const FunctionPtr& /*factoryCallback*/,
                                                                                          PropertyObjectPtr& propObjPtr)
{
    const auto keyStr = String("propertyOrder");
    const auto hasKey = serialized.hasKey(keyStr);

    if (!IsTrue(hasKey))
        return;

    const auto propertyOrder = serialized.readList<IString>(keyStr, context);
    if (!propertyOrder.assigned())
        return;

    propObjPtr.setPropertyOrder(propertyOrder.toVector());
}

template <class PropObjInterface, class... Interfaces>
void GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::DeserializeLocalProperties(const SerializedObjectPtr& serialized,
                                                                                            const BaseObjectPtr& context,
                                                                                            const FunctionPtr& factoryCallback,
                                                                                            PropertyObjectPtr& propObjPtr)
{
    const auto keyStr = String("properties");

    const auto hasKey = serialized.hasKey(keyStr);

    if (!IsTrue(hasKey))
        return;

    const auto propertyList = serialized.readSerializedList(keyStr);

    for (SizeT i = 0; i < propertyList.getCount(); i++)
    {
        const PropertyPtr prop = propertyList.readObject(context, factoryCallback);
        const auto propName = prop.getName();

        if (!propObjPtr.hasProperty(propName))
        {
            propObjPtr.addProperty(prop);
        }
    }
}

template <class PropObjInterface, class... Interfaces>
ErrCode GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::getSerializeId(ConstCharPtr* id) const
{
    *id = SerializeId();
    return OPENDAQ_SUCCESS;
}

template <class PropObjInterface, class... Interfaces>
ErrCode GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::toString(CharPtr* str)
{
    if (str == nullptr)
    {
        return DAQ_MAKE_ERROR_INFO(OPENDAQ_ERR_ARGUMENT_NULL, "Parameter must not be null");
    }

    std::ostringstream stream;
    stream << "PropertyObject";

    if (className.assigned())
        stream << " {" << className << "}";

    return daqDuplicateCharPtr(stream.str().c_str(), str);
}

template <class PropObjInterface, class... Interfaces>
ConstCharPtr GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::SerializeId()
{
    return "PropertyObject";
}

template <class PropObjInterface, class... Interfaces>
WeakRefPtr<IPropertyObject> GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::getOwner() const
{
    return owner;
}

template <class PropObjInterface, class... Interfaces>
ErrCode GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::setOwner(IPropertyObject* newOwner)
{
    if (getPropertyObjectParent() == newOwner)
        return OPENDAQ_IGNORED;

    this->owner = newOwner;

    PermissionManagerPtr parentManager;
    if (newOwner != nullptr)
    {
        auto newOwnerPtr = PropertyObjectPtr::Borrow(newOwner);
        parentManager = newOwnerPtr.getPermissionManager();
        
        if (lockingStrategy == LockingStrategy::InheritLock)
        {
            PropertyObjectInternalPtr lockOwnerPtr;
            OPENDAQ_RETURN_IF_FAILED(getMutexOwner(&lockOwnerPtr));
            setLockOwner(lockOwnerPtr);
            setMutex(lockOwnerPtr.getMutex());
        }
    }

    this->permissionManager.template asPtr<IPermissionManagerInternal>(true).setParent(parentManager);
    return OPENDAQ_SUCCESS;
}

template <class PropObjInterface, typename... Interfaces>
ErrCode GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::freeze()
{
    if (frozen)
        return OPENDAQ_IGNORED;

    frozen = true;
    return OPENDAQ_SUCCESS;
}

template <class PropObjInterface, typename... Interfaces>
ErrCode GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::isFrozen(Bool* isFrozen) const
{
    OPENDAQ_PARAM_NOT_NULL(isFrozen);

    *isFrozen = frozen;
    return OPENDAQ_SUCCESS;
}

template <class PropObjInterface, class... Interfaces>
ErrCode GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::hasProperty(IString* propertyName, Bool* hasProperty)
{
    OPENDAQ_PARAM_NOT_NULL(propertyName);
    OPENDAQ_PARAM_NOT_NULL(hasProperty);

    auto propName = StringPtr::Borrow(propertyName);

    if (details::isChildProperty(propName))
    {
        PropertyObjectPtr parentObj;
        StringPtr leafName;
        const ErrCode err = getParentObject(propName, parentObj, leafName);
        if (err == OPENDAQ_ERR_NOTFOUND || err == OPENDAQ_ERR_NOINTERFACE)
        {
            daqClearErrorInfo();
            *hasProperty = False;
            return OPENDAQ_SUCCESS;
        }
        OPENDAQ_RETURN_IF_FAILED(err, fmt::format(R"(Failed to retrieve child object for property "{}")", propName));

        return parentObj->hasProperty(leafName, hasProperty);
    }
    
    if (localProperties.find(propertyName) != localProperties.cend())
    {
        *hasProperty = true;
        return OPENDAQ_SUCCESS;
    }

    if (objectClass.assigned())
    {
        try
        {
            *hasProperty = objectClass.hasProperty(propertyName);
            if (*hasProperty)
                return OPENDAQ_SUCCESS;
        }
        catch (...)
        {
        }
    }

    *hasProperty = False;
    return OPENDAQ_SUCCESS;
}

template <class PropObjInterface, class... Interfaces>
ErrCode GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::setPropertyFromSerialized(const StringPtr& propName,
                                                                                              const PropertyObjectPtr& propObj,
                                                                                              const SerializedObjectPtr& serialized,
                                                                                              const TypeManagerPtr& typeManager)
{
    if (!serialized.assigned())
    {
        return propObj->clearPropertyValue(propName);
    }

    BaseObjectPtr propValue;

    switch (serialized.getType(propName))
    {
        case ctBool:
            propValue = serialized.readBool(propName);
            break;
        case ctInt:
            propValue = serialized.readInt(propName);
            break;
        case ctFloat:
            propValue = serialized.readFloat(propName);
            break;
        case ctString:
            propValue = serialized.readString(propName);
            break;
        case ctList:
            propValue = serialized.readList<IBaseObject>(propName, typeManager);
            break;
        case ctDict:
        case ctRatio:
        case ctStruct:
        case ctObject:
        {
            const auto obj = propObj.getPropertyValue(propName);
            if (const auto updatable = obj.asPtrOrNull<IUpdatable>(true); updatable.assigned())
            {
                const auto serializedNestedObj = serialized.readSerializedObject(propName);
                return updatable->updateInternal(serializedNestedObj, typeManager);
            }

            propValue = serialized.readObject(propName, typeManager);
            break;
        }
        case ctProc:
        case ctBinaryData:
        case ctFunc:
        case ctComplexNumber:
        case ctEnumeration:
        case ctUndefined:
            return OPENDAQ_SUCCESS;
    }

    return propObj.as<IPropertyObjectProtected>(true)->setProtectedPropertyValue(propName, propValue);
}

template <typename PropObjInterface, typename... Interfaces>
void GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::endApplyProperties(const UpdatingActions& propsAndValues,
                                                                                    bool parentUpdating)
{
    auto list = List<IString>();
    auto dict = Dict<IString, IBaseObject>();
    for (const auto& [propName, action] : propsAndValues)
    {
        list.pushBack(propName);
        dict.set(propName, action.value);
    }

    if (endUpdateEvent.hasListeners())
    {
        auto args = EndUpdateEventArgs(list, parentUpdating);
        endUpdateEvent(objPtr, args);
    }

    if (dict.getCount() > 0)
        triggerCoreEventInternal(CoreEventArgsPropertyObjectUpdateEnd(objPtr, dict, path));
}

template <typename PropObjInterface, typename... Interfaces>
void GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::beginApplyProperties(const UpdatingActions& /* propsAndValues */,
                                                                                      bool /* parentUpdating */)
{
}

template <class PropObjInterface, class... Interfaces>
ErrCode GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::updateObjectProperties(const PropertyObjectPtr& propObj,
                                                                                           const SerializedObjectPtr& serialized,
                                                                                           const ListPtr<IProperty>& props)
{
    SerializedObjectPtr serializedProps;

    if (serialized.hasKey("propValues"))
        serializedProps = serialized.readSerializedObject("propValues");

    for (const auto& prop : props)
    {
        const auto propName = prop.getName();

        const auto propInternal = prop.asPtrOrNull<IPropertyInternal>(true);
        if (propInternal.assigned())
        {
            if (propInternal.getReferencedPropertyUnresolved().assigned())
                continue;
            const auto valueTypeUnresolved = propInternal.getValueTypeUnresolved();
            if (valueTypeUnresolved == CoreType::ctFunc || valueTypeUnresolved == CoreType::ctProc)
                continue;
        }

        if (!serializedProps.assigned() || !serializedProps.hasKey(propName))
        {
            const auto err = propObj.as<IPropertyObjectProtected>(true)->clearProtectedPropertyValue(propName);
            OPENDAQ_RETURN_IF_FAILED_EXCEPT(err, OPENDAQ_ERR_INVALID_OPERATION);
            continue;
        }

        auto typeManager = manager.assigned() ? manager.getRef() : nullptr;
        const auto err = setPropertyFromSerialized(propName, propObj, serializedProps, typeManager);
        OPENDAQ_RETURN_IF_FAILED(err);
    }

    return OPENDAQ_SUCCESS;
}

template <typename PropObjInterface, typename... Interfaces>
bool GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::hasUserReadAccess(const BaseObjectPtr& userContext,
                                                                                   const BaseObjectPtr& obj)
{
    if (!obj.assigned())
        return true;

    auto objPtr = obj.asPtrOrNull<IPropertyObject>(true);
    if (!objPtr.assigned())
        return true;

    auto userPtr = userContext.asPtrOrNull<IUser>(true);
    if (!userPtr.assigned())
        return true;

    return objPtr.getPermissionManager().isAuthorized(userPtr, Permission::Read);
}

template <class PropObjInterface, class... Interfaces>
ErrCode GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::updateInternal(ISerializedObject* obj, IBaseObject* /* context */)
{
    OPENDAQ_PARAM_NOT_NULL(obj);

    // Don't fail the upgrade if frozen just skip it
    // TODO: Check if upgrade should be allowed
    if (frozen)
        return OPENDAQ_IGNORED;

    const auto serialized = SerializedObjectPtr::Borrow(obj);

    ListPtr<IProperty> allProps;
    ErrCode errCode = getPropertiesInternal(True, False, &allProps, true);
    OPENDAQ_RETURN_IF_FAILED(errCode, "Failed to get properties");

    errCode = updateObjectProperties(this->thisInterface(), serialized, allProps);
    OPENDAQ_RETURN_IF_FAILED(errCode, "Failed to update object properties");
    return errCode;
}

template <class PropObjInterface, typename... Interfaces>
ErrCode GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::update(ISerializedObject* obj, IBaseObject* /* config */)
{
    if (frozen)
        return OPENDAQ_IGNORED;

    OPENDAQ_RETURN_IF_FAILED(beginUpdate());

    const ErrCode errCode = updateInternal(obj, nullptr);

    const ErrCode endUpdateErrCode = endUpdate();
    if (OPENDAQ_FAILED(endUpdateErrCode))
    {
        if (OPENDAQ_FAILED(errCode))
            daqClearErrorInfo();
        else
            OPENDAQ_RETURN_IF_FAILED(endUpdateErrCode);
    }
    return errCode;
}

template <typename PropObjInterface, typename... Interfaces>
ErrCode GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::serializeForUpdate(ISerializer* serializer)
{
    serializer->startTaggedObject(this);

    SERIALIZE_PROP_PTR(className)

    if (frozen)
    {
        serializer->key("frozen");
        serializer->writeBool(frozen);
    }

    ErrCode errCode = serializeCustomValues(serializer, true);
    OPENDAQ_RETURN_IF_FAILED(errCode);

    errCode = serializePropertyValues(serializer, true);
    OPENDAQ_RETURN_IF_FAILED(errCode);

    serializer->endObject();
    return OPENDAQ_SUCCESS;
}

template <typename PropObjInterface, typename... Interfaces>
ErrCode GenericPropertyObjectImpl<PropObjInterface, Interfaces...>::updateEnded(IBaseObject* context)
{
    auto contextPtr = BaseObjectPtr::Borrow(context);
    const ErrCode errCode = daqTry([this, &contextPtr] { onUpdatableUpdateEnd(contextPtr); });
    OPENDAQ_RETURN_IF_FAILED(errCode);
    return errCode;
}

OPENDAQ_REGISTER_DESERIALIZE_FACTORY(PropertyObjectImpl)

END_NAMESPACE_OPENDAQ
