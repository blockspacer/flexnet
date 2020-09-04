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
#include <base/threading/thread_collision_warner.h>

#include <basis/bitmask.h>

#include <functional>
#include <map>
#include <string>

namespace util {

enum class AccessVerifyPermissions
{
  None
      = 0
  // If `VerifyNothing::hasReadPermission` is true,
  // when you can read stored in `VerifyNothing` value
  // depending on `AccessVerifyPolicy`.
  // For example, if `VerifyNothing::hasReadPermission` is false
  // and `AccessVerifyPolicy` is not `Skip`, then
  // checks may be perfomed during call to
  // `VerifyNothing::operator->()`
  , Readable
      = 1 << 1
  // If `VerifyNothing::hasModifyPermission` is true,
  // when you can change stored in `VerifyNothing` value
  // depending on `AccessVerifyPolicy`.
  // For example, if `VerifyNothing::hasModifyPermission` is false
  // and `AccessVerifyPolicy` is not `Skip`, then
  // checks may be perfomed during call to
  // `VerifyNothing::emplace`
  , Modifiable
      = 1 << 2
  , All
      = AccessVerifyPermissions::Readable
        | AccessVerifyPermissions::Modifiable
};
ALLOW_BITMASK_OPERATORS(AccessVerifyPermissions)

} // namespace util

namespace basis {

// Creates a callback that does nothing when called.
class BASE_EXPORT VerifyNothing {
 public:
  template <typename... Args>
  operator base::RepeatingCallback<bool(Args...)>() const {
    return Repeatedly<Args...>();
  }
  template <typename... Args>
  operator base::OnceCallback<bool(Args...)>() const {
    return Once<Args...>();
  }
  // Explicit way of specifying a specific callback type when the compiler can't
  // deduce it.
  template <typename... Args>
  static base::RepeatingCallback<bool(Args...)> Repeatedly() {
    return base::BindRepeating([](Args... /*args*/){ return true; });
  }
  template <typename... Args>
  static base::OnceCallback<bool(Args...)> Once() {
    return base::BindOnce([](Args... /*args*/) { return true; });
  }
};

enum class AccessVerifyPolicy {
  // Will call `access_verifier_.Run()` in any builds (including release),
  // so take care of performance
  Always
  // Will call `access_verifier_.Run()` only in debug builds,
  // prefer for performance reasons
  , DebugOnly
  // Can be used to implement custom verification logic
  , Skip
};

// Used to verify that each access to underlying type
// is protected by conditional function.
//
/// \note All checks performed on storage of `Type` object
/// (i.e. controls modification of `base::Optional`),
/// not on API of `Type` object.
/// You must also control thread-safety and permissions
/// in API of `Type` object.
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
//    BIND_UNRETAINED_RUN_ON_STRAND_CHECK(&acceptorStrand_) // see AccessVerifier constructor
//    // "disallow `emplace` for thread-safety reasons"
//    , util::AccessVerifyPermissions::Readable // see AccessVerifier constructor
//    , base::in_place // see base::Optional constructor
//    , UNINITIALIZED // see StateMachineType constructor
//    , FillStateTransitionTable()) // see StateMachineType constructor
//
//  sm_.forceValidToModify(FROM_HERE
//    , "allow `emplace`"
//  );
//
//  sm_.forceNotValidToModify(FROM_HERE
//    , "disallow `emplace` for thread-safety reasons"
//  );
template<
  typename Type
  , AccessVerifyPolicy VerifyPolicyType
>
class AccessVerifier
{
public:
  // May be called on each member function call
  // depending on `AccessVerifyPolicy`.
  // Usually it is used for thread-safety checks.
  using VerifierCb
    = base::RepeatingCallback<bool()>;

public:
  /// \note if you want to initialize `value_`,
  /// than you can pass `base::in_place` as second argument
  explicit AccessVerifier(
    VerifierCb&& verifierCb
    , const util::AccessVerifyPermissions& permissions
        = util::AccessVerifyPermissions::All)
    : access_verifier_(verifierCb)
    , accessVerifyPermissions(permissions)
  {
    DETACH_FROM_SEQUENCE(sequence_checker_);

    DCHECK(!value_);
  }

