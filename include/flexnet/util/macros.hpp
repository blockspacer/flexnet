#pragma once

#include <base/compiler_specific.h> // IWYU pragma: keep
#include <base/macros.h> // IWYU pragma: keep

// documents that moved-from object will be in the same state
// as if constructed using moved-from object the constructor.
/// \example boost.org/doc/libs/1_54_0/doc/html/boost_asio/reference/basic_stream_socket/basic_stream_socket/overload5.html
/// i.e. it does not actually destroy |stream| by |move|
#define COPY_ON_MOVE(x) x

// documents that value must be created/modified/used
// only from one base::Sequence
/// \todo integrate with thread-safety annotations
#define LIVES_ON(sequenceChecker)

// similar to __attribute__((warn_unused_result))
/// \usage (note order restriction)
/// [[nodisard]] extern bool foo();
/// [[nodisard]] inline bool foo();
/// [[nodisard]] static bool foo();
/// [[nodisard]] static inline bool foo();
/// [[nodisard]] virtual bool foo();
#define MUST_USE_RETURN_VALUE \
  [[nodiscard]] /* do not ignore return value */

/// \usage
/// NEW_NO_THROW(FROM_HERE,
///   ptr // lhs of assignment
///   , SomeType(somearg1, somearg2) // rhs of assignment
///   , LOG(ERROR) // log allocation failure
/// );
#define NEW_NO_THROW(from_here, lhs, rhs, FAILED_LOG_STREAM) \
  DCHECK(!lhs); \
  lhs = new(std::nothrow/*, from_here.file_name(), from_here.line_number()*/) rhs; \
  if(!lhs) \
  { \
    FAILED_LOG_STREAM \
      << "failed to allocate " \
      << from_here.ToString(); \
  }

/// \usage
/// DELETE_NOT_ARRAY_AND_NULLIFY(FROM_HERE, ptr);
#define DELETE_NOT_ARRAY_AND_NULLIFY(from_here, x) \
  DCHECK(x) \
      << "failed to deallocate " \
      << from_here.ToString(); \
  delete x; \
  x = nullptr; \
  DCHECK(!x) \
      << "failed to deallocate " \
      << from_here.ToString();

// Documents that value may not be thread-safe in general,
// but because it can be modified only during
// `initialization` and `teardown` steps
// it can be used by multiple threads during `running` step.
#define GLOBAL_THREAD_SAFETY(x) x

// Documents that value can NOT be used from
// any thread without extra thread-safety checks.
// i.e. take care of possible thread-safety bugs.
// Usually it means that value MUST be guarded by some mutex lock
// OR modified only during `initialization` step.
/// \note prefer sequence checkers or
/// <base/thread_collision_warner.h> to it if possible
#define CAUTION_NOT_THREAD_SAFE(x) x

// Documents that value can be used from any thread.
// Usually it means that value is guarded by some mutex lock.
#define THREAD_SAFE(x) x

/// \note prefer |MoveOnly| to |COPIED|
// Documents that value will be copied.
/// \note use it to annotate arguments that are bound to function
#define COPIED(x) x

/// \note prefer `REFERENCED` to `RAW_REFERENCED`
// Documents that value will be used as alias
// i.e. another name for an already existing variable.
#define RAW_REFERENCED(x) x

// Documents that value will be used as alias
// i.e. another name for an already existing variable.
/// \note use it to annotate arguments that are bound to function
#define REFERENCED(x) std::ref(x)

#define CONST_REFERENCED(x) std::cref(x)

// Documents that value has shared storage
// like shaped_ptr, scoped_refptr, etc.
// i.e. that object lifetime will be prolonged.
/// \note use it to annotate arguments that are bound to function
#define SHARED_LIFETIME(x) x

// Documents that function returns promise,
// so next ThenOn/ThenHere will `wait` (asynchronously) for NESTED promise
#define NESTED_PROMISE(x) x

// Documents that value has external storage
// i.e. that object lifetime not conrolled.
// If you found lifetime-related bug,
// than you can `grep-search` for |UNOWNED_LIFETIME| in code.
/// \note see also |UnownedPtr|
/// \note use it to annotate arguments that are bound to function
/// \example
///   beast::async_detect_ssl(
///     stream_, // The stream to read from
///     buffer_, // The dynamic buffer to use
///     boost::asio::bind_executor(perConnectionStrand_,
///       std::bind(
///         &DetectChannel::onDetected
///         , /// \note Lifetime must be managed externally.
///           /// API user can free |DetectChannel| only if
///           /// that callback finished (or failed to schedule).
///           UNOWNED_LIFETIME(
///             COPIED(this))
///         , std::placeholders::_1
///         , std::placeholders::_2
///       )
///     )
///   );
#define UNOWNED_LIFETIME(x) x

