#pragma once

#include <basis/ECS/ecs.hpp>
#include <basis/ECS/unsafe_context.hpp>

#include <base/rvalue_cast.h>
#include <base/path_service.h>
#include <base/optional.h>
#include <base/bind.h>
#include <base/run_loop.h>
#include <base/macros.h>
#include <base/logging.h>
#include <base/files/file_path.h>
#include <base/threading/platform_thread.h>
#include <base/threading/thread.h>
#include <base/task/thread_pool/thread_pool.h>
#include <base/stl_util.h>

#include <cstddef>
#include <cstdint>

namespace ECS {

// Each entity representing tcp connection
// must have that component.
struct TcpConnection
{
  // Shortcut for `.context`
  //
  // BEFORE
  // DCHECK(component.context.try_ctx_var<StrandComponent>());
  //
  // AFTER
  // DCHECK((*component).try_ctx_var<StrandComponent>());
  constexpr const UnsafeTypeContext& operator*() const
  {
    return context;
  }

  constexpr UnsafeTypeContext& operator*()
  {
    return context;
  }

  // Shortcut for `.context`
  //
  // BEFORE
  // DCHECK(component.context.try_ctx_var<StrandComponent>());
  //
  // AFTER
  // DCHECK(component->try_ctx_var<StrandComponent>());
  constexpr const UnsafeTypeContext* operator->() const
  {
    return &context;
  }

  constexpr UnsafeTypeContext* operator->()
  {
    return &context;
  }

  std::string debug_id;

  // Stores arbitrary UNIQUE 'child components' i.e. `context-components`.
  // Allows to compose entity out of arbitrary `context-components`
  // that can handle any complex logic (unlike ECS components).
  // Use `context-components` to represent ownership (think about RAII).
  // You can not not iterate `context-components` using ECS view or group
  // because `context-components` are not ECS components
  // (ECS registry not used at all for them).
  UnsafeTypeContext context{};
};

} // namespace ECS
