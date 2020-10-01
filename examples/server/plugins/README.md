# About `plugins`

Place here plugins (shared or static) that can be loaded by plugin manager (`Corrade::PluginManager`).

## NOTICE

Make sure that each symbol in plugin has unique-per-plugin namespace.

To avaid symbol collision - avoid global AND anonymous namespace, even for static vars or functions.

```cpp

// BEFORE, WRONG

namepace {
const int kGlobalBad = 1;
void foo()
{
  // ...
}
} // namepace

// AFTER, GOOD
// Use unique namespace for ALL plugin code.

namepace MyPlugin {
const int kGlobalBad = 1;
static void foo()
{
  // ...
}
} // namepace MyPlugin
```
