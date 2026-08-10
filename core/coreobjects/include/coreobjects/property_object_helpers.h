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
#include <coretypes/stringobject_factory.h>
#include <cstring>
#include <cstdlib>

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

}

END_NAMESPACE_OPENDAQ
