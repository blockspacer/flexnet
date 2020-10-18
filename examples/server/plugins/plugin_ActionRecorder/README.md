# About

Plugin logs information about triggered user actions etc.

User actions inspired by `chrome://user-actions`. See https://chromium.googlesource.com/chromium/src/+/master/tools/metrics/actions/README.md and https://chromium.googlesource.com/chromium/src.git/+/master/tools/metrics/histograms/README.md

User actions come with only a name and a timestamp. They are best used when you care about a sequence--which actions happen in what order. If you don't care about the order, you should be using histograms (likely enumerated histograms).

Uses `base/metrics/user_metrics_action.h` to do `base::SetRecordActionTaskRunner` and `base::AddActionCallback`

Generally a logged user action should correspond with a single, uh, action by the user. :-) As such, they should probably only appear in a single place in the code. If the same user action needs to be logged in multiple places, consider whether you should be using different user action names for these separate call paths.

That said, if you truly need to record the same user action in multiple places, that's okay. Use a compile-time constant of appropriate scope that can be referenced everywhere. Using inline strings in multiple places can lead to errors if you ever need to revise the name and you update one one location and forget another.

Due to the practices about when and how often to emit a user action, actions should not be emitted often enough to cause efficiency issues.

## Usage

You may want to log user actions to separate log file and perform periodic transmission of collected log data to an external server.

Record action:

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

Add action callback:

```cpp
base::RemoveActionCallback(action_callback_);
action_callback_ = base::Bind(&MetricsService::OnUserAction,
                              base::Unretained(this));
base::AddActionCallback(action_callback_);
```

```cpp
void MetricsService::OnUserAction(const std::string& action) {
  if (!CanLogNotification())
    return;

  log_manager_.current_log()->RecordUserAction(action.c_str());
  HandleIdleSinceLastTransmission(false);
}
```
