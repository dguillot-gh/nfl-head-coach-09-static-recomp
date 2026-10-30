#pragma once

// std::atomic_ref, or a stand-in where the C++ library lacks it (the Android NDK's libc++ as of r28).
// The stand-in covers what the kernel uses on guest words: load, store and compare-exchange.

#include <atomic>

namespace hc09
{
#if defined(__cpp_lib_atomic_ref)
    template <typename T>
    using AtomicRef = std::atomic_ref<T>;
#else
    template <typename T>
    class AtomicRef
    {
    public:
        explicit AtomicRef(T& value) : value_(&value) {}

        T load(std::memory_order order = std::memory_order_seq_cst) const
        {
            return __atomic_load_n(value_, Order(order));
        }

        void store(T desired, std::memory_order order = std::memory_order_seq_cst) const
        {
            __atomic_store_n(value_, desired, Order(order));
        }

        bool compare_exchange_strong(T& expected, T desired, std::memory_order order = std::memory_order_seq_cst) const
        {
            return __atomic_compare_exchange_n(value_, &expected, desired, false, Order(order), FailureOrder(order));
        }

        bool compare_exchange_weak(T& expected, T desired, std::memory_order order = std::memory_order_seq_cst) const
        {
            return __atomic_compare_exchange_n(value_, &expected, desired, true, Order(order), FailureOrder(order));
        }

    private:
        static constexpr int Order(std::memory_order order)
        {
            switch (order)
            {
            case std::memory_order_relaxed: return __ATOMIC_RELAXED;
            case std::memory_order_consume: return __ATOMIC_CONSUME;
            case std::memory_order_acquire: return __ATOMIC_ACQUIRE;
            case std::memory_order_release: return __ATOMIC_RELEASE;
            case std::memory_order_acq_rel: return __ATOMIC_ACQ_REL;
            default: return __ATOMIC_SEQ_CST;
            }
        }

        // A failed compare-exchange only loads, so it cannot use release ordering.
        static constexpr int FailureOrder(std::memory_order order)
        {
            switch (order)
            {
            case std::memory_order_release: return __ATOMIC_RELAXED;
            case std::memory_order_acq_rel: return __ATOMIC_ACQUIRE;
            default: return Order(order);
            }
        }

        T* value_;
    };
#endif
}
