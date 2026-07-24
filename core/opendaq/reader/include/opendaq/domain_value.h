/*
 * Copyright 2022-2025 openDAQ d.o.o.
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

#include <coretypes/ratio_ptr.h>
#include <opendaq/range_type.h>
#include <opendaq/reader_utils.h>

#include <chrono>
#include <iostream>
#include <memory>

BEGIN_NAMESPACE_OPENDAQ

/// Plain-value tick resolution (num/den); replaces RatioPtr in the domain-value arithmetic so
/// the hot conversion paths carry no refcounted object. {0, 0} means "not assigned".
struct TickResolution
{
    Int num = 0;
    Int den = 0;

    TickResolution() = default;

    TickResolution(Int num, Int den)
        : num(num)
        , den(den)
    {
    }

    // Convenience conversion from the descriptor's RatioPtr; an unassigned ratio yields {0, 0}.
    TickResolution(const RatioPtr& ratio)
    {
        if (ratio.assigned())
        {
            num = ratio.getNumerator();
            den = ratio.getDenominator();
        }
    }

    friend bool operator==(const TickResolution& lhs, const TickResolution& rhs)
    {
        return lhs.num == rhs.num && lhs.den == rhs.den;
    }

    friend bool operator!=(const TickResolution& lhs, const TickResolution& rhs)
    {
        return !(lhs == rhs);
    }
};

struct DomainInfo
{
    std::chrono::system_clock::time_point epoch;
    TickResolution resolution;

    static DomainInfo fromDescriptor(const DataDescriptorPtr& descriptor)
    {
        if (!descriptor.assigned())
            DAQ_THROW_EXCEPTION(ArgumentNullException, "Descriptor must not be null");

        auto epoch = daq::reader::parseEpoch(descriptor.getOrigin());
        auto resolution = descriptor.getTickResolution();

        return {epoch, resolution};
    }

    friend bool operator==(const DomainInfo& lhs, const DomainInfo& rhs)
    {
        // {0, 0} is the unassigned sentinel (see TickResolution)
        if (lhs.resolution == TickResolution{} || rhs.resolution == TickResolution{})
            DAQ_THROW_EXCEPTION(InvalidParameterException, "DomainInfo::resolution must be assigned.");

        return lhs.epoch == rhs.epoch && lhs.resolution == rhs.resolution;
    }

    friend bool operator!=(const DomainInfo& lhs, const DomainInfo& rhs)
    {
        return !(lhs == rhs);
    }
};

inline std::ostream& operator<<(std::ostream& os, const DomainInfo& info)
{
    os << "DomainInfo{"
       << "epoch=" << info.epoch.time_since_epoch().count() << ", resolution=" << info.resolution.num << "/"
       << info.resolution.den << "}";

    return os;
}

namespace domain_conversion
{
    /// Epoch difference (from - to) expressed in ticks of @p resolution, truncated toward
    /// zero (the sub-tick remainder of an epoch is not representable on the tick grid).
    inline Int epochOffsetTicks(const std::chrono::system_clock::time_point& from,
                                const std::chrono::system_clock::time_point& to,
                                const TickResolution& resolution)
    {
        using SysPeriod = std::chrono::system_clock::period;
        const Int epochDiff = from.time_since_epoch().count() - to.time_since_epoch().count();
        const Int scaleNumerator = SysPeriod::num * resolution.den;
        const Int scaleDenominator = SysPeriod::den * resolution.num;
        return epochDiff * scaleNumerator / scaleDenominator;
    }

    /// tick_target = tick_source * numerator / denominator
    struct TickMultiplier
    {
        Int numerator;
        Int denominator;
    };

    inline TickMultiplier tickMultiplier(const TickResolution& sourceResolution, const TickResolution& targetResolution)
    {
        return {sourceResolution.num * targetResolution.den,
                sourceResolution.den * targetResolution.num};
    }
}  // namespace domain_conversion

class DomainValue
{
public:
    explicit DomainValue(const DomainInfo& info)
        : domain(info)
    {
    }
    virtual ~DomainValue() = default;

    const DomainInfo& getDomain() const
    {
        return domain;
    }

    /**
     * @brief This value re-expressed in @p targetDomain, rounding the scaled tick to the
     * nearest target tick and truncating the epoch offset in target ticks. Entering a
     * common domain whose resolution folds this one is exact (spec section 4.1).
     */
    virtual std::unique_ptr<DomainValue> toDomain(const DomainInfo& targetDomain) = 0;

    /**
     * @brief The inverse pairing of toDomain: this value re-expressed in @p targetDomain,
     * with the epoch offset truncated in THIS domain's ticks before scaling. The alignment
     * loop uses it to map a common-domain candidate back onto an input's grid; the two
     * directions keep their historical rounding order, so round trips through sub-tick
     * epoch remainders stay bit-identical to the original implementation.
     */
    virtual std::unique_ptr<DomainValue> fromDomain(const DomainInfo& targetDomain) = 0;

    virtual void roundUpOnDomainInterval(const RatioPtr& interval) = 0;

    /// Shift the value by a whole number of ticks of its own domain (used to anchor
    /// grid rounding at an arbitrary phase, spec section 5).
    virtual void shiftTicks(std::int64_t delta) = 0;

    /**
     * @brief System-clock time this value represents (epoch + tick * resolution).
     * Used for synchronization-distance diagnostics; not for tick-exact comparisons.
     */
    virtual std::chrono::system_clock::time_point toAbsoluteTime() const = 0;

