#pragma once

#include <boost/algorithm/string.hpp>

#include <base/macros.h>
#include <base/sequence_checker.h>
#include <base/callback.h>
#include <base/optional.h>
#include <base/location.h>
#include <base/rvalue_cast.h>
#include <base/bind_helpers.h>
#include <base/strings/string_piece.h>

#include <functional>
#include <map>
#include <string>

namespace basis {

enum class AccessVerifyPolicy {
  // Will call `verifier_.Run()` in any builds (including release),
  // so take care of performance
  Always
  // Will call `verifier_.Run()` only in debug builds,
  // prefer for performance reasons
  , DebugOnly
  // Can be used to implement custom verification logic
  , Skip
};

// Used to verify that each access to underlying type
// is protected by conditional function.
//
// MOTIVATION
//
// Similar to |optional|, but with extra checks on each use.
//
// USAGE
//
//  basis::AccessVerifier<
//    StateMachineType
//  > sm_(
//    BIND_UNRETAINED_RUN_ON_STRAND_CHECK(&acceptorStrand_)// see AccessVerifier constructor
//    , base::in_place // see base::Optional constructor
//    , UNINITIALIZED // see StateMachineType constructor
//    , FillStateTransitionTable()) // see StateMachineType constructor
template<
  typename Type
  , AccessVerifyPolicy VerifyPolicyType
>
class AccessVerifier
{
public:
  using VerifierCb
    = base::RepeatingCallback<bool()>;

public:
  AccessVerifier(VerifierCb&& verifierCb)
    : verifier_(verifierCb)
  {
    DETACH_FROM_SEQUENCE(sequence_checker_);

    DCHECK(!value_);
  }

  /// \note you may want to pass `base::in_place` as second argument
  template <class... Args>
  AccessVerifier(VerifierCb&& verifierCb, Args&&... args)
    : verifier_(verifierCb)
    , value_(std::forward<Args>(args)...)
  {
    DETACH_FROM_SEQUENCE(sequence_checker_);

    DCHECK(value_);
  }

  ~AccessVerifier()
  {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  }

  MUST_USE_RETURN_VALUE
  ALWAYS_INLINE
  bool verify() const
  {
    if constexpr (VerifyPolicyType == AccessVerifyPolicy::Skip)
    {
      NOTREACHED();
    }

    if constexpr (VerifyPolicyType == AccessVerifyPolicy::DebugOnly
      && !DCHECK_IS_ON())
    {
      NOTREACHED();
    }

    DCHECK(verifier_);
    return verifier_.Run();
  }

  /// \note performs automatic checks only in debug mode,
  /// in other modes you must call `verify()` manually
  MUST_USE_RETURN_VALUE
  ALWAYS_INLINE
  base::Optional<Type>& ref_optional(
    const base::Location& from_here)
  {
    if constexpr (VerifyPolicyType == AccessVerifyPolicy::Always
      || (VerifyPolicyType == AccessVerifyPolicy::DebugOnly && DCHECK_IS_ON()))
    {
      DCHECK(verify())
        << from_here.ToString();
      return ref_optional_unsafe(from_here, "");
    } else {
      return ref_optional_unsafe(from_here, "");
    }
  }

  MUST_USE_RETURN_VALUE
  ALWAYS_INLINE
  const base::Optional<Type>& ref_optional(
    const base::Location& from_here) const
  {
    if constexpr (VerifyPolicyType == AccessVerifyPolicy::Always
      || (VerifyPolicyType == AccessVerifyPolicy::DebugOnly && DCHECK_IS_ON()))
    {
      DCHECK(verify())
        << from_here.ToString();
    }

    return ref_optional_unsafe(from_here, "");
  }

  // Similar to |ref_optional|, but without thread-safety checks
  // Usually you want to use *_unsafe in destructors
  // (when data no longer used between threads)
  MUST_USE_RETURN_VALUE
  ALWAYS_INLINE
  base::Optional<Type>& ref_optional_unsafe(
    const base::Location& from_here
    , base::StringPiece reason_why_using_unsafe
    , base::OnceClosure&& check_unsafe_allowed = base::DoNothing::Once())
  {
    ignore_result(from_here);
    ignore_result(reason_why_using_unsafe);
    base::rvalue_cast(check_unsafe_allowed).Run();
    return value_;
  }

  MUST_USE_RETURN_VALUE
  ALWAYS_INLINE
  const base::Optional<Type>& ref_optional_unsafe(
    const base::Location& from_here
    , base::StringPiece reason_why_using_unsafe
    , base::OnceClosure&& check_unsafe_allowed = base::DoNothing::Once()) const
  {
    ignore_result(from_here);
    ignore_result(reason_why_using_unsafe);
    base::rvalue_cast(check_unsafe_allowed).Run();
    return value_;
  }

