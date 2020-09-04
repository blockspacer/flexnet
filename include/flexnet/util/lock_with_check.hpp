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

namespace basis {

// Will call `callback_.Run()` in any builds (including release),
// so take care of performance
struct FakeLockPolicyAlways
{
  /// \todo refactor to `enum class { isDebug, isSkip, isAlways }`
  static constexpr bool isDebugOnly = false;
  static constexpr bool isSkip = false;
  static constexpr bool isAlways = true;
};

// Will call `callback_.Run()` only in debug builds,
// prefer for performance reasons
struct FakeLockPolicyDebugOnly
{
  /// \todo refactor to `enum class { isDebug, isSkip, isAlways }`
  static constexpr bool isDebugOnly = true;
  static constexpr bool isSkip = false;
  static constexpr bool isAlways = false;
};

// Can be used to implement custom verification logic
struct FakeLockPolicySkip
{
  /// \todo refactor to `enum class { isDebug, isSkip, isAlways }`
  static constexpr bool isDebugOnly = false;
  static constexpr bool isSkip = true;
  static constexpr bool isAlways = false;
};

template <
  typename Signature
>
class FakeLockWithCheck;

template <
  typename FakeLockPolicyType
  , typename Signature
>
class AutoFakeLockWithCheck;

/// \note It is not real lock, only annotated as lock.
/// \note Need to build with `-Wthread-safety-analysis`
/// flag to see some effect.
/// see https://pspdfkit.com/blog/2020/the-cpp-lifetime-profile/
/// see http://clang.llvm.org/docs/ThreadSafetyAnalysis.html
/// see https://github.com/isocpp/CppCoreGuidelines/blob/master/docs/Lifetime.pdf
template<
  typename R
  , typename... Args
>
class LOCKABLE
  FakeLockWithCheck<R(Args...)>
{
 public:
  using RunType = R(Args...);

  FakeLockWithCheck(
    const base::RepeatingCallback<RunType>& callback)
    : callback_(callback) {}

  MUST_USE_RETURN_VALUE
  bool Acquire() const NO_EXCEPTION EXCLUSIVE_LOCK_FUNCTION()
  {
    return callback_.Run();
  }

  MUST_USE_RETURN_VALUE
  bool Release() const NO_EXCEPTION UNLOCK_FUNCTION()
  {
    return callback_.Run();
  }

 private:
  base::RepeatingCallback<RunType> callback_;

  DISALLOW_COPY_AND_ASSIGN(FakeLockWithCheck);
};

// Will call `FakeLockWithCheck::callback_.Run()`
// on scope entry AND exit.
//
// USAGE
//  class MyClass {
//    // ...
//
//    using FakeLockRunType = bool();
//
//    using FakeLockPolicy = basis::FakeLockPolicyDebugOnly;
//
//    MUST_USE_RETURN_VALUE
//    base::WeakPtr<Listener> weakSelf() const NO_EXCEPTION
//    {
//      /// \note `FakeLockPolicySkip` will NOT perform any checks.
//      /// No need to check thread-safety because `weak_this_`
//      /// can be passed safely between threads if not modified.
//      basis::AutoFakeLockWithCheck<basis::FakeLockPolicySkip, FakeLockRunType>
//        auto_lock(fakeLockToSequence);
//
//      // It is thread-safe to copy |base::WeakPtr|.
//      // Weak pointers may be passed safely between sequences, but must always be
//      // dereferenced and invalidated on the same SequencedTaskRunner otherwise
//      // checking the pointer would be racey.
//      return weak_this_;
//    }
//
//    // After constructing |weak_ptr_factory_|
//    // we immediately construct a WeakPtr
//    // in order to bind the WeakPtr object to its thread.
//    // When we need a WeakPtr, we copy construct this,
//    // which is safe to do from any
//    // thread according to weak_ptr.h (versus calling
//    // |weak_ptr_factory_.GetWeakPtr() which is not).
//    const base::WeakPtr<Listener> weak_this_
//      GUARDED_BY(fakeLockToSequence);
//
//    /// \note It is not real lock, only annotated as lock.
//    /// It just calls callback on scope entry AND exit.
//    basis::FakeLockWithCheck<FakeLockRunType> fakeLockToSequence{
//        base::BindRepeating(
//          &base::SequenceChecker::CalledOnValidSequence
//          , base::Unretained(&sequence_checker_)
//        )
//    };
//
//    // ...
//  };
template <
  typename FakeLockPolicyType
  , typename R
  , typename... Args
>
class SCOPED_LOCKABLE
  AutoFakeLockWithCheck<FakeLockPolicyType, R(Args...)>
{
 public:
  using RunType = R(Args...);

  AutoFakeLockWithCheck(
    const FakeLockWithCheck<RunType>& lock)
    EXCLUSIVE_LOCK_FUNCTION(lock)
    : lock_(lock)
  {
    if constexpr (FakeLockPolicyType::isDebugOnly
      && DCHECK_IS_ON())
    {
      DCHECK(lock_.Acquire());
    }
    // all except `isSkip` run check always
    if constexpr (!FakeLockPolicyType::isSkip)
    {
      CHECK(lock_.Acquire());
    }
  }

  ~AutoFakeLockWithCheck() UNLOCK_FUNCTION()
  {
    if constexpr (FakeLockPolicyType::isDebugOnly
      && DCHECK_IS_ON())
    {
      DCHECK(lock_.Release());
    }
    // all except `isSkip` run check always
    if constexpr (!FakeLockPolicyType::isSkip)
    {
      CHECK(lock_.Release());
    }
  }

 private:
  /// \note take care of reference limetime
  const FakeLockWithCheck<RunType>& lock_;

  DISALLOW_COPY_AND_ASSIGN(AutoFakeLockWithCheck);
};

} // namespace basis