  /// \note you may want to pass `base::in_place` as second argument
  template <class... Args>
  AccessVerifier(
    VerifierCb&& verifierCb
    , const util::AccessVerifyPermissions& permissions
    , Args&&... args)
    : access_verifier_(verifierCb)
    , accessVerifyPermissions(permissions)
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
  bool runVerifierCallback() const NO_EXCEPTION
  {
    DFAKE_SCOPED_RECURSIVE_LOCK(debug_collision_warner_);

    if constexpr (VerifyPolicyType == AccessVerifyPolicy::Skip)
    {
      NOTREACHED();
    }

    if constexpr (VerifyPolicyType == AccessVerifyPolicy::DebugOnly
      && !DCHECK_IS_ON())
    {
      NOTREACHED();
    }

    DCHECK(access_verifier_);
    return access_verifier_.Run();
  }

  MUST_USE_RETURN_VALUE
  ALWAYS_INLINE
  bool hasReadPermission() const NO_EXCEPTION
  {
    DFAKE_SCOPED_RECURSIVE_LOCK(debug_collision_warner_);

    if constexpr (VerifyPolicyType == AccessVerifyPolicy::Skip)
    {
      NOTREACHED();
    }

    if constexpr (VerifyPolicyType == AccessVerifyPolicy::DebugOnly
      && !DCHECK_IS_ON())
    {
      NOTREACHED();
    }

    return util::hasBit(accessVerifyPermissions
          , util::AccessVerifyPermissions::Readable);
  }

  MUST_USE_RETURN_VALUE
  ALWAYS_INLINE
  bool hasModifyPermission() const NO_EXCEPTION
  {
    DFAKE_SCOPED_RECURSIVE_LOCK(debug_collision_warner_);

    if constexpr (VerifyPolicyType == AccessVerifyPolicy::Skip)
    {
      NOTREACHED();
    }

    if constexpr (VerifyPolicyType == AccessVerifyPolicy::DebugOnly
      && !DCHECK_IS_ON())
    {
      NOTREACHED();
    }

    return
      util::hasBit(accessVerifyPermissions
          , util::AccessVerifyPermissions::Modifiable);
  }

  /// \note performs automatic checks only in debug mode,
  /// in other modes you must call `runVerifierCallback()` manually
  MUST_USE_RETURN_VALUE
  ALWAYS_INLINE
  base::Optional<Type>& ref_optional(
    const base::Location& from_here)
  {
    DFAKE_SCOPED_RECURSIVE_LOCK(debug_collision_warner_);

    if constexpr (VerifyPolicyType == AccessVerifyPolicy::Always)
    {
      CHECK(runVerifierCallback())
        << from_here.ToString();
      CHECK(hasReadPermission())
        << from_here.ToString();
    }
    else if constexpr (VerifyPolicyType == AccessVerifyPolicy::DebugOnly && DCHECK_IS_ON())
    {
      DCHECK(runVerifierCallback())
        << from_here.ToString();
      DCHECK(hasReadPermission())
        << from_here.ToString();
    }

    return ref_optional_unsafe(from_here, "");
  }

