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
  {}

  template <typename U>
  explicit UnownedPtr(
    UNOWNED_LIFETIME(U* pObj))
    : COPIED(m_pObj(pObj))
  {
    DCHECK(m_pObj);
  }

  // Deliberately implicit to allow returning nullptrs.
  // NOLINTNEXTLINE(runtime/explicit)
  UnownedPtr(std::nullptr_t ptr)
  {
    ignore_result(ptr);
  }

  ~UnownedPtr()
  {
    ProbeForLowSeverityLifetimeIssue();
  }

  UnownedPtr(
    UnownedPtr&& other)
  {
    ProbeForLowSeverityLifetimeIssue();
    if (*this != other) {
      m_pObj = other.Get();
      DCHECK(m_pObj);
    }
  }

  UnownedPtr& operator=(
    UnownedPtr&& other)
  {
    ProbeForLowSeverityLifetimeIssue();
    if (*this != other) {
      m_pObj = other.Get();
      DCHECK(m_pObj);
    }
    return *this;
  }

  UnownedPtr& operator=(
    UNOWNED_LIFETIME(T*) that)
  {
    DCHECK(that);
    ProbeForLowSeverityLifetimeIssue();
    m_pObj = that;
    return *this;
  }

  UnownedPtr& operator=(const UnownedPtr& that)
  {
    ProbeForLowSeverityLifetimeIssue();
    if (*this != that) {
      m_pObj = that.Get();
      DCHECK(m_pObj);
    }
    return *this;
  }

  bool operator==(const UnownedPtr& that) const
  {
    return Get() == that.Get();
  }

  bool operator!=(const UnownedPtr& that) const
  {
    return !(*this == that);
  }

  bool operator<(const UnownedPtr& that) const
  {
    return std::less<T*>()(Get(), that.Get());
  }

  template <typename U>
  bool operator==(const U* that) const
  {
    return Get() == that;
  }

  template <typename U>
  bool operator!=(const U* that) const
  {
    return !(*this == that);
  }

  T* Get() const { return m_pObj; }

  T* Release()
  {
    ProbeForLowSeverityLifetimeIssue();
    T* pTemp = nullptr;
    std::swap(pTemp, m_pObj);
    DCHECK(!m_pObj); // must become prev. |pTemp| i.e. nullptr
    return pTemp;
  }

  explicit operator bool() const
  {
    return !!m_pObj;
  }

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

// UnownedRef

template <class T>
struct is_reference_wrapper : std::false_type {};

template <class U>
struct is_reference_wrapper<std::reference_wrapper<U>> : std::true_type {};

template <class T>
class UnownedRef
{
 public:
  UnownedRef() = default;
  UnownedRef(const UnownedRef& that)
    : UnownedRef(that.Get())
  {}

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

  // Deliberately implicit to allow returning nullptrs.
  // NOLINTNEXTLINE(runtime/explicit)
  UnownedRef(std::nullptr_t ptr)
  {
    ignore_result(ptr);
  }

  ~UnownedRef()
  {
    ProbeForLowSeverityLifetimeIssue();
  }

  UnownedRef(
    UnownedRef&& other)
  {
    ProbeForLowSeverityLifetimeIssue();
    if (*this != other) {
      m_pObj = other.Get();
      DCHECK(m_pObj);
    }
  }

  UnownedRef& operator=(
    UnownedRef&& other)
  {
    ProbeForLowSeverityLifetimeIssue();
    if (*this != other) {
      m_pObj = other.Get();
      DCHECK(m_pObj);
    }
    return *this;
  }

  UnownedRef& operator=(
    UNOWNED_LIFETIME(const T&) that)
  {
    ProbeForLowSeverityLifetimeIssue();
    m_pObj = &that;
    return *this;
  }

  UnownedRef& operator=(
    UNOWNED_LIFETIME(const std::reference_wrapper<T>&) that)
  {
    ProbeForLowSeverityLifetimeIssue();
    m_pObj = &that.get();
    return *this;
  }

  UnownedRef& operator=(const UnownedRef& that)
  {
    ProbeForLowSeverityLifetimeIssue();
    if (*this != that) {
      m_pObj = that.Get();
      DCHECK(m_pObj);
    }
    return *this;
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
  bool operator==(const U* that) const
  {
    return Get() == that;
  }

  template <typename U>
  bool operator!=(const U* that) const
  {
    return !(*this == that);
  }

  T& Ref() const
  {
    DCHECK(m_pObj);
    return *m_pObj;
  }

  T* Get() const { return m_pObj; }

  T* Release()
  {
    ProbeForLowSeverityLifetimeIssue();
    T* pTemp = nullptr;
    std::swap(pTemp, m_pObj);
    DCHECK(!m_pObj); // must become prev. |pTemp| i.e. nullptr
    return pTemp;
  }

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
};

template <typename T, typename U>
inline bool operator==(const U* lhs, const UnownedRef<T>& rhs)
{
  return rhs == lhs;
}

template <typename T, typename U>
inline bool operator!=(const U* lhs, const UnownedRef<T>& rhs)
{
  return rhs != lhs;
}

}  // namespace util
