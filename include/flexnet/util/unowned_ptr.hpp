// Copyright 2017 PDFium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include "flexnet/util/macros.hpp"

#include <base/macros.h>
#include <base/logging.h>
#include <base/threading/thread_collision_warner.h>

#include <cstddef>
#include <functional>

// UnownedPtr is a smart pointer class that behaves very much like a
// standard C-style pointer. The advantages of using it over raw
// pointers are:
//
// 1. It documents the nature of the pointer with no need to add a comment
//    explaining that is it // Not owned. Additionally, an attempt to delete
//    an unowned ptr will fail to compile rather than silently succeeding,
//    since it is a class and not a raw pointer.
//
// 2. When built for a memory tool like ASAN, the class provides a destructor
//    which checks that the object being pointed to is still alive.
//
// Hence, when using UnownedPtr, no dangling pointers are ever permitted,
// even if they are not de-referenced after becoming dangling. The style of
// programming required is that the lifetime an object containing an
// UnownedPtr must be strictly less than the object to which it points.
//
// The same checks are also performed at assignment time to prove that the
// old value was not a dangling pointer, either.
//
// The array indexing operation [] is not supported on an unowned ptr,
// because an unowned ptr expresses a one to one relationship with some
// other heap object.

namespace util {

template <class T>
class UnownedPtr
{
 public:
  UnownedPtr() = default;
  UnownedPtr(const UnownedPtr& that)
    : UnownedPtr(that.Get())
  {
    DFAKE_SCOPED_LOCK(debug_collision_warner_);
  }

  template <typename U>
  explicit UnownedPtr(
    UNOWNED_LIFETIME(U* pObj))
    : COPIED(m_pObj(pObj))
  {
    DFAKE_SCOPED_LOCK(debug_collision_warner_);

    DCHECK(m_pObj);
  }

  // Deliberately implicit to allow returning nullptrs.
  // NOLINTNEXTLINE(runtime/explicit)
  UnownedPtr(std::nullptr_t ptr)
  {
    DFAKE_SCOPED_LOCK(debug_collision_warner_);

    ignore_result(ptr);
  }

  ~UnownedPtr()
  {
    DFAKE_SCOPED_LOCK(debug_collision_warner_);

    ProbeForLowSeverityLifetimeIssue();
  }

  CAUTION_NOT_THREAD_SAFE()
  UnownedPtr(
    UnownedPtr&& other)
  {
    DFAKE_SCOPED_LOCK(debug_collision_warner_);

    ProbeForLowSeverityLifetimeIssue();
    if (*this != other) {
      m_pObj = other.Get();
      DCHECK(m_pObj);
    }
  }

  CAUTION_NOT_THREAD_SAFE()
  UnownedPtr& operator=(
    UnownedPtr&& other)
  {
    DFAKE_SCOPED_LOCK(debug_collision_warner_);

    ProbeForLowSeverityLifetimeIssue();
    if (*this != other) {
      m_pObj = other.Get();
      DCHECK(m_pObj);
    }
    return *this;
  }

  CAUTION_NOT_THREAD_SAFE()
  UnownedPtr& operator=(
    UNOWNED_LIFETIME(T*) that)
  {
    DFAKE_SCOPED_LOCK(debug_collision_warner_);

    DCHECK(that);
    ProbeForLowSeverityLifetimeIssue();
    m_pObj = that;
    return *this;
  }

  CAUTION_NOT_THREAD_SAFE()
  UnownedPtr& operator=(
    const UnownedPtr& that)
  {
    DFAKE_SCOPED_LOCK(debug_collision_warner_);

    ProbeForLowSeverityLifetimeIssue();
    if (*this != that) {
      m_pObj = that.Get();
      DCHECK(m_pObj);
    }
    return *this;
  }

  CAUTION_NOT_THREAD_SAFE()
  bool operator==(
    const UnownedPtr& that) const
  {
    DFAKE_SCOPED_RECURSIVE_LOCK(debug_collision_warner_);

    return Get() == that.Get();
  }

  CAUTION_NOT_THREAD_SAFE()
  bool operator!=(
    const UnownedPtr& that) const
  {
    DFAKE_SCOPED_RECURSIVE_LOCK(debug_collision_warner_);

    return !(*this == that);
  }

  CAUTION_NOT_THREAD_SAFE()
  bool operator<(
    const UnownedPtr& that) const
  {
    DFAKE_SCOPED_RECURSIVE_LOCK(debug_collision_warner_);

    return std::less<T*>()(Get(), that.Get());
  }

  CAUTION_NOT_THREAD_SAFE()
  template <typename U>
  bool operator==(
    const U* that) const
  {
    DFAKE_SCOPED_RECURSIVE_LOCK(debug_collision_warner_);

    return Get() == that;
  }

  CAUTION_NOT_THREAD_SAFE()
  template <typename U>
  bool operator!=(
    const U* that) const
  {
    DFAKE_SCOPED_RECURSIVE_LOCK(debug_collision_warner_);

    return !(*this == that);
  }

  /// \note Do not do stupid things like
  /// `delete unownedPtr.Get();`
  CAUTION_NOT_THREAD_SAFE()
  T* Get() const
  {
    DFAKE_SCOPED_RECURSIVE_LOCK(debug_collision_warner_);

    return m_pObj;
  }

  CAUTION_NOT_THREAD_SAFE()
  T* Release()
  {
    DFAKE_SCOPED_RECURSIVE_LOCK(debug_collision_warner_);

    ProbeForLowSeverityLifetimeIssue();
    T* pTemp = nullptr;
    std::swap(pTemp, m_pObj);
    DCHECK(!m_pObj); // must become prev. |pTemp| i.e. nullptr
    return pTemp;
  }

  CAUTION_NOT_THREAD_SAFE()
  explicit operator bool() const
  {
    DFAKE_SCOPED_RECURSIVE_LOCK(debug_collision_warner_);

    return !!m_pObj;
  }

  CAUTION_NOT_THREAD_SAFE()
  T& operator*() const
  {
    DFAKE_SCOPED_RECURSIVE_LOCK(debug_collision_warner_);

    DCHECK(m_pObj);
    return *m_pObj;
  }

  CAUTION_NOT_THREAD_SAFE()
  T* operator->() const
  {
    DFAKE_SCOPED_RECURSIVE_LOCK(debug_collision_warner_);

    DCHECK(m_pObj);
    return m_pObj;
  }

 private:
  // check that object is alive, use memory tool like ASAN
  inline void ProbeForLowSeverityLifetimeIssue()
  {
    DFAKE_SCOPED_RECURSIVE_LOCK(debug_collision_warner_);

#if defined(MEMORY_TOOL_REPLACES_ALLOCATOR)
    if (m_pObj)
      reinterpret_cast<const volatile uint8_t*>(m_pObj)[0];
#endif
  }

  // Thread collision warner to ensure that API is not called concurrently.
  // |m_pObj| allowed to call from multiple threads, but not
  // concurrently.
  DFAKE_MUTEX(debug_collision_warner_);

  CAUTION_NOT_THREAD_SAFE()
  T* m_pObj = nullptr
    LIVES_ON(debug_collision_warner_);
};

template <typename T, typename U>
inline bool operator==(const U* lhs, const UnownedPtr<T>& rhs)
{
  return rhs == lhs;
}

template <typename T, typename U>
inline bool operator!=(const U* lhs, const UnownedPtr<T>& rhs)
{
  return rhs != lhs;
}

}  // namespace util