/**
 * @usage
  if (ec == ::boost::asio::error::connection_aborted)
  {
    LOG_ERROR_CODE(LOG(WARNING),
      "Listener failed with"
      " connection_aborted error: ", what, ec);
  }
 **/
#define LOG_ERROR_CODE(LOG_STREAM, description, what, ec) \
  LOG_STREAM  \
    << description  \
    << what  \
    << ": "  \
    << ec.message()

/**
 * @usage
  LOG_CALL(VLOG(9));
 **/
#define LOG_CALL(LOG_STREAM) \
  LOG_STREAM \
    << "called " \
    << FROM_HERE.ToString()


#if defined(COMPILER_MSVC)

// For _Printf_format_string_.
#include <sal.h>

// Macros for suppressing and disabling warnings on MSVC.
//
// Warning numbers are enumerated at:
// http://msdn.microsoft.com/en-us/library/8x5x43k7(VS.80).aspx
//
// The warning pragma:
// http://msdn.microsoft.com/en-us/library/2c8f766e(VS.80).aspx
//
// Using __pragma instead of #pragma inside macros:
// http://msdn.microsoft.com/en-us/library/d9x1s805.aspx

// MSVC_SUPPRESS_WARNING disables warning |n| for the remainder of the line and
// for the next line of the source file.
#if !defined(MSVC_SUPPRESS_WARNING)
#define MSVC_SUPPRESS_WARNING(n) __pragma(warning(suppress:n))
#endif // !defined(MSVC_SUPPRESS_WARNING)

// Macros for suppressing and disabling warnings on MSVC.
//
// Warning numbers are enumerated at:
// http://msdn.microsoft.com/en-us/library/8x5x43k7(VS.80).aspx
//
// The warning pragma:
// http://msdn.microsoft.com/en-us/library/2c8f766e(VS.80).aspx
//
// Using __pragma instead of #pragma inside macros:
// http://msdn.microsoft.com/en-us/library/d9x1s805.aspx

#if !defined(MSVC_PUSH_DISABLE_WARNING)
// MSVC_PUSH_DISABLE_WARNING pushes |n| onto a stack of warnings to be disabled.
// The warning remains disabled until popped by MSVC_POP_WARNING.
#define MSVC_PUSH_DISABLE_WARNING(n) __pragma(warning(push)) \
                                     __pragma(warning(disable:n))
#endif // !defined(MSVC_PUSH_DISABLE_WARNING)

// Pop effects of innermost MSVC_PUSH_* macro.
#if !defined(MSVC_POP_WARNING)
#define MSVC_POP_WARNING() __pragma(warning(pop))
#endif // !defined(MSVC_POP_WARNING)

// Allows |this| to be passed as an argument in constructor initializer lists.
// This uses push/pop instead of the seemingly simpler suppress feature to avoid
// having the warning be disabled for more than just |code|.
//
// Example usage:
// Foo::Foo() : x(NULL), ALLOW_THIS_IN_INITIALIZER_LIST(y(this)), z(3) {}
//
// Compiler warning C4355: 'this': used in base member initializer list:
// http://msdn.microsoft.com/en-us/library/3c594ae3(VS.80).aspx
#if !defined(ALLOW_THIS_IN_INITIALIZER_LIST)
#define ALLOW_THIS_IN_INITIALIZER_LIST(code) \
  MSVC_PUSH_DISABLE_WARNING(4355)            \
  code MSVC_POP_WARNING()
#endif // !defined(ALLOW_THIS_IN_INITIALIZER_LIST)

#else  // Not MSVC

#if !defined(ALLOW_THIS_IN_INITIALIZER_LIST)
#define ALLOW_THIS_IN_INITIALIZER_LIST(code) code
#endif // !defined(ALLOW_THIS_IN_INITIALIZER_LIST)

#endif  // COMPILER_MSVC

// Macro used to simplify the task of deleting the new and new[]
// operators i.e, disallow heap allocations.
/// \note accepts |ClassName| argument
/// for documentation purposes and to avoid copy-n-paste errors
#define DISALLOW_NEW_OPERATOR(ClassName)                         \
  static void* operator new(size_t) = delete;   \
  static void* operator new[](size_t) = delete

// Use Clang's thread safety analysis annotations when available. In other
// environments, the macros receive empty definitions.
// Usage documentation: https://clang.llvm.org/docs/ThreadSafetyAnalysis.html

#if !defined(THREAD_ANNOTATION_ATTRIBUTE__)

#if defined(__clang__)

#define THREAD_ANNOTATION_ATTRIBUTE__(x) __attribute__((x))
#else
#define THREAD_ANNOTATION_ATTRIBUTE__(x)  // no-op
#endif // defined(__clang__)

#endif  // !defined(THREAD_ANNOTATION_ATTRIBUTE__)

