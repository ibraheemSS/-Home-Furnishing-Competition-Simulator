# Design Notes

## 1. Process model

The master process forks one process for each member of each team.

For two teams and `N` members per team:

```text
children = 2 * N
```

Each child keeps a simple role:

- Source: chooses random pieces from its furniture pile.
- Middle: passes pieces forward or backward.
- Sink: accepts correct serials and rejects wrong serials.

## 2. IPC model

Each team owns these pipes:

```text
forward[i]   member i     -> member i + 1
backward[i]  member i + 1 -> member i
ack          sink         -> source
```

The master also owns:

```text
event_pipe   children -> master
fifo         master   -> optional OpenGL visualizer
```

## 3. Message types

`FurnitureMsg` is passed between children. It carries:

- team id
- round id
- piece id
- serial number
- direction
- touch count

`EventMsg` is sent from children to the master. It carries:

- accepted progress
- rejected events
- round winner
- child status/error messages
- `EVENT_MOVED` animation events with `from_member`, `to_member`, `direction`, `piece_id`, and `serial_no`

## 4. Round algorithm

For each round:

1. Master sends `SIGUSR1` to all children.
2. Source processes regenerate serial numbers and reset delivered/blocked arrays.
3. The source randomly chooses an available piece.
4. The piece moves forward through the team.
5. The sink checks whether `serial_no == expected_serial`.
6. If correct, the sink keeps it and ACKs the source.
7. If wrong, the sink returns it backward to the source.
8. The source blocks a returned piece until some different piece succeeds.
9. When all pieces are accepted, the sink reports `EVENT_ROUND_WIN`.
10. Master sends `SIGUSR2`, updates the scoreboard, then starts the next round.

## 5. Why this IPC design is good

- Pipes naturally model one-way movement between neighboring members.
- Backward pipes naturally model the return path for rejected furniture.
- ACK pipe avoids shared memory and tells the source exactly when a piece succeeded.
- Signals are used for global control, not for data payloads.
- FIFO decouples the simulation engine from the graphics process.
- The visualizer receives the same structured `EventMsg` objects as the logger, so animation is driven by real pipe movement events rather than fake progress bars.
- `EVENT_MOVED` is emitted after each successful write to a forward or backward pipe, which lets the OpenGL program animate the product symbol over the matching pipe segment.

## 6. Extension ideas

- Allow several pieces to be in flight at the same time.
- Add a CSV statistics file with trip counts and timings.
- Add command-line overrides for config values.
- Add more detailed visual positions for each furniture piece.
- Add per-member speed/tiredness parameters.

## 2026 update: scheduled rounds and tie-breaks

The master now treats `rounds_to_play` as the scheduled number of rounds. After every round, it checks whether one team has an unreachable lead. For example, with `rounds_to_play=4`, a score of `3-0` ends immediately because the other team has only one possible remaining win.

If the scheduled rounds end with an equal score, the master starts additional tie-break rounds until one team leads. It also emits `EVENT_TIE_BREAK` messages so the visualizer/frontend can notify the user.

## 2026 update: visualizer synchronization

The master creates the FIFO used by the visualizer. The visualizer can still run from a separate terminal, but it now waits for the master-created FIFO instead of creating it itself.

The master also sends explicit `EVENT_ROUND_START` messages before each round. The visualizer resets progress bars and queued animations on these events, which fixes the old issue where a full progress bar could remain visible at the beginning of a new round.
