# sash

`tee` with a live tail window.

Pipes stdin to output files (like `tee`) while showing only the last N lines
in a fixed terminal region that redraws in place. Your scrollback stays clean;
you always see the tail of the stream.

![demo](demo.gif)

## Installation

### From source (CMake)

```sh
cmake -B build
cmake --build build
sudo cmake --install build
```

### Nix

```sh
# Run directly
nix run github:bflyblue/sash

# Install into a profile
nix profile install github:bflyblue/sash
```

## Usage

**Pipe mode** — read from stdin:

```sh
make 2>&1 | sash -w build.log
```

**Command mode** — run a command and capture its output:

```sh
sash -w build.log make
```

### Options

| Flag | Description |
|------|-------------|
| `-n N` | Window height in lines (default: 10) |
| `-f` | Flush output files after each line |
| `-x` | Use exec instead of shell (no pipes, `&&`, etc.) |
| `-l` | Show line numbers |
| `-c` | Force color on |
| `-C` | Force color off |
| `-w FILE` | Write output to FILE (truncate) |
| `-a FILE` | Append output to FILE |
| `-h` | Show help |

### Examples

```sh
# Watch a long build, save full log
sash -n 20 -w build.log make -j8

# Tail a deployment with line numbers
sash -l -a deploy.log ./deploy.sh

# Pipe mode with multiple output files
cargo test 2>&1 | sash -w test.log -a all-tests.log
```

## How it works

sash opens `/dev/tty` for display and reserves the bottom N rows of the
terminal using a scroll region. Input lines are stored in a ring buffer and
the visible window is redrawn with a single `write()` call to minimise
flicker. When the input stream ends, the scroll region is restored and the
terminal returns to normal.

In command mode, the command is spawned via `sh -c` (or `exec` with `-x`)
and both stdout and stderr are captured through a pipe.

## Requirements

- POSIX system (Linux, macOS, BSDs)
- C11 compiler
- CMake 3.10+
- `getline()`, `sigaction()`, `TIOCGWINSZ`

## License

[BSD 2-Clause](LICENSE)
