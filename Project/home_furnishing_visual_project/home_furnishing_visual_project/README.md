# Home Furnishing Competition

Multi-processing Linux project for simulating two teams competing to furnish two houses.

The implementation uses:

- `fork()` to create one process per team member.
- Anonymous pipes for member-to-member furniture transfer.
- A backward pipe path for rejected furniture.
- An ACK pipe from sink to source for successful furniture delivery.
- Signals to start rounds, stop rounds, and terminate children.
- A named FIFO (Linux named pipe) for optional OpenGL visualization; the master creates the FIFO and launches the visualizer as its own child process.
- Optional OpenMP for parallel initialization/reset loops.
- A text config file to avoid hard-coded parameters.

## Build

```bash
make
```

The executable is generated at:

```bash
bin/furnish
```

## Run

```bash
./bin/furnish config/sample.conf
```

Fast smoke test:

```bash
make fast
```

## Optional OpenMP build

```bash
make openmp
./bin/furnish config/large_openmp.conf
```

Compare with:

```bash
make clean && make
./bin/furnish config/large_openmp.conf
```

For small furniture counts, OpenMP can be slower because thread overhead is larger than the loop work. For larger counts, it may help the serial array and status-array reset steps.

## Optional OpenGL visualizer

The visualizer shows the actual simulation flow and now resets progress exactly on master round-start events:

- Each team member is drawn as a box with a simple man icon inside it.
- Lines between neighboring boxes represent the forward and backward pipes.
- The blue upper pipe carries products from source to sink.
- The red lower pipe carries rejected products back from sink to source.
- A moving carton symbol shows the current product, including its serial number and piece id.
- Green flashes mean an accepted product; red flashes mean a rejected product.
- The source pile and house stack update as the round progresses.

Install OpenGL/freeglut development packages if needed.

Ubuntu/Debian example:

```bash
sudo apt-get install freeglut3-dev mesa-common-dev
```

Build the simulator and visualizer:

```bash
make graphics
```

Then run only the simulator. The master creates the FIFO, forks the visualizer child, and sends visual events through that FIFO:

```bash
./bin/furnish config/graphics.conf
```

Keyboard shortcuts in the visualizer:

```text
q or Esc    quit
c           clear queued animation moves
```

## Configuration

The main config values are:

```text
members_per_team      number of processes in each team
furniture_count       number of furniture pieces per house
rounds_to_play        scheduled rounds before tie-breaker rounds
wins_to_end           legacy setting used only to derive rounds_to_play if rounds_to_play is omitted
min_delay_ms          minimum random member pause
max_delay_ms          maximum random member pause
fatigue_step_ms       extra delay added as members get tired
fatigue_every_moves   add fatigue after this many moves
serial_mode           random or file
graphics_enabled      0 or 1
graphics_fifo         FIFO path created by the master for visualizer communication
visualizer_path       executable launched by the master when graphics are enabled
log_file              output log path
verbose               1 logs rejections, 0 logs mainly progress/winners
```

## Architecture

For `members_per_team = N`, the program creates:

```text
2 * N child processes
```

Each team has:

```text
member 0       source
member N - 1   sink
others         middle members
```

Furniture moves through forward pipes:

```text
source -> middle -> ... -> sink
```

If the sink receives the expected serial number, it accepts the piece and sends an ACK back to the source.

If the sink receives a wrong serial number, it sends the piece back through backward pipes:

```text
sink -> middle -> ... -> source
```

The source blocks a failed piece until another piece is accepted successfully.

## Signals

```text
SIGUSR1  start/reset a round
SIGUSR2  stop current round
SIGTERM  terminate children
SIGINT   master shutdown from Ctrl-C
```

## Important files

```text
include/config.h       config structure and loading API
include/ipc.h          pipe message structures and IPC helpers
include/team.h         child process API
include/master.h       master process API
src/master.c           creates children and controls rounds
src/team_member.c      source/middle/sink behavior
src/furniture.c        serial generation and piece selection
src/visualizer.c       optional OpenGL FIFO visualizer
```

## Debugging

Build with no optimization and full debug info:

```bash
make debug
```

Run with gdb:

```bash
gdb --args ./bin/furnish config/fast.conf
```

## Notes on performance

The required rule says the source chooses random furniture while the sink only accepts serials in order. Therefore many pieces can be rejected and returned. Expected total trips can grow roughly quadratically with the number of pieces. For large tests such as 1000 pieces, use very small delays and `verbose=0`.

## Round rules in this version

The competition uses `rounds_to_play` as the number of scheduled rounds.

- If one team becomes impossible to catch before all scheduled rounds are played, the competition ends early.
  Example: with `rounds_to_play=4`, a score of `3-0` ends the competition because the other team cannot catch up.
- If the scheduled rounds finish with a tie, the master starts extra tie-break rounds until one team leads.
  Example: with `rounds_to_play=4`, a score of `2-2` starts a new judgment round.
- The master sends round-start and tie-break events to the visualizer so the frontend clears the progress bars before each new round.
- The visualizer ignores stale movement/accept/reject events after a round winner is detected, so products do not keep moving after a team has already completed its furniture.

## Serial number modes

Serial assignment is controlled from the config file:

```text
serial_mode=random
```

or:

```text
serial_mode=file
serial_file=config/serials_example.txt
```

When `serial_mode=file`, the serial file must contain exactly `furniture_count` unique integers from `1` to `furniture_count`.