#ifndef GUARDED_BY
#define GUARDED_BY(x) THREAD_ANNOTATION_ATTRIBUTE__(guarded_by(x))
#endif

#ifndef PT_GUARDED_BY
#define PT_GUARDED_BY(x) THREAD_ANNOTATION_ATTRIBUTE__(pt_guarded_by(x))
#endif

#ifndef ACQUIRED_AFTER
#define ACQUIRED_AFTER(...) \
  THREAD_ANNOTATION_ATTRIBUTE__(acquired_after(__VA_ARGS__))
#endif

#ifndef ACQUIRED_BEFORE
#define ACQUIRED_BEFORE(...) \
  THREAD_ANNOTATION_ATTRIBUTE__(acquired_before(__VA_ARGS__))
#endif

#ifndef EXCLUSIVE_LOCKS_REQUIRED
#define EXCLUSIVE_LOCKS_REQUIRED(...) \
  THREAD_ANNOTATION_ATTRIBUTE__(exclusive_locks_required(__VA_ARGS__))
#endif

#ifndef SHARED_LOCKS_REQUIRED
#define SHARED_LOCKS_REQUIRED(...) \
  THREAD_ANNOTATION_ATTRIBUTE__(shared_locks_required(__VA_ARGS__))
#endif

#ifndef LOCKS_EXCLUDED
#define LOCKS_EXCLUDED(...) \
  THREAD_ANNOTATION_ATTRIBUTE__(locks_excluded(__VA_ARGS__))
#endif

#ifndef LOCK_RETURNED
#define LOCK_RETURNED(x) THREAD_ANNOTATION_ATTRIBUTE__(lock_returned(x))
#endif

#ifndef LOCKABLE
#define LOCKABLE THREAD_ANNOTATION_ATTRIBUTE__(lockable)
#endif

#ifndef SCOPED_LOCKABLE
#define SCOPED_LOCKABLE THREAD_ANNOTATION_ATTRIBUTE__(scoped_lockable)
#endif

#ifndef EXCLUSIVE_LOCK_FUNCTION
#define EXCLUSIVE_LOCK_FUNCTION(...) \
  THREAD_ANNOTATION_ATTRIBUTE__(exclusive_lock_function(__VA_ARGS__))
#endif

#ifndef SHARED_LOCK_FUNCTION
#define SHARED_LOCK_FUNCTION(...) \
  THREAD_ANNOTATION_ATTRIBUTE__(shared_lock_function(__VA_ARGS__))
#endif

#ifndef EXCLUSIVE_TRYLOCK_FUNCTION
#define EXCLUSIVE_TRYLOCK_FUNCTION(...) \
  THREAD_ANNOTATION_ATTRIBUTE__(exclusive_trylock_function(__VA_ARGS__))
#endif

#ifndef SHARED_TRYLOCK_FUNCTION
#define SHARED_TRYLOCK_FUNCTION(...) \
  THREAD_ANNOTATION_ATTRIBUTE__(shared_trylock_function(__VA_ARGS__))
#endif

#ifndef UNLOCK_FUNCTION
#define UNLOCK_FUNCTION(...) \
  THREAD_ANNOTATION_ATTRIBUTE__(unlock_function(__VA_ARGS__))
#endif

#ifndef NO_THREAD_SAFETY_ANALYSIS
#define NO_THREAD_SAFETY_ANALYSIS \
  THREAD_ANNOTATION_ATTRIBUTE__(no_thread_safety_analysis)
#endif

#ifndef ASSERT_EXCLUSIVE_LOCK
#define ASSERT_EXCLUSIVE_LOCK(...) \
  THREAD_ANNOTATION_ATTRIBUTE__(assert_exclusive_lock(__VA_ARGS__))
#endif

#ifndef ASSERT_SHARED_LOCK
#define ASSERT_SHARED_LOCK(...) \
  THREAD_ANNOTATION_ATTRIBUTE__(assert_shared_lock(__VA_ARGS__))
#endif

// BAD_CALL_IF()
//
// Used on a function overload to trap bad calls: any call that matches the
// overload will cause a compile-time error. This macro uses a clang-specific
// "enable_if" attribute, as described at
// http://clang.llvm.org/docs/AttributeReference.html#enable-if
//
// Overloads which use this macro should be bracketed by
// `#ifdef BAD_CALL_IF`.
//
// Example:
//
//   int isdigit(int c);
//   #ifdef BAD_CALL_IF
//   int isdigit(int c)
//     BAD_CALL_IF(c <= -1 || c > 255,
//                       "'c' must have the value of an unsigned char or EOF");
//   #endif // BAD_CALL_IF

#if defined(__clang__)
# if __has_attribute(enable_if)
#  define BAD_CALL_IF(expr, msg) \
    __attribute__((enable_if(expr, "Bad call trap"), unavailable(msg)))
# endif
#endif
