sash

It's basically tee + a live "tail window" for humans.

You run a command and pipe its output through this tool. It writes the full,
unmodified stream to a file (like tee), but instead of dumping everything to
your terminal, it keeps the terminal as a fixed-height window showing only the
last N lines. As new lines arrive, it redraws the terminal so you're always
looking at "the end of the log" without your scrollback exploding.

Mental model: "I want tee out.log, but the TTY behaves like a dashboard
viewport into the stream rather than an infinite printer."

## Done

### Command execution mode

Implemented. Both stdout and stderr of the child are captured.

    sash [-n lines] [-f] [file ...] -- command [args...]
