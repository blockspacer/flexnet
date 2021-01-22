// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.
//
// The following only applies to changes made to this file as part of YugaByte development.
//
// Portions Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//

#include "tests_common.h"

#include "app_state.hpp"

#include "base/test/scoped_task_environment.h"
#include "base/test/bind_test_util.h"
#include "base/test/gtest_util.h"
#include "base/test/test_simple_task_runner.h"

#include <basis/status/status_macros.hpp>
#include <basis/promise/post_promise.h>

#include <base/atomicops.h>
#include <base/threading/thread_local.h>
#include <base/threading/platform_thread.h>
#include <base/synchronization/lock.h>
#include <base/threading/thread.h>
#include <base/strings/substitute.h>
#include <base/run_loop.h>

#include <memory>

namespace backend {

class AppStateTest : public testing::Test {
 public:
  AppStateTest() = default;
  void SetUp() override {
  }

 protected:
  base::test::ScopedTaskEnvironment scoped_task_environment_;
};

TEST_F(AppStateTest, TestInvalidStateChange) {
  // The default style "fast" does not support multi-threaded tests
  // (introduces deadlock on Linux).
  ::testing::FLAGS_gtest_death_test_style = "threadsafe";

  std::unique_ptr<AppState> appState
    = std::make_unique<AppState>(AppState::UNINITIALIZED);

  {
#if DCHECK_IS_ON()
     EXPECT_DCHECK_DEATH({
       appState->processStateChange(
         FROM_HERE
         , AppState::SUSPEND);
     });
#else
     ::basis::Status result =
       appState->processStateChange(
         FROM_HERE
         , AppState::SUSPEND);
     // can not change to SUSPENDED state from UNINITIALIZED state
     EXPECT_NOT_OK(result);
     EXPECT_EQ(appState->currentState(), AppState::UNINITIALIZED);
#endif // DCHECK_IS_ON()
  }

  {
     ::basis::Status result =
       appState->processStateChange(
         FROM_HERE
         , AppState::TERMINATE);
     EXPECT_OK(result);
     EXPECT_EQ(appState->currentState(), AppState::TERMINATED);
  }
}

TEST_F(AppStateTest, TestSameStateChange) {
  // The default style "fast" does not support multi-threaded tests
  // (introduces deadlock on Linux).
  ::testing::FLAGS_gtest_death_test_style = "threadsafe";

  std::unique_ptr<AppState> appState
    = std::make_unique<AppState>(AppState::UNINITIALIZED);

  {
     ::basis::Status result =
       appState->processStateChange(
         FROM_HERE
         , AppState::START);
     EXPECT_OK(result);
     EXPECT_EQ(appState->currentState(), AppState::STARTED);
  }

  {
#if DCHECK_IS_ON()
     EXPECT_DCHECK_DEATH({
       appState->processStateChange(
         FROM_HERE
         , AppState::START);
     });
#else
     ::basis::Status result =
       appState->processStateChange(
         FROM_HERE
         , AppState::START);
     // can not change to STARTED state from STARTED state
     EXPECT_NOT_OK(result);
     EXPECT_EQ(appState->currentState(), AppState::STARTED);
#endif // DCHECK_IS_ON()
  }

  {
     ::basis::Status result =
       appState->processStateChange(
         FROM_HERE
         , AppState::TERMINATE);
     EXPECT_OK(result);
     EXPECT_EQ(appState->currentState(), AppState::TERMINATED);
  }
}

TEST_F(AppStateTest, TestBasic) {
  std::unique_ptr<AppState> appState
    = std::make_unique<AppState>(AppState::UNINITIALIZED);

  {
     ::basis::Status result =
       appState->processStateChange(
         FROM_HERE
         , AppState::START);
     EXPECT_OK(result);
     EXPECT_EQ(appState->currentState(), AppState::STARTED);
  }

  {
     ::basis::Status result =
       appState->processStateChange(
         FROM_HERE
         , AppState::TERMINATE);
     EXPECT_OK(result);
     EXPECT_EQ(appState->currentState(), AppState::TERMINATED);
  }

  ::base::WeakPtr<AppState> weakAppState
    = appState->weakSelf();

  appState.reset();

  auto runner = ::base::ThreadPool::GetInstance()->
    CreateSequencedTaskRunnerWithTraits(
      ::base::TaskTraits{
        ::base::TaskPriority::BEST_EFFORT
        , ::base::MayBlock()
        , ::base::TaskShutdownBehavior::BLOCK_SHUTDOWN
      }
    );

  base::RunLoop run_loop;

  ::base::PostPromise(FROM_HERE
    , runner.get()
    , ::base::BindOnce([
        ](::base::WeakPtr<AppState> weakPtr){
          // see `appState.reset()` above
          EXPECT_FALSE(weakPtr.MaybeValid());
        }
        , weakAppState
      )
  )
  .ThenHere(FROM_HERE, run_loop.QuitClosure());


  run_loop.Run();
  scoped_task_environment_.RunUntilIdle();
}

}  // namespace backend
