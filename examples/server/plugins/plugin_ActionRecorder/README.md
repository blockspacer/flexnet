# About

Plugin logs information about triggered used actions etc.

Uses `base/metrics/user_metrics_action.h` to do `base::SetRecordActionTaskRunner` and `base::AddActionCallback`

## Usage

```cpp
#include <base/trace_event/trace_event.h>
#include <base/trace_event/trace_log.h>
#include <base/metrics/histogram.h>
#include <base/metrics/histogram_macros.h>
#include <base/metrics/statistics_recorder.h>
#include <base/metrics/user_metrics.h>
#include <base/metrics/user_metrics_action.h>

base::RecordAction(
  base::UserMetricsAction("Login_Success"));
```
