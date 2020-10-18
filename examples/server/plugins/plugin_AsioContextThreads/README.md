# About

Creates asio context and threads that call `::boost::asio::io_context::run()`. Asio context will be stored globally, so other plugins can access created asio context.