#if !defined(NDEBUG)
    virtual std::string asTime() const = 0;
#endif

    friend bool operator<(const DomainValue& lhs, const DomainValue& rhs)
    {
        return lhs.compare(rhs) < 0;
    }
    friend bool operator>(const DomainValue& lhs, const DomainValue& rhs)
    {
        return lhs.compare(rhs) > 0;
    }
    friend bool operator==(const DomainValue& lhs, const DomainValue& rhs)
    {
        return lhs.compare(rhs) == 0;
    }
    friend bool operator!=(const DomainValue& lhs, const DomainValue& rhs)
    {
        return lhs.compare(rhs) != 0;
    }

protected:
    /// A domain value is only meaningful relative to an epoch and resolution; carrying the
    /// DomainInfo keeps values self-describing so compare/toDomain need no external
    /// bookkeeping (#12)
    DomainInfo domain;

private:
    virtual int compare(const DomainValue& other) const = 0;
};

template <typename Type>
class DomainValueImpl : public DomainValue
{
public:
    explicit DomainValueImpl(const DomainInfo& info, Type value)
        : DomainValue(info)
        , value(value)
    {
    }

    ~DomainValueImpl() override = default;

    std::unique_ptr<DomainValue> toDomain(const DomainInfo& targetDomain) override
    {
        const Int offsetTicks = domain_conversion::epochOffsetTicks(domain.epoch, targetDomain.epoch, targetDomain.resolution);
        const auto multiplier = domain_conversion::tickMultiplier(domain.resolution, targetDomain.resolution);

        Type valueScaled = 0;
        if constexpr (std::is_integral_v<Type>)
        {
            // Round to the closest tick
            valueScaled = static_cast<Type>((value / multiplier.denominator) * multiplier.numerator +
                                            (2 * (value % multiplier.denominator) * multiplier.numerator + multiplier.denominator) /
                                                (2 * multiplier.denominator));
        }
        else
        {
            valueScaled = static_cast<Type>(value * multiplier.numerator / static_cast<double>(multiplier.denominator));
        }

        return std::make_unique<DomainValueImpl<Type>>(targetDomain, static_cast<Type>(offsetTicks + valueScaled));
    }

    std::unique_ptr<DomainValue> fromDomain(const DomainInfo& targetDomain) override
    {
        // Epoch offset truncated in THIS domain's ticks, subtracted before scaling - see
        // the base-class contract for why the two directions round differently
        const Int offsetTicks = domain_conversion::epochOffsetTicks(targetDomain.epoch, domain.epoch, domain.resolution);
        const auto multiplier = domain_conversion::tickMultiplier(targetDomain.resolution, domain.resolution);

        const Type valueShifted = static_cast<Type>(value - offsetTicks);
        Type targetValue = 0;
        if constexpr (std::is_integral_v<Type>)
        {
            // Round to the closest tick
            targetValue = static_cast<Type>((valueShifted / multiplier.numerator) * multiplier.denominator +
                                            (2 * (valueShifted % multiplier.numerator) * multiplier.denominator + multiplier.numerator) /
                                                (2 * multiplier.numerator));
        }
        else
        {
            targetValue = static_cast<Type>(valueShifted * multiplier.denominator / static_cast<double>(multiplier.numerator));
        }

        return std::make_unique<DomainValueImpl<Type>>(targetDomain, targetValue);
    }

    void roundUpOnDomainInterval(const RatioPtr& interval) override
    {
        auto num = domain.resolution.num * interval.getDenominator();
        auto den = domain.resolution.den * interval.getNumerator();

        const Int gcd = std::gcd(num, den);
        num /= gcd;
        den /= gcd;

        if (den % num != 0)  // 1 = k * num/den, the resolution is a fractional divider of a unit
            DAQ_THROW_EXCEPTION(NotSupportedException, "Resolution must be aligned on full unit of domain");

        value = static_cast<Type>((((value * num + den - 1) / den) * den) / num);
    }

    void shiftTicks(std::int64_t delta) override
    {
        value = static_cast<Type>(value + delta);
    }

    std::chrono::system_clock::time_point toAbsoluteTime() const override
    {
        return reader::toSysTime(value, domain.epoch, domain.resolution.num, domain.resolution.den);
    }

