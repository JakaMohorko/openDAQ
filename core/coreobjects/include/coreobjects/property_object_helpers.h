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
#include <coreobjects/exceptions.h>
#include <coreobjects/eval_value_ptr.h>
#include <coreobjects/property_object.h>
#include <coreobjects/property_object_ptr.h>
#include <coreobjects/property_ptr.h>
#include <coreobjects/property_internal_ptr.h>
#include <coretypes/coretypes.h>
#include <stdexcept>
#include <coretypes/stringobject_factory.h>
#include <coretypes/enumeration_factory.h>
#include <coretypes/inspectable_ptr.h>
#include <coretypes/struct_ptr.h>
#include <coretypes/validation.h>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <limits>

BEGIN_NAMESPACE_OPENDAQ

// Stateless helpers used by GenericPropertyObjectImpl. Internal to openDAQ; not part of the public API surface.
namespace details
{

struct PropertyNameInfo
{
    StringPtr name;
    Int index{};
};

#if defined(__GNUC__) && __GNUC__ >= 12
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wdangling-pointer"
#endif

// Child property handling - Used when a property is queried in the "parent.child" format

inline bool isChildProperty(const StringPtr& name)
{
    auto chr = strchr(name.getCharPtr(), '.');
    return chr != nullptr;
}

inline void splitOnFirstDot(const StringPtr& input, StringPtr& head, StringPtr& tail)
{
    const std::string inputStr = input;
    head = input;

    size_t pos = inputStr.find('.');
    if (pos == std::string::npos)
        return;

    head = inputStr.substr(0, pos);
    tail = inputStr.substr(pos + 1);
}

inline void splitOnLastDot(const StringPtr& input, StringPtr& head, StringPtr& tail)
{
    const std::string inputStr = input;
    head = input;

    size_t pos = inputStr.rfind('.');
    if (pos == std::string::npos)
        return;

    head = inputStr.substr(0, pos);
    tail = inputStr.substr(pos + 1);
}

// Gets the index integer value between two square brackets
inline int parseIndex(char const* lBracket)
{
    auto last = strchr(lBracket, ']');
    if (last != nullptr)
    {
        char* end;
        int index = strtol(lBracket + 1, &end, 10);

        if (end != last)
        {
            DAQ_THROW_EXCEPTION(InvalidParameterException, "Could not parse the property index.");
        }

        return index;
    }
    DAQ_THROW_EXCEPTION(InvalidParameterException, "No matching ] found.");
}

inline PropertyNameInfo getPropertyNameInfo(const StringPtr& name)
{
    PropertyNameInfo nameInfo;

    auto propNameData = name.getCharPtr();
    auto first = strchr(propNameData, '[');
    if (first != nullptr)
    {
        nameInfo.index = parseIndex(first);
        nameInfo.name = String(propNameData, first - propNameData);
    }
    else
    {
        nameInfo.index = -1;
        nameInfo.name = name;
    }

    return nameInfo;
}

// Gets the property name without the index as the `propName` output parameter
// Returns the index in the form of [index], eg. [0]
inline ConstCharPtr getPropNameWithoutIndex(const StringPtr& name, StringPtr& propName)
{
    auto propNameData = name.getCharPtr();
    auto first = strchr(propNameData, '[');

    if (first == nullptr)
    {
        propName = String(propNameData);
    }
    else
    {
        propName = String(propNameData, first - propNameData);
    }
    return first;
}

#if defined(__GNUC__) && __GNUC__ >= 12
    #pragma GCC diagnostic pop
#endif

// Checks if the value is a container type, or base `IPropertyObject`. Only such values can be set in `setProperty`
inline ErrCode checkContainerType(const PropertyPtr& prop, const BaseObjectPtr& value)
{
    if (!value.assigned())
        return OPENDAQ_SUCCESS;

    auto coreType = value.getCoreType();
    if (coreType == ctObject)
    {
        auto inspect = value.asPtrOrNull<IInspectable>(true);
        if (inspect.assigned() && !inspect.getInterfaceIds().empty())
        {
            return inspect.getInterfaceIds()[0] == IPropertyObject::Id;
        }

        return DAQ_MAKE_ERROR_INFO(OPENDAQ_ERR_INVALIDTYPE, "Only base Property Object object-type values are allowed");
    }

    auto iterate = [](const IterablePtr<IBaseObject>& it, CoreType type)
    {
        for (const auto& key : it)
        {
            if (key.getCoreType() != type)
                return false;
        }
        return true;
    };

    const auto propInternal = prop.asPtr<IPropertyInternal>(true);
    if (coreType == ctDict)
    {
        const auto dict = value.asPtr<IDict>();
        const auto keyType = propInternal.getKeyTypeNoLock();
        const auto itemType = propInternal.getItemTypeNoLock();

        IterablePtr<IBaseObject> it;
        dict->getKeys(&it);
        if (!iterate(it, keyType))
            return DAQ_MAKE_ERROR_INFO(OPENDAQ_ERR_INVALIDTYPE, fmt::format(R"(Invalid dictionary key type for property "{}")", prop.getName()));

        dict->getValues(&it);
        if (!iterate(it, itemType))
            return DAQ_MAKE_ERROR_INFO(OPENDAQ_ERR_INVALIDTYPE, fmt::format(R"(Invalid dictionary item type for property "{}")", prop.getName()));
    }
    else if (coreType == ctList)
    {
        const auto itemType = propInternal.getItemTypeNoLock();

        if (itemType != ctUndefined && !iterate(value, itemType))
            return DAQ_MAKE_ERROR_INFO(OPENDAQ_ERR_INVALIDTYPE, fmt::format(R"(Invalid list item type for property "{}")", prop.getName()));
    }

    return OPENDAQ_SUCCESS;
}

// Checks if the property is a struct type, and checks its fields for type/name compatibility
inline ErrCode checkStructType(const PropertyPtr& prop, const BaseObjectPtr& value)
{
    if (prop.getValueType() != ctStruct)
        return OPENDAQ_SUCCESS;

    auto structPtr = value.asPtrOrNull<IStruct>();
    if (!structPtr.assigned())
        return DAQ_MAKE_ERROR_INFO(OPENDAQ_ERR_INVALIDSTATE, fmt::format(R"(Set value is not a struct for property "{}")", prop.getName()));

    StructTypePtr structType = prop.asPtr<IPropertyInternal>().getStructTypeNoLock();
    StructTypePtr valueStructType = structPtr.getStructType();

    if (structType != valueStructType)
        return DAQ_MAKE_ERROR_INFO(OPENDAQ_ERR_INVALIDSTATE, fmt::format(R"(Set value StructureType is different from the default for property "{}")", prop.getName()));

    return OPENDAQ_SUCCESS;
}

// Checks if the property is a enumeration type and checks for type/name compatibility
inline ErrCode checkEnumerationType(const PropertyPtr& prop, const BaseObjectPtr& value)
{
    const auto propInternal = prop.asPtr<IPropertyInternal>();
    if (propInternal.getValueTypeNoLock() != ctEnumeration)
        return OPENDAQ_SUCCESS;

    auto enumerationPtr = value.asPtrOrNull<IEnumeration>();
    if (!enumerationPtr.assigned())
        return DAQ_MAKE_ERROR_INFO(OPENDAQ_ERR_INVALIDSTATE, fmt::format(R"(Set value is not an enumeration for property "{}")", prop.getName()));

    auto propEnumerationPtr = propInternal.getDefaultValueNoLock().asPtrOrNull<IEnumeration>();
    if (!propEnumerationPtr.assigned())
        return DAQ_MAKE_ERROR_INFO(OPENDAQ_ERR_INVALIDSTATE, fmt::format(R"(Property default value is not an enumeration for property "{}")", prop.getName()));

    EnumerationTypePtr valueEnumerationType = enumerationPtr.getEnumerationType();
    EnumerationTypePtr propEnumerationType = propEnumerationPtr.getEnumerationType();

    if (propEnumerationType != valueEnumerationType)
        return DAQ_MAKE_ERROR_INFO(OPENDAQ_ERR_INVALIDSTATE, fmt::format(R"(Set value EnumerationType is different from the default for property "{}")", prop.getName()));

    return OPENDAQ_SUCCESS;
}

// Checks if value is a correct key into the list/dictionary of selection values
inline ErrCode checkSelectionValues(const PropertyPtr& prop, const BaseObjectPtr& value)
{
    const auto selectionValues = prop.asPtr<IPropertyInternal>(true).getSelectionValuesNoLock();
    if (selectionValues.assigned())
    {
        const PropertyType propType = prop.getPropertyType();
        if (propType == PropertyType::IndexSelection)
        {
            if (const auto list = selectionValues.asPtrOrNull<IList>(true); list.assigned())
            {
                const SizeT key = value;
                if (key < list.getCount())
                    return OPENDAQ_SUCCESS;
            }
        }
        else if (propType == PropertyType::Selection)
        {
            if (const auto list = selectionValues.asPtrOrNull<IList>(true); list.assigned())
            {
                if (prop.getValueType() == ctFloat)
                {
                    const double valueDouble = value;
                    const double preScale =  std::max({1.0, std::abs(valueDouble)});
                    for (const double& item : list)
                    {
                        const double scale = std::max({preScale, std::abs(item)});
                        if (std::abs(item - valueDouble) <= std::numeric_limits<double>::epsilon() * scale)
                            return OPENDAQ_SUCCESS;
                    }
                }
                else
                {
                    for (const auto& item : list)
                    {
                        if (item == value)
                            return OPENDAQ_SUCCESS;
                    }
                }
            }
        }
        else if (propType == PropertyType::SparseSelection)
        {
            if (const auto dict = selectionValues.asPtrOrNull<IDict>(true); dict.assigned() && dict.hasKey(value))
                return OPENDAQ_SUCCESS;
        }
        return DAQ_MAKE_ERROR_INFO(OPENDAQ_ERR_NOTFOUND, fmt::format(R"(Value is not a key/index of selection values for property "{}")", prop.getName()));
    }

    return OPENDAQ_SUCCESS;
}

// Checks if property and value type match. If not, attempts to convert the value
inline ErrCode checkPropertyTypeAndConvert(const PropertyPtr& prop, BaseObjectPtr& value)
{
    if (!prop.assigned() || !value.assigned())
        return OPENDAQ_SUCCESS;

    if (value.supportsInterface<IEvalValue>())
        return OPENDAQ_SUCCESS;

    const ErrCode errCode = daqTry([&]()
    {
        const auto propInternal = prop.asPtr<IPropertyInternal>();
        const auto propCoreType = propInternal.getValueTypeNoLock();
        const auto valueCoreType = value.getCoreType();

        if (propCoreType != valueCoreType)
        {
            if (propCoreType == ctEnumeration)
            {
                const auto enumVal = propInternal.getDefaultValueNoLock().asPtrOrNull<IEnumeration>();
                if (!enumVal.assigned())
                    return DAQ_MAKE_ERROR_INFO(OPENDAQ_ERR_INVALIDSTATE,
                                               fmt::format(R"(Default value of enumeration property {} is not assigned)", prop.getName()));

                const auto type = enumVal.getEnumerationType();
                const Int intVal = value.convertTo(ctInt);
                value = EnumerationWithIntValueAndType(type, intVal);
            }
            else
                value = value.convertTo(propCoreType);
        }
        return OPENDAQ_SUCCESS;
    });

    OPENDAQ_RETURN_IF_FAILED(errCode, fmt::format(R"(Value type is different than Property type and conversion failed for property "{}")", prop.getName()));
    return errCode;
}

// Coercion/Validation

inline void coercePropertyWrite(const PropertyPtr& prop, ObjectPtr<IBaseObject>& valuePtr, const PropertyObjectPtr& objPtr)
{
    if (prop.assigned() && valuePtr.assigned())
    {
        const auto coercer = prop.asPtr<IPropertyInternal>().getCoercerNoLock();
        if (coercer.assigned())
        {
            try
            {
                valuePtr = coercer.coerceNoLock(objPtr, valuePtr);
            }
            catch (const DaqException&)
            {
                throw;
            }
            catch (...)
            {
                DAQ_THROW_EXCEPTION(CoerceFailedException);
            }
        }
    }
}

inline void validatePropertyWrite(const PropertyPtr& prop, ObjectPtr<IBaseObject>& valuePtr, const PropertyObjectPtr& objPtr)
{
    if (prop.assigned() && valuePtr.assigned())
    {
        const auto validator = prop.asPtr<IPropertyInternal>().getValidatorNoLock();
        if (validator.assigned())
        {
            try
            {
                validator.validateNoLock(objPtr, valuePtr);
            }
            catch (const DaqException&)
            {
                throw;
            }
            catch (...)
            {
                DAQ_THROW_EXCEPTION(ValidateFailedException);
            }
        }
    }
}

inline void coerceMinMax(const PropertyPtr& prop, ObjectPtr<IBaseObject>& valuePtr)
{
    if (!prop.assigned() || !valuePtr.assigned())
        return;

    const auto propInternal = prop.asPtr<IPropertyInternal>();
    const auto min = propInternal.getMinValueNoLock();
    if (min.assigned())
    {
        try
        {
            if (valuePtr < min)
                valuePtr = min;
        }
        catch (...)
        {
        }
    }

    const auto max = propInternal.getMaxValueNoLock();
    if (max.assigned())
    {
        try
        {
            if (valuePtr > max)
                valuePtr = max;
        }
        catch (...)
        {
        }
    }
}

// Reference property handling

inline PropertyPtr checkForRefPropAndGetBoundProp(PropertyPtr& prop, const PropertyObjectPtr& objPtr, bool* isReferenced = nullptr)
{
    if (!prop.assigned())
    {
        return prop;
    }

    PropertyInternalPtr boundProp = prop.asPtr<IPropertyInternal>(true).cloneWithOwner(objPtr);
    auto refProp = boundProp.getReferencedPropertyNoLock();
    if (refProp.assigned())
    {
        CoreType ct = refProp.getCoreType();

        if (ct != ctObject)
            throw std::invalid_argument("Invalid reference to property");

        if (isReferenced)
            *isReferenced = true;

        return checkForRefPropAndGetBoundProp(refProp, objPtr);
    }

    if (isReferenced)
        *isReferenced = false;
    return boundProp;
}

// Checks whether the property is a reference property that references an already referenced property
inline bool hasDuplicateReferences(const PropertyPtr& prop, const PropertyObjectPtr& objPtr)
{
    auto refEval = prop.asPtr<IPropertyInternal>().getReferencedPropertyUnresolved();
    if (refEval.assigned())
    {
        auto refNames = refEval.getPropertyReferences();
        for (auto refPropName : refNames)
        {
            if (objPtr.hasProperty(refPropName) && objPtr.getProperty(refPropName).getIsReferenced())
                return true;
        }
    }

    return false;
}

inline Bool checkIsReferenced(const StringPtr& referencedPropName, const PropertyInternalPtr& prop)
{
    const auto refProp = prop.getReferencedPropertyUnresolved();
    if (!refProp.assigned())
        return false;

    for (auto propName : refProp.getPropertyReferences())
    {
        if (propName == referencedPropName)
        {
            return true;
        }
    }

    return false;
}

}

END_NAMESPACE_OPENDAQ
