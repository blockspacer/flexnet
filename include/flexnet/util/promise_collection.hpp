// Copyright 2017 PDFium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include "flexnet/util/macros.hpp"

#include <set>

#include <base/location.h>
#include <base/logging.h>
#include <base/sequence_checker.h>

#include <basis/promise/promise.h>
#include <basis/promise/promise_value.h>

namespace util {

template <
  typename ResolveType
  , typename RejectType = base::NoReject
  >
class PromiseCollection
{
public:
  using PromiseType =
    base::Promise<ResolveType, RejectType>;

  // Define a total order based on the |task_runner| affinity, so that MDPs
  // belonging to the same SequencedTaskRunner are adjacent in the set.
  struct PromiseComparator {
    bool operator()(const PromiseType& a,
                    const PromiseType& b) const
    {
      return a.GetScopedRefptrForTesting()
             < b.GetScopedRefptrForTesting();
    }
  };

  using PromiseContainer =
    std::set<SHARED_LIFETIME(PromiseType), PromiseComparator>;

  bool empty() const
  {
    return promiseContainer_.empty();
  }

  typename PromiseContainer::size_type size() const
  {
    return promiseContainer_.size();
  }

  PromiseCollection()
  {
    DETACH_FROM_SEQUENCE(sequence_checker_);
  }

  ~PromiseCollection()
  {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  }

  PromiseType All(const base::Location& from_here)
  {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

    if(!promiseContainer_.empty()) {
      return base::Promises::All(FROM_HERE, promiseContainer_);
    }

    // dummy promise
    return PromiseType::CreateResolved(FROM_HERE);
  }

  void add(
    SHARED_LIFETIME(PromiseType)promise)
  {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

    promiseContainer_.emplace(promise);
  }

  void remove(
    SHARED_LIFETIME(PromiseType)boundPromise)
  {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

    base::EraseIf(
      promiseContainer_,
      [
        SHARED_LIFETIME(boundPromise)
      ](
        const PromiseType& key
        ){
        return key.GetScopedRefptrForTesting()
        == boundPromise.GetScopedRefptrForTesting();
      });
  }

private:
  SEQUENCE_CHECKER(sequence_checker_);

  PromiseContainer promiseContainer_
  LIVES_ON(sequence_checker_);
};

}  // namespace util
