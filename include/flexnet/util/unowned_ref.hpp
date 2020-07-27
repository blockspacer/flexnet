// Copyright 2017 PDFium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include "flexnet/util/macros.hpp"

#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

#include <base/logging.h>

namespace util {

// UnownedRef is similar to std::reference_wrapper
//
// 1. It documents the nature of the reference with no need to add a comment
//    explaining that is it // Not owned.
//
// 2. Supports memory tools like ASAN
//
// 3. Can construct from std::ref and std::cref
//    (like std::reference_wrapper)
//
// 4. Assignment do NOT rebind the internal pointer
//    (NOT like std::reference_wrapper)
//
// 5. Because |UnownedRef| expected to be NOT modified after construction,
//    it is more thread-safe than |UnownedPtr|

/// \todo move to separate file
template <class T>
struct is_reference_wrapper
  : std::false_type {};

/// \todo move to separate file
template <class U>
struct is_reference_wrapper<std::reference_wrapper<U>>
  : std::true_type {};

template <class T>
class UnownedRef
{
 public:
  UnownedRef() = default;
  UnownedRef(const UnownedRef& that)
    : UnownedRef(that.Ref())
  {}

  CAUTION_NOT_THREAD_SAFE(UnownedRef(
    UnownedRef&& other))
  {
    // can be changed only if not initialized
    DCHECK(!m_pObj);

    ProbeForLowSeverityLifetimeIssue();
    if (*this != other) {
      m_pObj = other.Get();
      DCHECK(m_pObj);
    }
  }

  template <
    typename U
    , std::enable_if_t<
        !is_reference_wrapper<std::decay_t<U>>::value
        , void
      >
  >
  explicit UnownedRef(
    UNOWNED_LIFETIME(const U& pObj))
    : COPIED(m_pObj(&pObj))
  {
    DCHECK(m_pObj);
  }

  template <
    typename U
  >
  UnownedRef(
    UNOWNED_LIFETIME(const std::reference_wrapper<U>& pObj))
    : COPIED(m_pObj(&pObj.get()))
  {
    DCHECK(m_pObj);
  }

  ~UnownedRef()
  {
    ProbeForLowSeverityLifetimeIssue();
  }

  bool operator==(const UnownedRef& that) const
  {
    return Get() == that.Get();
  }

  bool operator!=(const UnownedRef& that) const
  {
    return !(*this == that);
  }

  bool operator<(const UnownedRef& that) const
  {
    return std::less<T*>()(Get(), that.Get());
  }

  template <typename U>
  bool operator==(const U& that) const
  {
    return Get() == &that;
  }

  template <typename U>
  bool operator!=(const U& that) const
  {
    return !(*this == &that);
  }

  T& Ref() const
  {
    DCHECK(m_pObj);
    return *m_pObj;
  }

  T* Get() const { return m_pObj; }

  T& operator*() const
  {
    DCHECK(m_pObj);
    return *m_pObj;
  }

  T* operator->() const
  {
    DCHECK(m_pObj);
    return m_pObj;
  }

 private:
  // check that object is alive, use memory tool like ASAN
  inline void ProbeForLowSeverityLifetimeIssue()
  {
#if defined(MEMORY_TOOL_REPLACES_ALLOCATOR)
    if (m_pObj)
      reinterpret_cast<const volatile uint8_t*>(m_pObj)[0];
#endif
  }

  T* m_pObj = nullptr;

  DISALLOW_ASSIGN(UnownedRef);
};

template <typename T, typename U>
inline bool operator==(const U& lhs, const UnownedRef<T>& rhs)
{
  return rhs == lhs;
}

template <typename T, typename U>
inline bool operator!=(const U& lhs, const UnownedRef<T>& rhs)
{
  return rhs != lhs;
}

}  // namespace util