    Type getValue() const
    {
        return value;
    }

#if !defined(NDEBUG)
    virtual std::string asTime() const override
    {
        using namespace reader;

        std::stringstream ss;
        ss << toSysTime(value, domain.epoch, domain.resolution.num, domain.resolution.den);

        return ss.str();
    }
#endif

    int compare(const DomainValue& other) const override
    {
        const auto* otherImpl = dynamic_cast<const DomainValueImpl<Type>*>(&other);
        if (otherImpl == nullptr)
        {
            DAQ_THROW_EXCEPTION(InvalidParameterException, "Both DomainValue objects must be of the same type!");
        }
        if (otherImpl->domain != this->domain)
            DAQ_THROW_EXCEPTION(InvalidParameterException, "Have to compare DomainValue objects in the same domain!");

        if (this->value > otherImpl->value)
            return 1;
        else if (this->value == otherImpl->value)
            return 0;
        else  // this->value < otherImpl->value
            return -1;
    }

private:
    Type value;
};

template <>
class DomainValueImpl<RangeType64> final : public DomainValue
{
public:
    using RangeValue = RangeType64::Type;

    explicit DomainValueImpl(const DomainInfo& info, RangeType64 value)
        : DomainValue(info)
        , value(value)
    {
    }
    std::unique_ptr<DomainValue> toDomain(const DomainInfo& targetDomain) override
    {
        const Int offsetTicks = domain_conversion::epochOffsetTicks(domain.epoch, targetDomain.epoch, targetDomain.resolution);
        const auto multiplier = domain_conversion::tickMultiplier(domain.resolution, targetDomain.resolution);

        const auto scale = [&multiplier](RangeValue tick)
        {
            return static_cast<RangeValue>((tick / multiplier.denominator) * multiplier.numerator +
                                           (tick % multiplier.denominator) * multiplier.numerator / multiplier.denominator);
        };

        const RangeValue start = offsetTicks + scale(value.start);
        const RangeValue end = value.end == -1 ? static_cast<RangeValue>(-1) : offsetTicks + scale(value.end);

        return std::make_unique<DomainValueImpl<RangeType64>>(targetDomain, RangeType64{start, end});
    }

    std::unique_ptr<DomainValue> fromDomain(const DomainInfo& targetDomain) override
    {
        const Int offsetTicks = domain_conversion::epochOffsetTicks(targetDomain.epoch, domain.epoch, domain.resolution);
        const auto multiplier = domain_conversion::tickMultiplier(targetDomain.resolution, domain.resolution);

        const auto scale = [&multiplier](RangeValue tick)
        {
            return static_cast<RangeValue>((tick / multiplier.numerator) * multiplier.denominator +
                                           (tick % multiplier.numerator) * multiplier.denominator / multiplier.numerator);
        };

        const RangeValue start = scale(value.start - offsetTicks);
        const RangeValue end = value.end == -1 ? static_cast<RangeValue>(-1) : scale(value.end - offsetTicks);

        return std::make_unique<DomainValueImpl<RangeType64>>(targetDomain, RangeType64{start, end});
    }

    void roundUpOnDomainInterval(const RatioPtr& interval) override
    {
        DAQ_THROW_EXCEPTION(NotSupportedException);
    }

    void shiftTicks(std::int64_t delta) override
    {
        value.start += delta;
        if (value.end != -1)
            value.end += delta;
    }

    std::chrono::system_clock::time_point toAbsoluteTime() const override
    {
        return reader::toSysTime(value.start, domain.epoch, domain.resolution.num, domain.resolution.den);
    }

    RangeType64 getValue() const
    {
        return value;
    }

#if !defined(NDEBUG)
    virtual std::string asTime() const override
    {
        using namespace reader;

        std::stringstream ss;
        ss << toSysTime(value.start, domain.epoch, domain.resolution.num, domain.resolution.den);

        return ss.str();
    }
#endif

    int compare(const DomainValue& other) const override
    {
        const auto* otherImpl = dynamic_cast<const DomainValueImpl<RangeType64>*>(&other);
        if (otherImpl == nullptr)
        {
            DAQ_THROW_EXCEPTION(InvalidParameterException, "Both DomainValue objects must be of the same type!");
        }
        if (otherImpl->domain != this->domain)
            DAQ_THROW_EXCEPTION(InvalidParameterException, "Have to compare DomainValue objects in the same domain!");

        if (this->value.start > otherImpl->value.start)
            return 1;
        else if (this->value.start == otherImpl->value.start)
            return 0;
        else  // this->value.start < otherImpl->value.start
            return -1;
    }

private:
    RangeType64 value;
};

/// Only integral scalars and RangeType64 make sense as domain values; the reading
/// utilities reject every other sample type before instantiating DomainValueImpl (#15),
/// so no throwing specializations are needed.
template <typename Type>
inline constexpr bool isDomainValueType =
    std::is_integral_v<Type> || std::is_same_v<Type, RangeType64> || std::is_floating_point_v<Type>;

END_NAMESPACE_OPENDAQ
