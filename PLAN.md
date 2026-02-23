sash

It’s basically tee + a live “tail window” for humans.

You run a command and pipe its output through this tool. It writes the full,
unmodified stream to a file (like tee), but instead of dumping everything to
your terminal, it keeps the terminal as a fixed-height window showing only the
last N lines. As new lines arrive, it redraws the terminal so you’re always
looking at “the end of the log” without your scrollback exploding.

So you get:

Complete logs preserved on disk for later debugging/grep.

Real-time visibility while it runs, but with bounded noise — just the most
recent N lines.

A terminal UI that behaves like a live tail without needing a second pane or
tail -f follow-up.

Mental model: “I want tee out.log, but the TTY behaves like a dashboard
viewport into the stream rather than an infinite printer.”

