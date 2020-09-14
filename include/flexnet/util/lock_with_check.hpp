#pragma once

#include "flexnet/util/verify_nothing.hpp"

#include <base/macros.h>
#include <base/sequence_checker.h>
#include <base/callback.h>
#include <base/optional.h>
#include <base/location.h>
#include <base/rvalue_cast.h>
#include <base/bind_helpers.h>
#include <base/strings/string_piece.h>
#include <base/threading/thread_collision_warner.h>
#include <base/sequenced_task_runner.h>
#include <base/thread_annotations.h>

#include <basis/bitmask.h>

#include <boost/algorithm/string.hpp>
#include <boost/asio.hpp>

#include <functional>
#include <map>
#include <string>

namespace basis {

// Allows to use `Type` with clang thread-safety annotations like `GUARDED_BY`.
// See http://clang.llvm.org/docs/ThreadSafetyAnalysis.html
template<typename Type>
struct LOCKABLE AnnotateLockable
{
  template <class... Args>
  AnnotateLockable(
    Args&&... args)
    : data(std::forward<Args>(args)...)
  {}

  constexpr const Type& operator*() const
  {
    return data;
  }

  constexpr Type& operator*()
  {
    return data;
  }

  constexpr const Type* operator->() const
  {
    return &data;
  }

  constexpr Type* operator->()
  {
    return &data;
  }

  using StoredType = Type;

  Type data;
};

// Helper class used by DCHECK_RUN_ON
class SCOPED_LOCKABLE SequenceCheckerScope {
 public:
  explicit SequenceCheckerScope(
    const base::SequenceChecker* thread_like_object)
      EXCLUSIVE_LOCK_FUNCTION(thread_like_object) {}

  SequenceCheckerScope(
    const SequenceCheckerScope&) = delete;

  SequenceCheckerScope& operator=(
    const SequenceCheckerScope&) = delete;

  ~SequenceCheckerScope() UNLOCK_FUNCTION() {}

  static bool CalledOnValidSequence(
    const base::SequenceChecker* thread_like_object)
  {
    return thread_like_object->CalledOnValidSequence();
  }
};

// Helper class used by DCHECK_RUN_ON
class SCOPED_LOCKABLE SequencedTaskRunnerScope {
 public:
  explicit SequencedTaskRunnerScope(
    const base::SequencedTaskRunner* thread_like_object)
      EXCLUSIVE_LOCK_FUNCTION(thread_like_object) {}

  SequencedTaskRunnerScope(
    const SequencedTaskRunnerScope&) = delete;

  SequencedTaskRunnerScope& operator=(
    const SequencedTaskRunnerScope&) = delete;

  ~SequencedTaskRunnerScope() UNLOCK_FUNCTION() {}

  static bool RunsTasksInCurrentSequence(
    const base::SequencedTaskRunner* thread_like_object)
  {
    return thread_like_object->RunsTasksInCurrentSequence();
  }
};

// Allows to use `boost::asio::strand` with clang thread-safety annotations like `GUARDED_BY`.
// See http://clang.llvm.org/docs/ThreadSafetyAnalysis.html
template <typename Executor>
using AnnotatedStrand
  = basis::AnnotateLockable<
      ::boost::asio::strand<Executor>
    >;

// Helper class used by DCHECK_RUN_ON_STRAND
template <typename Executor>
class SCOPED_LOCKABLE StrandCheckerScope {
 public:
  using AnnotatedStrandType
    = basis::AnnotateLockable<
        boost::asio::strand<Executor>>;

  explicit StrandCheckerScope(
    const AnnotatedStrandType* thread_like_object)
      EXCLUSIVE_LOCK_FUNCTION(thread_like_object) {}

  StrandCheckerScope(
    const StrandCheckerScope&) = delete;

  StrandCheckerScope& operator=(
    const StrandCheckerScope&) = delete;

  ~StrandCheckerScope() UNLOCK_FUNCTION() {}

