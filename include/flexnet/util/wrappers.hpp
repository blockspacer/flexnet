#pragma once

#include "flexnet/util/macros.hpp"

#include <base/sequence_checker.h>

#include <algorithm>
#include <memory>
#include <type_traits>
#include <utility>

namespace util {

/* Required wrapper for if constexpr
 *
 * Is dependent on a template parameter.
 * Is used in static_assert in a false branch to produce a compile error
 * with message containing provided type.
 * See an example with dependent_false at https://en.cppreference.com/w/cpp/language/if
 *
 * if constexpr (std::is_same<T, someType1>) {
 * } else if constexpr (std::is_same<T, someType2>) {
 * } else {
 *     static_assert(dependent_false<T>::value, "unknown type");
 * }
 */
template<class U>
struct dependent_false : std::false_type {};

// similar to dependent_false
// used to print type names in static_assert
template<typename... typeclass>
struct typename_false : std::false_type {};

/// \todo add ASAN support like in |UnownedPtr|
// template<typename T>
// using UnownedRef
//   = std::reference_wrapper<T>;

// Use it to make sure that you `copy-only-once` or `move`.
// It is good practice to document
// `copy-only-once` operation via |MoveOnly| for large data types.
/// \note |MoveOnly| is movable but NOT copiable
/// to make sure that you copy large data type ONLY ONCE!
template <
  class T
  , typename = void
>
class MoveOnly
{
  static_assert(
    typename_false<T>::value
    , "unable to find MoveOnly implementation");
};

template <
  class T
>
class MoveOnly<
  T
  , std::enable_if_t<
      !std::is_const<T>{}
        // you may want to use |UnownedPtr|
        // if you want to wrap pointer
        && !std::is_pointer<T>{}
      , void
    >
>
{
 private:
  // Made private bacause it makes
  // `move` operation implicit.
  // Use |moveFrom| instead.
  explicit MoveOnly(T&& scoper)
      : is_valid_(true)
      , scoper_(std::move(scoper))
  {}

 public:
  // We want to explicitly document that `copy` operation will happen
  static MoveOnly copyFrom(COPIED(const T& scoper))
  {
    T tmp = scoper;
    return MoveOnly(std::move(tmp));
  }

  // We want to explicitly document that `move` operation will happen
  static MoveOnly moveFrom(T&& scoper)
  {
    return MoveOnly(std::move(scoper));
  }

  /// \note |MoveOnly| must be movable but NOT copiable
  /// to make sure that you copy large data type ONLY ONCE!
  MoveOnly(MoveOnly&& other)
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

  DISALLOW_NEW_OPERATOR(MoveOnly);
};

// version of |MoveOnly| for `const T` data types
template <
  class T
>
class MoveOnly<
  const T
  , std::enable_if_t<
      // you may want to use |UnownedPtr|
      // if you want to wrap pointer
      !std::is_pointer<T>{}
      , void
    >
>
{
 private:
  // Made private bacause it makes
  // `move` operation implicit.
  // Use |moveFrom| instead.
  explicit MoveOnly(T&& scoper)
      : is_valid_(true)
      , scoper_(std::move(scoper))
  {}

 public:
  // We want to explicitly document that `copy` operation will happen
  static MoveOnly copyFrom(COPIED(const T& scoper))
  {
    T tmp = scoper;
    return MoveOnly(std::move(tmp));
  }

  // We want to explicitly document that `move` operation will happen
  static MoveOnly moveFrom(T&& scoper)
  {
    return MoveOnly(std::move(scoper));
  }

  /// \note |MoveOnly| must be movable but NOT copiable
  /// to make sure that you copy large data type ONLY ONCE!
  MoveOnly(MoveOnly&& other)
      : is_valid_(other.is_valid_)
      , scoper_(std::move(other.scoper_))
  {}

  MUST_USE_RETURN_VALUE
  const T TakeConst() const
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

  DISALLOW_NEW_OPERATOR(MoveOnly);
};

} // namespace util
