//==------- group_helpers_sorters.hpp - SYCL sorters and group helpers -----==//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#pragma once

#if (!defined(_HAS_STD_BYTE) || _HAS_STD_BYTE != 0)

#include <sycl/aliases.hpp>             // for half
#include <sycl/builtins.hpp>            // for min
#include <sycl/detail/pi.h>             // for PI_ERROR_INVALID_DEVICE
#include <sycl/exception.hpp>           // for sycl_category, exception
#include <sycl/ext/oneapi/bfloat16.hpp> // for bfloat16
#include <sycl/memory_enums.hpp>        // for memory_scope
#include <sycl/range.hpp>               // for range
#include <sycl/sycl_span.hpp>           // for span

#ifdef __SYCL_DEVICE_ONLY__
#include <sycl/detail/group_sort_impl.hpp>
#endif

#include <bitset>       // for bitset
#include <cstddef>      // for size_t, byte
#include <functional>   // for less, greater
#include <limits.h>     // for CHAR_BIT
#include <limits>       // for numeric_limits
#include <stdint.h>     // for uint32_t
#include <system_error> // for error_code
#include <type_traits>  // for is_same, is_arithmetic

namespace sycl {
inline namespace _V1 {
namespace ext::oneapi::experimental {

// ---- group helpers
template <typename Group, size_t Extent> class group_with_scratchpad {
  Group g;
  sycl::span<std::byte, Extent> scratch;

public:
  group_with_scratchpad(Group g_, sycl::span<std::byte, Extent> scratch_)
      : g(g_), scratch(scratch_) {}
  Group get_group() const { return g; }
  sycl::span<std::byte, Extent> get_memory() const { return scratch; }
};

// ---- sorters
template <typename Compare = std::less<>> class default_sorter {
  Compare comp;
  sycl::span<std::byte> scratch;

public:
  template <size_t Extent>
  default_sorter(sycl::span<std::byte, Extent> scratch_,
                 Compare comp_ = Compare())
      : comp(comp_), scratch(scratch_) {}

  template <typename Group, typename Ptr>
  void operator()([[maybe_unused]] Group g, [[maybe_unused]] Ptr first,
                  [[maybe_unused]] Ptr last) {
#ifdef __SYCL_DEVICE_ONLY__
    // Per extension specification if scratch size is less than the value
    // returned by memory_required then behavior is undefined, so we don't check
    // that the scratch size statisfies the requirement.
    sycl::detail::merge_sort(g, first, last - first, comp, scratch.data());
#else
    throw sycl::exception(
        std::error_code(PI_ERROR_INVALID_DEVICE, sycl::sycl_category()),
        "default_sorter constructor is not supported on host device.");
#endif
  }

  template <typename Group, typename T>
  T operator()([[maybe_unused]] Group g, T val) {
#ifdef __SYCL_DEVICE_ONLY__
    // Per extension specification if scratch size is less than the value
    // returned by memory_required then behavior is undefined, so we don't check
    // that the scratch size statisfies the requirement.
    auto range_size = g.get_local_range().size();
    size_t local_id = g.get_local_linear_id();
    T *temp = reinterpret_cast<T *>(scratch.data());
    ::new (temp + local_id) T(val);
    sycl::detail::merge_sort(g, temp, range_size, comp,
                             scratch.data() + range_size * sizeof(T));
    val = temp[local_id];
#else
    throw sycl::exception(
        std::error_code(PI_ERROR_INVALID_DEVICE, sycl::sycl_category()),
        "default_sorter operator() is not supported on host device.");
#endif
    return val;
  }

  template <typename T>
  static constexpr size_t memory_required(sycl::memory_scope,
                                          size_t range_size) {
    return range_size * sizeof(T) + alignof(T);
  }

  template <typename T, int dim = 1>
  static constexpr size_t memory_required(sycl::memory_scope scope,
                                          sycl::range<dim> r) {
    return 2 * memory_required<T>(scope, r.size());
  }
};

enum class sorting_order { ascending, descending };

namespace detail {

template <typename T, sorting_order = sorting_order::ascending>
struct ConvertToComp {
  using Type = std::less<T>;
};

template <typename T> struct ConvertToComp<T, sorting_order::descending> {
  using Type = std::greater<T>;
};
} // namespace detail

template <typename ValT, sorting_order OrderT = sorting_order::ascending,
          unsigned int BitsPerPass = 4>
class radix_sorter {

  sycl::span<std::byte> scratch;
  uint32_t first_bit = 0;
  uint32_t last_bit = 0;

  static constexpr uint32_t bits = BitsPerPass;
  using bitset_t = std::bitset<sizeof(ValT) * CHAR_BIT>;

public:
  template <size_t Extent>
  radix_sorter(sycl::span<std::byte, Extent> scratch_,
               const bitset_t mask = bitset_t{}.set())
      : scratch(scratch_) {
    static_assert((std::is_arithmetic<ValT>::value ||
                   std::is_same<ValT, sycl::half>::value ||
                   std::is_same<ValT, sycl::ext::oneapi::bfloat16>::value),
                  "radix sort is not usable");

    for (first_bit = 0; first_bit < mask.size() && !mask[first_bit];
         ++first_bit)
      ;
    for (last_bit = first_bit; last_bit < mask.size() && mask[last_bit];
         ++last_bit)
      ;
  }

  template <typename GroupT, typename PtrT>
  void operator()([[maybe_unused]] GroupT g, [[maybe_unused]] PtrT first,
                  [[maybe_unused]] PtrT last) {
#ifdef __SYCL_DEVICE_ONLY__
    sycl::detail::privateDynamicSort</*is_key_value=*/false,
                                     OrderT == sorting_order::ascending,
                                     /*empty*/ 1, BitsPerPass>(
        g, first, /*empty*/ first, last - first, scratch.data(), first_bit,
        last_bit);
#else
    throw sycl::exception(
        std::error_code(PI_ERROR_INVALID_DEVICE, sycl::sycl_category()),
        "radix_sorter is not supported on host device.");
#endif
  }

  template <typename GroupT>
  ValT operator()([[maybe_unused]] GroupT g, [[maybe_unused]] ValT val) {
#ifdef __SYCL_DEVICE_ONLY__
    ValT result[]{val};
    sycl::detail::privateStaticSort</*is_key_value=*/false,
                                    /*is_blocked=*/true,
                                    OrderT == sorting_order::ascending,
                                    /*items_per_work_item=*/1, bits>(
        g, result, /*empty*/ result, scratch.data(), first_bit, last_bit);
    return result[0];
#else
    throw sycl::exception(
        std::error_code(PI_ERROR_INVALID_DEVICE, sycl::sycl_category()),
        "radix_sorter is not supported on host device.");
#endif
  }

  static constexpr size_t memory_required(sycl::memory_scope,
                                          size_t range_size) {
    return range_size * sizeof(ValT) +
           (1 << bits) * range_size * sizeof(uint32_t) + alignof(uint32_t);
  }

  // memory_helpers
  template <int dimensions = 1>
  static constexpr size_t memory_required(sycl::memory_scope,
                                          sycl::range<dimensions> local_range) {
    return (std::max)(local_range.size() * sizeof(ValT),
                      local_range.size() * (1 << bits) * sizeof(uint32_t));
  }
};

} // namespace ext::oneapi::experimental
} // namespace _V1
} // namespace sycl
#endif