  /// \note performs automatic checks only in debug mode,
  /// in other modes you must call `verify()` manually
  MUST_USE_RETURN_VALUE
  ALWAYS_INLINE
  Type& ref_value(
    const base::Location& from_here)
  {
    if constexpr (VerifyPolicyType == AccessVerifyPolicy::Always
      || (VerifyPolicyType == AccessVerifyPolicy::DebugOnly && DCHECK_IS_ON()))
    {
      DCHECK(verify() && value_.has_value())
        << from_here.ToString();
    }

    return ref_value_unsafe(from_here, "");
  }

  MUST_USE_RETURN_VALUE
  ALWAYS_INLINE
  const Type& ref_value(
    const base::Location& from_here) const
  {
    if constexpr (VerifyPolicyType == AccessVerifyPolicy::Always
      || (VerifyPolicyType == AccessVerifyPolicy::DebugOnly && DCHECK_IS_ON()))
    {
      DCHECK(verify() && value_.has_value())
        << from_here.ToString();
      return ref_value_unsafe(from_here, "");
    } else {
      return ref_value_unsafe(from_here, "");
    }
  }

  MUST_USE_RETURN_VALUE
  ALWAYS_INLINE
  Type& ref_value_unsafe(
    const base::Location& from_here
    , base::StringPiece reason_why_using_unsafe
    , base::OnceClosure&& check_unsafe_allowed = base::DoNothing::Once())
  {
    ignore_result(from_here);
    ignore_result(reason_why_using_unsafe);
    base::rvalue_cast(check_unsafe_allowed).Run();

    return value_.value();
  }

  // Similar to |ref_value|, but without thread-safety checks
  MUST_USE_RETURN_VALUE
  ALWAYS_INLINE
  const Type& ref_value_unsafe(
    const base::Location& from_here
    , base::StringPiece reason_why_using_unsafe
    , base::OnceClosure&& check_unsafe_allowed = base::DoNothing::Once()) const
  {
    ignore_result(from_here);
    ignore_result(reason_why_using_unsafe);
    base::rvalue_cast(check_unsafe_allowed).Run();
    return value_.value();
  }

  constexpr const Type& operator*() const
  {
    DCHECK(verify() && value_.has_value())
      << FROM_HERE.ToString();
    return value_.operator*();
  }

  constexpr Type& operator*()
  {
    DCHECK(verify() && value_.has_value())
      << FROM_HERE.ToString();
    return value_.operator*();
  }

  constexpr const Type* operator->() const
  {
    DCHECK(verify() && value_.has_value())
      << FROM_HERE.ToString();
    return value_.operator->();
  }

  constexpr Type* operator->()
  {
    DCHECK(verify() && value_.has_value())
      << FROM_HERE.ToString();
    return value_.operator->();
  }

  template<
    class... Args
  >
  MUST_USE_RETURN_VALUE
  ALWAYS_INLINE
  Type& emplace(
    const base::Location& from_here
    , Args&&... args)
  {
    if constexpr (VerifyPolicyType == AccessVerifyPolicy::Always
      || (VerifyPolicyType == AccessVerifyPolicy::DebugOnly && DCHECK_IS_ON()))
    {
      DCHECK(verify())
        << from_here.ToString();
    }

    return value_.emplace(std::forward<Args>(args)...);
  }

  // Similar to |emplace|, but without thread-safety checks
  template<
    class... Args
  >
  MUST_USE_RETURN_VALUE
  ALWAYS_INLINE
  Type& emplace_unsafe(
    const base::Location& from_here
    , base::StringPiece reason_why_using_unsafe
    // usually you want to pass `= base::DoNothing::Once()` here
    , base::OnceClosure&& check_unsafe_allowed
    , Args&&... args)
  {
    ignore_result(from_here);
    ignore_result(reason_why_using_unsafe);
    base::rvalue_cast(check_unsafe_allowed).Run();

    return value_.emplace(std::forward<Args>(args)...);
  }

  bool operator==(const AccessVerifier& that) const
  {
    return ref_value(FROM_HERE) == that.ref_value(FROM_HERE);
  }

  bool operator!=(const AccessVerifier& that) const
  {
    return !(*this == that);
  }

  bool operator<(const AccessVerifier& that) const
  {
    return std::less<Type*>()(ref_value(FROM_HERE), that.ref_value(FROM_HERE));
  }

  template <typename U>
  bool operator==(const U& that) const
  {
    return ref_value(FROM_HERE) == &that;
  }

  template <typename U>
  bool operator!=(const U& that) const
  {
    return !(*this == &that);
  }

private:
  VerifierCb verifier_;

  base::Optional<Type> value_;

  SEQUENCE_CHECKER(sequence_checker_);

  DISALLOW_COPY_AND_ASSIGN(AccessVerifier);
};

template <typename U, class... Args>
inline bool operator==(const U& lhs, const AccessVerifier<Args...>& rhs)
{
  return rhs == lhs;
}

template <typename U, class... Args>
inline bool operator!=(const U& lhs, const AccessVerifier<Args...>& rhs)
{
  return rhs != lhs;
}

} // namespace basis
