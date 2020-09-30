# About

Plugin manager features like:

- Global configuration for all plugins.
- Plugin hierarchy (`depends=...`).
- etc.

## Planned features

- Plugin aliases.
- Per-plugin configuration, not only global configuration for all plugins.
- Dynamic plugin loading/unloading (via command-line etc.) that returns hierarchy that required to loaded (or unloaded) in case of dependency errors.

## Global configuration for all plugins

If global configuration does not have plugins section (empty),
than all plugins will be loaded from plugins directory (also static plugins will be loaded).

If global configuration has not-empty plugins section,
than all listed plugin names (or aliases) expected be able to load (you will get error otherwise).

`depends=` in configuration can be used to control loading and unloading order of plugins:

Example loading and unloading order (based on config file below):

```txt
-> loading `ConsoleTerminal`
-> loading `AsioContextThreads`
-> loading `ExtraLogging`
-> loading `NetworkEntity`
-> loading `SignalHandler`
-> loading `TcpServer`
-> loading `BasicConsoleCommands`
<- unloading `ExtraLogging`
<- unloading `BasicConsoleCommands`
<- unloading `TcpServer`
<- unloading `ConsoleTerminal`
<- unloading `SignalHandler`
<- unloading `NetworkEntity`
<- unloading `AsioContextThreads`
```

Example configuration file:

```txt
# configuration file format https://doc.magnum.graphics/corrade/classCorrade_1_1Utility_1_1Configuration.html

# you can remove plugins section to load all plugins from plugins directory
[plugins]

[plugins/plugin]
title=ExtraLogging

[plugins/plugin]
title=AsioContextThreads

[plugins/plugin]
title=SignalHandler
depends=AsioContextThreads

[plugins/plugin]
title=NetworkEntity
depends=AsioContextThreads

[plugins/plugin]
title=TcpServer
depends=AsioContextThreads
depends=SignalHandler
depends=NetworkEntity

[plugins/plugin]
title=ConsoleTerminal

[plugins/plugin]
title=BasicConsoleCommands
depends=ConsoleTerminal
depends=TcpServer
```

Notes about loading order:

- `ExtraLogging`, `AsioContextThreads` and `ConsoleTerminal` can be loaded anytime (no `depends=` section in their configuration) and we can load them using `base::Promises::All` i.e. in parallel.

Notes about unloading order:

- `ExtraLogging`, `AsioContextThreads` and `ConsoleTerminal` required by some other plugins and their unloading order must be controlled (first you must unload plugin that require them i.e. have `depends=` section).
- `ExtraLogging` and `BasicConsoleCommands` are not required by any pligins and can be unloaded anytime (no `depends=` section from other plugins points to them) and we can unload them using `base::Promises::All` i.e. in parallel.
