# mycord

A multithreaded TCP chat client written in C, using a custom binary wire protocol.

## Features

- Custom packed binary protocol (login, logout, message, system, disconnect message types) with proper network byte order handling
- Concurrent send/receive via POSIX threads and mutex-protected shared state — sending never blocks receiving
- Optional raw-terminal TUI mode (`--tui`) built from scratch with `termios` and ANSI escape codes, plus a standard line-mode fallback
- Non-blocking, `poll()`-based I/O for responsive input handling
- Graceful shutdown on `SIGINT`/`SIGTERM`, server-initiated disconnect, or EOF, with safe socket teardown across threads

## Usage

```
./client [--port PORT] [--ip IP | --domain DOMAIN] [--quiet] [--tui]
```

- `--port` — port to connect to (default: 8080)
- `--ip` — IP address to connect to (default: 127.0.0.1)
- `--domain` — domain name to connect to (resolved via DNS; mutually exclusive with `--ip`)
- `--quiet` — disable alert sounds and @-mention highlighting
- `--tui` — enable the text UI mode

## Build

```
gcc -o client client.c -lpthread
```

## Notes

Requires a compatible mycord server implementing the same binary message protocol (see `Message` struct: type, timestamp, username, message, all in network byte order).