  MUST_USE_RETURN_VALUE
  ALWAYS_INLINE
  const base::Optional<Type>& ref_optional(
    const base::Location& from_here) const
  {
    DFAKE_SCOPED_RECURSIVE_LOCK(debug_collision_warner_);

    if constexpr (VerifyPolicyType == AccessVerifyPolicy::Always)
    {
      CHECK(runVerifierCallback())
        << from_here.ToString();
      CHECK(hasReadPermission())
        << from_here.ToString();
    }
    else if constexpr (VerifyPolicyType == AccessVerifyPolicy::DebugOnly && DCHECK_IS_ON())
    {
      DCHECK(runVerifierCallback())
        << from_here.ToString();
      DCHECK(hasReadPermission())
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
    DFAKE_SCOPED_RECURSIVE_LOCK(debug_collision_warner_);

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
    DFAKE_SCOPED_RECURSIVE_LOCK(debug_collision_warner_);

    ignore_result(from_here);
    ignore_result(reason_why_using_unsafe);
    base::rvalue_cast(check_unsafe_allowed).Run();
    return value_;
  }

  /// \note performs automatic checks only in debug mode,
  /// in other modes you must call `runVerifierCallback()` manually
  MUST_USE_RETURN_VALUE
  ALWAYS_INLINE
  Type& ref_value(
    const base::Location& from_here)
  {
    DFAKE_SCOPED_RECURSIVE_LOCK(debug_collision_warner_);

    if constexpr (VerifyPolicyType == AccessVerifyPolicy::Always)
    {
      CHECK(runVerifierCallback())
        << from_here.ToString();
      CHECK(hasReadPermission())
        << from_here.ToString();
      CHECK(value_.has_value())
        << from_here.ToString();
    }
    else if constexpr (VerifyPolicyType == AccessVerifyPolicy::DebugOnly && DCHECK_IS_ON())
    {
      DCHECK(runVerifierCallback())
        << from_here.ToString();
      DCHECK(hasReadPermission())
        << from_here.ToString();
      DCHECK(value_.has_value())
        << from_here.ToString();
    }

    return ref_value_unsafe(from_here, "");
  }

  MUST_USE_RETURN_VALUE
  ALWAYS_INLINE
  const Type& ref_value(
    const base::Location& from_here) const
  {
    DFAKE_SCOPED_RECURSIVE_LOCK(debug_collision_warner_);

    if constexpr (VerifyPolicyType == AccessVerifyPolicy::Always)
    {
      CHECK(runVerifierCallback())
        << from_here.ToString();
      CHECK(hasReadPermission())
        << from_here.ToString();
      CHECK(value_.has_value())
        << from_here.ToString();
    }
    else if constexpr (VerifyPolicyType == AccessVerifyPolicy::DebugOnly && DCHECK_IS_ON())
    {
      DCHECK(runVerifierCallback())
        << from_here.ToString();
      DCHECK(hasReadPermission())
        << from_here.ToString();
      DCHECK(value_.has_value())
        << from_here.ToString();
    }

    return ref_value_unsafe(from_here, "");
  }

  MUST_USE_RETURN_VALUE
  ALWAYS_INLINE
  Type& ref_value_unsafe(
    const base::Location& from_here
    , base::StringPiece reason_why_using_unsafe
    , base::OnceClosure&& check_unsafe_allowed = base::DoNothing::Once())
  {
    DFAKE_SCOPED_RECURSIVE_LOCK(debug_collision_warner_);

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
    DFAKE_SCOPED_RECURSIVE_LOCK(debug_collision_warner_);

    ignore_result(from_here);
    ignore_result(reason_why_using_unsafe);
    base::rvalue_cast(check_unsafe_allowed).Run();
    return value_.value();
  }

  constexpr const Type& operator*() const
  {
    if constexpr (VerifyPolicyType == AccessVerifyPolicy::Always)
    {
      CHECK(runVerifierCallback())
        << FROM_HERE.ToString();

      CHECK(hasReadPermission())
        << FROM_HERE.ToString();

      CHECK(value_.has_value())
        << FROM_HERE.ToString();
    }
    else if constexpr (VerifyPolicyType == AccessVerifyPolicy::DebugOnly && DCHECK_IS_ON())
    {
      DCHECK(runVerifierCallback())
        << FROM_HERE.ToString();

      DCHECK(hasReadPermission())
        << FROM_HERE.ToString();

      DCHECK(value_.has_value())
        << FROM_HERE.ToString();
    }

    return value_.operator*();
  }

  constexpr Type& operator*()
  {
    if constexpr (VerifyPolicyType == AccessVerifyPolicy::Always)
    {
      CHECK(runVerifierCallback())
        << FROM_HERE.ToString();

      CHECK(hasReadPermission())
        << FROM_HERE.ToString();

      CHECK(value_.has_value())
        << FROM_HERE.ToString();
    }
    else if constexpr (VerifyPolicyType == AccessVerifyPolicy::DebugOnly && DCHECK_IS_ON())
    {
      DCHECK(runVerifierCallback())
        << FROM_HERE.ToString();

      DCHECK(hasReadPermission())
        << FROM_HERE.ToString();

      DCHECK(value_.has_value())
        << FROM_HERE.ToString();
    }

    return value_.operator*();
  }

  constexpr const Type* operator->() const
  {
    if constexpr (VerifyPolicyType == AccessVerifyPolicy::Always)
    {
      CHECK(runVerifierCallback())
        << FROM_HERE.ToString();

      CHECK(hasReadPermission())
        << FROM_HERE.ToString();

      CHECK(value_.has_value())
        << FROM_HERE.ToString();
    }
    else if constexpr (VerifyPolicyType == AccessVerifyPolicy::DebugOnly && DCHECK_IS_ON())
    {
      DCHECK(runVerifierCallback())
        << FROM_HERE.ToString();

      DCHECK(hasReadPermission())
        << FROM_HERE.ToString();

      DCHECK(value_.has_value())
        << FROM_HERE.ToString();
    }

    return value_.operator->();
  }

  constexpr Type* operator->()
  {
    if constexpr (VerifyPolicyType == AccessVerifyPolicy::Always)
    {
      CHECK(runVerifierCallback())
        << FROM_HERE.ToString();

      CHECK(hasReadPermission())
        << FROM_HERE.ToString();

      CHECK(value_.has_value())
        << FROM_HERE.ToString();
    }
    else if constexpr (VerifyPolicyType == AccessVerifyPolicy::DebugOnly && DCHECK_IS_ON())
    {
      DCHECK(runVerifierCallback())
        << FROM_HERE.ToString();

      DCHECK(hasReadPermission())
        << FROM_HERE.ToString();

      DCHECK(value_.has_value())
        << FROM_HERE.ToString();
    }

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
    DFAKE_SCOPED_RECURSIVE_LOCK(debug_collision_warner_);

    if constexpr (VerifyPolicyType == AccessVerifyPolicy::Always)
    {
      CHECK(runVerifierCallback())
        << from_here.ToString();

      CHECK(hasModifyPermission())
        << from_here.ToString();
    }
    else if constexpr (VerifyPolicyType == AccessVerifyPolicy::DebugOnly && DCHECK_IS_ON())
    {
      DCHECK(runVerifierCallback())
        << from_here.ToString();

      DCHECK(hasModifyPermission())
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
    DFAKE_SCOPED_RECURSIVE_LOCK(debug_collision_warner_);

    ignore_result(from_here);
    ignore_result(reason_why_using_unsafe);
    base::rvalue_cast(check_unsafe_allowed).Run();

    return value_.emplace(std::forward<Args>(args)...);
  }

  void reset(const base::Location& from_here)
  {
    DFAKE_SCOPED_RECURSIVE_LOCK(debug_collision_warner_);

    if constexpr (VerifyPolicyType == AccessVerifyPolicy::Always)
    {
      CHECK(runVerifierCallback())
        << FROM_HERE.ToString();

      CHECK(hasModifyPermission())
        << FROM_HERE.ToString();
    }
    else if constexpr (VerifyPolicyType == AccessVerifyPolicy::DebugOnly && DCHECK_IS_ON())
    {
      DCHECK(runVerifierCallback())
        << FROM_HERE.ToString();

      DCHECK(hasModifyPermission())
        << FROM_HERE.ToString();
    }

    value_.reset();
  }

  void reset_unsafe(
    const base::Location& from_here
    , base::StringPiece reason_why_using_unsafe
    // usually you want to pass `= base::DoNothing::Once()` here
    , base::OnceClosure&& check_unsafe_allowed)
  {
    DFAKE_SCOPED_RECURSIVE_LOCK(debug_collision_warner_);

    ignore_result(from_here);
    ignore_result(reason_why_using_unsafe);
    base::rvalue_cast(check_unsafe_allowed).Run();

    value_.reset();
  }

  void forceNotValidToRead(
    const base::Location& from_here
    , base::StringPiece reason_why_make_invalid)
  {
    DFAKE_SCOPED_RECURSIVE_LOCK(debug_collision_warner_);

    DCHECK(hasReadPermission());

    ignore_result(from_here);
    ignore_result(reason_why_make_invalid);

    util::removeBit(accessVerifyPermissions
      , util::AccessVerifyPermissions::Readable);
  }

  void forceNotValidToModify(
    const base::Location& from_here
    , base::StringPiece reason_why_make_invalid)
  {
    DFAKE_SCOPED_RECURSIVE_LOCK(debug_collision_warner_);

    DCHECK(hasModifyPermission());

    ignore_result(from_here);
    ignore_result(reason_why_make_invalid);

    util::removeBit(accessVerifyPermissions
      , util::AccessVerifyPermissions::Modifiable);
  }

  void forceValidToRead(
    const base::Location& from_here
    , base::StringPiece reason_why_make_valid)
  {
    DFAKE_SCOPED_RECURSIVE_LOCK(debug_collision_warner_);

    DCHECK(!hasReadPermission());

    ignore_result(from_here);
    ignore_result(reason_why_make_valid);

    util::addBit(accessVerifyPermissions
      , util::AccessVerifyPermissions::Readable);
  }

  void forceValidToModify(
    const base::Location& from_here
    , base::StringPiece reason_why_make_valid)
  {
    DFAKE_SCOPED_RECURSIVE_LOCK(debug_collision_warner_);

    DCHECK(!hasModifyPermission());

    ignore_result(from_here);
    ignore_result(reason_why_make_valid);

    util::addBit(accessVerifyPermissions
      , util::AccessVerifyPermissions::Modifiable);
  }

  bool operator==(const AccessVerifier& that) const
  {
    DFAKE_SCOPED_RECURSIVE_LOCK(debug_collision_warner_);

    return ref_value(FROM_HERE) == that.ref_value(FROM_HERE);
  }

  bool operator!=(const AccessVerifier& that) const
  {
    DFAKE_SCOPED_RECURSIVE_LOCK(debug_collision_warner_);

    return !(*this == that);
  }

  bool operator<(const AccessVerifier& that) const
  {
    DFAKE_SCOPED_RECURSIVE_LOCK(debug_collision_warner_);

    return std::less<Type*>()(ref_value(FROM_HERE), that.ref_value(FROM_HERE));
  }

  template <typename U>
  bool operator==(const U& that) const
  {
    DFAKE_SCOPED_RECURSIVE_LOCK(debug_collision_warner_);

    return ref_value(FROM_HERE) == &that;
  }

  template <typename U>
  bool operator!=(const U& that) const
  {
    DFAKE_SCOPED_RECURSIVE_LOCK(debug_collision_warner_);

    return !(*this == &that);
  }

private:
  VerifierCb access_verifier_
    LIVES_ON(debug_collision_warner_);

  base::Optional<Type> value_
    LIVES_ON(debug_collision_warner_);

  // MOTIVATION
  //
  // We already have custom validation function `access_verifier_`,
  // but it is too common to make object not valid using
  // `std::move` (just mark invalid after move)
  // or to force object to initialize only once
  // (just mark invalid after initialization to prohibit `emplace()`)
  util::AccessVerifyPermissions accessVerifyPermissions{
    util::AccessVerifyPermissions::All}
    LIVES_ON(debug_collision_warner_);

  // Thread collision warner to ensure that API is not called concurrently.
  // API allowed to call from multiple threads, but not
  // concurrently.
  DFAKE_MUTEX(debug_collision_warner_);

  // check sequence on which class was constructed/destructed/configured
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
