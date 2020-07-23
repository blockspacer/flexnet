#pragma once

#include <base/macros.h> // IWYU pragma: keep
#include <base/compiler_specific.h> // IWYU pragma: keep

// Documents that value can be used from any thread.
// Usually it means that value is guarded by some mutex lock.
#define THREAD_SAFE(x) x

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
    << ec.message();

/**
 * @usage
  LOG_CALL(VLOG(9));
 **/
#define LOG_CALL(LOG_STREAM) \
  LOG_STREAM \
    << "called " \
    << FROM_HERE.ToString();


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
