# actors/ — AIPL actor binaries (.avm)

Compiled AIPL actor modules in the `.avm` bytecode format (magic `AVM1`).
The same format runs on this Xinu kernel's dynamic actor VM and on the
host VM (https://github.com/yaskodama/aice-avm).

Load one onto a running Xinu node over HTTP (it spawns + runs without a
kernel rebuild; answer the on-screen "accept actor?" dialog with Yes, or
append `?ask=0`):

    curl --data-binary @actors/DiningPhilosophers.avm http://<pi-ip>:8080/actor/loadvm

| file | what it does |
|------|--------------|
| PingPong.avm            | two actors bounce a counter (print output) |
| Rotate4Lines.avm        | 4 line segments (one per actor) rotate about a square's corners |
| Rotate4LinesLoop.avm    | same, endless |
| DiningPhilosophers.avm  | 5 Philosopher + 5 Fork actors; held forks drawn as arrows to the holder |

Rebuild from source with the AIPL->.avm compiler:

    abcl2c --avm --no-typecheck <name>.abcl -o actors/<name>.avm