  static bool running_in_this_thread(
    const AnnotatedStrandType* thread_like_object)
  {
    return thread_like_object->data.running_in_this_thread();
  }
};

// RUN_ON/GUARDED_BY/DCHECK_RUN_ON macros allows to annotate
// variables are accessed from same thread/task queue.
// Using tools designed to check mutexes, it checks at compile time everywhere
// variable is access, there is a run-time dcheck thread/task queue is correct.
//
// class ThreadExample {
//  public:
//   void NeedVar1() {
//     DCHECK_RUN_ON(network_thread_);
//     transport_->Send();
//   }
//
//  private:
//   Thread* network_thread_;
//   int transport_ GUARDED_BY(network_thread_);
// };
//
// class SequenceCheckerExample {
//  public:
//   int CalledFromPacer() RUN_ON(pacer_sequence_checker_) {
//     return var2_;
//   }
//
//   void CallMeFromPacer() {
//     DCHECK_RUN_ON(&pacer_sequence_checker_)
//        << "Should be called from pacer";
//     CalledFromPacer();
//   }
//
//  private:
//   int pacer_var_ GUARDED_BY(pacer_sequence_checker_);
//   SequenceChecker pacer_sequence_checker_;
// };
//
// class TaskQueueExample {
//  public:
//   class Encoder {
//    public:
//     TaskQueue* Queue() { return encoder_queue_; }
//     void Encode() {
//       DCHECK_RUN_ON(encoder_queue_);
//       DoSomething(var_);
//     }
//
//    private:
//     TaskQueue* const encoder_queue_;
//     Frame var_ GUARDED_BY(encoder_queue_);
//   };
//
//   void Encode() {
//     // Will fail at runtime when DCHECK is enabled:
//     // encoder_->Encode();
//     // Will work:
//     scoped_refptr<Encoder> encoder = encoder_;
//     encoder_->Queue()->PostTask([encoder] { encoder->Encode(); });
//   }
//
//  private:
//   scoped_refptr<Encoder> encoder_;
// }

// Document if a function expected to be called from same thread/task queue.
#define RUN_ON(x) \
  THREAD_ANNOTATION_ATTRIBUTE__(exclusive_locks_required(x))

// Type of `x` is `base::SequenceChecker*`
#define DCHECK_RUN_ON(x)                                              \
  basis::SequenceCheckerScope seq_check_scope(x); \
  DCHECK((x)); \
  DCHECK((x)->CalledOnValidSequence())

// Type of `x` is `base::SequencedTaskRunner*`
//
// USAGE
//
// scoped_refptr<base::SequencedTaskRunner> periodicVerifyRunner_
//   // It safe to read value from any thread because its storage
//   // expected to be not modified (if properly initialized)
//   SET_CUSTOM_THREAD_GUARD(periodicVerifyRunner_);
// // ...
// DCHECK_CUSTOM_THREAD_GUARD(periodicVerifyRunner_);
// DCHECK(periodicVerifyRunner_);
// DCHECK_RUN_ON_SEQUENCED_RUNNER(periodicVerifyRunner_.get());
#define DCHECK_RUN_ON_SEQUENCED_RUNNER(x)                                              \
  basis::SequencedTaskRunnerScope seq_rask_runner_scope(x); \
  DCHECK((x)); \
  DCHECK((x)->RunsTasksInCurrentSequence())

// Type of `x` is `basis::AnnotatedStrand&`
//
// EXAMPLE
//
// using ExecutorType
//   = StreamType::executor_type;
//
// using StrandType
//   = ::boost::asio::strand<ExecutorType>;
//
// // |stream_| and calls to |async_*| are guarded by strand
// basis::AnnotatedStrand<ExecutorType> perConnectionStrand_
//   SET_CUSTOM_THREAD_GUARD_WITH_CHECK(
//     perConnectionStrand_
//     // 1. It safe to read value from any thread
//     // because its storage expected to be not modified.
//     // 2. On each access to strand check that stream valid
//     // otherwise `::boost::asio::post` may fail.
//     , base::BindRepeating(
//       [
//       ](
//         bool is_stream_valid
//         , StreamType& stream
//       ){
//         /// \note |perConnectionStrand_|
//         /// is valid as long as |stream_| valid
//         /// i.e. valid util |stream_| moved out
//         /// (it uses executor from stream).
//         return is_stream_valid;
//       }
//       , is_stream_valid_.load()
//       , REFERENCED(stream_.value())
//     ));
//
// DCHECK_RUN_ON_STRAND(&perConnectionStrand_, ExecutorType);
//
#define DCHECK_RUN_ON_STRAND(x, Type)                                              \
  basis::StrandCheckerScope<Type> strand_check_scope(x); \
  DCHECK((x)); \
  DCHECK((x)->data.running_in_this_thread())

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

/// \note prefer `DCHECK_RUN_ON` to `FakeLockWithCheck` where possible.
/// \note It is not real lock, only annotated as lock.
/// It just calls callback on scope entry AND exit.
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
    DCHECK(callback_);
    return callback_.Run();
  }

  MUST_USE_RETURN_VALUE
  bool Release() const NO_EXCEPTION UNLOCK_FUNCTION()
  {
    DCHECK(callback_);
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
//        auto_lock(fakeLockToSequence_);
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
//      GUARDED_BY(fakeLockToSequence_);
//
//    /// \note It is not real lock, only annotated as lock.
//    /// It just calls callback on scope entry AND exit.
//    basis::FakeLockWithCheck<FakeLockRunType> fakeLockToSequence_{
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

// Used for documentation purposes (checks nothing).
/// \note It is not real lock, only annotated as lock.
/// It just calls callback on scope entry AND exit.
extern basis::FakeLockWithCheck<bool()>
  fakeLockDocumentNotThreadChecked;

// Documents that variable allowed to be used from any thread
// and you MUST take care of thread-safety somehow.
//
// USE CASE EXAMPLES
//
// 1. Unmodified global variable can be used from any thread
// (if properly initialized).
// It safe to read value from any thread
// because its storage expected to be not modified,
// we just need to check storage validity.
// 2. Thread-safe type (like atomic).
#define ANY_THREAD_GUARD() \
  basis::fakeLockDocumentNotThreadChecked

#define GUARDED_BY_ANY_THREAD() \
  GUARDED_BY(ANY_THREAD_GUARD())

/// \note Prefer instead RUN_ON(ANY_THREAD_GUARD(), m1, m2)
// Documents that function allowed to be used from any thread
// and you MUST take care of thread-safety somehow.
#define RUN_ON_ANY_THREAD() \
  THREAD_ANNOTATION_ATTRIBUTE__(exclusive_locks_required(ANY_THREAD_GUARD()))

// Allow to use code that can be used from any thread
// (in current scope only).
//
// Used for documentation purposes, so it is good idea
// to pass affected function names, variable names, etc.
//
// EXAMPLE
// class MySharedClass {
//   ~MySharedClass()
//   {
//     // documents that destructor called from any thread
//     DCHECK_RUN_ON_ANY_THREAD(MySharedClass);
//   }
// };
//
// // documents that `periodicRunner_` and `someVar` used from any thread
// DCHECK_RUN_ON_ANY_THREAD(periodicRunner_ && someVar);
// DCHECK(periodicRunner_.doSomeTask(someVar));
#define DCHECK_RUN_ON_ANY_THREAD(x) \
  basis::AutoFakeLockWithCheck<basis::FakeLockPolicyDebugOnly, bool()> \
    auto_lock_run_on_any_thread( \
      ANY_THREAD_GUARD())

#define CUSTOM_THREAD_GUARD(Name) \
  fakeLockDummy##Name

// Per-variable alternative to `GUARDED_BY`
// Documents that you must take care of thread safety somehow.
// That allows to notice (find & debug & fix) code
// that can be used from multiple threads.
#define SET_CUSTOM_THREAD_GUARD(Name) \
  GUARDED_BY(CUSTOM_THREAD_GUARD(Name)); \
  basis::FakeLockWithCheck<bool()> \
    CUSTOM_THREAD_GUARD(Name) { \
    basis::VerifyNothing::Repeatedly() \
  }

// USAGE
//
//   basis::AnnotatedStrand<ExecutorType> perConnectionStrand_
//     SET_CUSTOM_THREAD_GUARD_WITH_CHECK(
//       perConnectionStrand_
//       // 1. It safe to read value from any thread
//       // because its storage expected to be not modified.
//       // 2. On each access to strand check that stream valid
//       // otherwise `::boost::asio::post` may fail.
//       , base::BindRepeating(
//         [
//         ](
//           bool is_stream_valid
//           , StreamType& stream
//         ){
//           /// \note |perConnectionStrand_|
//           /// is valid as long as |stream_| valid
//           /// i.e. valid util |stream_| moved out
//           /// (it uses executor from stream).
//           return is_stream_valid;
//         }
//         , REFERENCED(is_stream_valid_)
//         , REFERENCED(stream_.value())
//       ));
#define SET_CUSTOM_THREAD_GUARD_WITH_CHECK(Name, Callback) \
  GUARDED_BY(CUSTOM_THREAD_GUARD(Name)); \
  basis::FakeLockWithCheck<bool()> \
    CUSTOM_THREAD_GUARD(Name) { \
    basis::VerifyNothing::Repeatedly() \
  }

/// \note Prefer instead RUN_ON(CUSTOM_THREAD_GUARD(Name), m1, m2)
// Per-variable alternative to `RUN_ON`
// Documents that you must take care of thread safety somehow.
// That allows to notice (find & debug & fix) code
// that can be used from multiple threads.
#define USE_CUSTOM_THREAD_GUARD(Name) \
  THREAD_ANNOTATION_ATTRIBUTE__(exclusive_locks_required(CUSTOM_THREAD_GUARD(Name)))

// Per-variable alternative to `DCHECK_RUN_ON`
// Documents that you must take care of thread safety somehow.
// That allows to notice (find & debug & fix) code
// that can be used from multiple threads.
#define DCHECK_CUSTOM_THREAD_GUARD(Name) \
  basis::AutoFakeLockWithCheck<basis::FakeLockPolicyDebugOnly, bool()> \
    auto_lock_run_on_##Name( \
      CUSTOM_THREAD_GUARD(Name))

} // namespace basis
