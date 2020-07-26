#pragma once

#include "flexnet/util/macros.hpp"

#include <base/sequence_checker.h>

#include <algorithm>
#include <memory>
#include <type_traits>
#include <utility>

namespace util {

// Use it to make `copy` operation explicit.
// It is good practice to document
// `copy` operation for large data types.
template <
  typename T
  , typename = std::enable_if_t<!std::is_const<T>::value>
>
class CopyWrapper {
 public:
  explicit CopyWrapper(COPIED(T scoper))
      : is_valid_(true)
      , scoper_(std::move(scoper))
  {}

  /// \note |CopyWrapper| must be movable but NOT copiable
  CopyWrapper(CopyWrapper&& other)
      : is_valid_(other.is_valid_)
      , scoper_(std::move(other.scoper_))
  {}

  MUST_USE_RETURN_VALUE
  T Take() const
  {
    // call |Take()| only once and only from one thread
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    CHECK(is_valid_);
    is_valid_ = false;
    return std::move(scoper_);
  }

 private:
  // is_valid_ is distinct from NULL
  mutable bool is_valid_;

  mutable T scoper_;

  SEQUENCE_CHECKER(sequence_checker_);
};

// version of |CopyWrapper| that respects constness
template <
  typename T
>
class ConstCopyWrapper {
 public:
  explicit ConstCopyWrapper(COPIED(const T scoper))
      : is_valid_(true)
      , scoper_(std::move(scoper))
  {}

  /// \note |ConstCopyWrapper| must be movable but NOT copiable
  ConstCopyWrapper(ConstCopyWrapper&& other)
      : is_valid_(other.is_valid_)
      , scoper_(std::move(other.scoper_))
  {}

  MUST_USE_RETURN_VALUE
  const T Take() const
  {
    // call |Take()| only once and only from one thread
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    CHECK(is_valid_);
    is_valid_ = false;
    return std::move(scoper_);
  }

 private:
  // is_valid_ is distinct from NULL
  mutable bool is_valid_;

  mutable T scoper_;

  SEQUENCE_CHECKER(sequence_checker_);
};

} // namespace util
