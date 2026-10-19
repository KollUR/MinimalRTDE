# Plan: Protocol Client

A single-threaded C++17 program that speaks RTDE on a TCP socket, exchanges the counters in `counter_exchange.urscript` at 250 Hz, and records whether a Missed Package occurred. No Stock Driver, no UR Client, and no other network library. Terms are in `CONTEXT.md`. The reason this client speaks the protocol itself is in `docs/adr/0001-protocol-client-speaks-rtde-itself.md`.

The operator starts `counter_exchange.urscript` on the robot before the program connects. The program opens only the RTDE port. The measured loop contains no primary or dashboard connection.

## 1. Project skeleton

Add a CMake build and one source file under `MinimalRTDE`. C++17. Link only the operating-system socket and pthread libraries. Generate the binary `protocol_client`.

Command line:

- `--host`, default `192.168.178.56`
- `--duration`, default `60` seconds
- `--output`, default `protocol_client.csv`

The exchange rate stays 250 Hz. The URScript reaches 250 Hz by calling `sync()` twice per iteration on a 500 Hz controller, so the rate is not a flag.

## 2. Connect, Nagle, priority

Connect to port 30004. Enable `TCP_NODELAY` on that socket.

Then raise this thread to `SCHED_FIFO` at `sched_get_priority_max`. If the kernel refuses that priority, request the next lower `SCHED_FIFO` priority until one is granted. If none is granted, continue at ordinary scheduling. Print the policy and priority actually in effect. When that is below `SCHED_FIFO` at the maximum, also print that the highest priority was not granted. Either way, continue into the exchange.

## 3. Handshake

Follow the [RTDE guide](https://docs.universal-robots.com/tutorials/communication-protocol-tutorials/rtde-guide.html#protocol), network byte order.

1. Request protocol version 2. Stop if the controller rejects it. Version 2 is the one whose output setup carries a frequency.
2. Set up the output recipe at 250 Hz: `timestamp`, `runtime_state`, `output_int_register_41`, `output_int_register_42`. On a 500 Hz controller, `floor(500 / 250)` is 2, so the stream is 250 Hz.
3. Set up the input recipe: `input_int_register_42`.
4. Stop if any returned type is `NOT_FOUND` or `IN_USE`, or if a type is anything other than `DOUBLE` for `timestamp`, `UINT32` for `runtime_state`, and `INT32` for the three registers.
5. Start the output stream. Stop if the controller refuses.

`runtime_state` value 2 means the program is playing.

## 4. Exchange loop

One thread. No sleep. Each Exchange Cycle:

1. Block until one full output package has arrived. A short read, a text message, or any other package type ends the run.
2. Read `timestamp`, `runtime_state`, the Echoed Client Counter (`output_int_register_41`), and the Script Counter (`output_int_register_42`) from that package.
3. If `runtime_state` is not 2, end the run.
4. Increment the Client Counter. It starts at 0, so the first value sent is 1.
5. Send one input package containing that Client Counter.
6. Keep the row in memory: `robot_timestamp`, `client_counter`, `echo_counter`, `script_counter`, `runtime_state`.

Leave the loop when the duration has elapsed, when `runtime_state` leaves 2, or when a read or send fails. There is no reconnect.

## 5. Write the CSV and the summary

After the loop, pause the RTDE stream if the socket is still open. Then write the rows to the output file. A failed run still writes the cycles it stored.

The summary prints:

- Sample count, the span of `robot_timestamp`, and the resulting rate.
- Every Script Counter step other than +1, split into repeats (step 0) and jumps (step greater than 1). A jump is the output-side Missed Package. A repeat means the same script iteration was sampled twice.
- For pairs whose Script Counter advanced by exactly 1, after the first Echoed Client Counter that belongs to a Client Counter sent in this run: how many echoes were held, and how many advanced by more than 1.
- The inbound Missed Package count: Client Counter values sent in this run that never appear in the echo. Echoes from before the first sent value are the register's previous contents.
- The resting Skew and the largest departure from it. The script is already counting when the client connects, so the resting Skew is a constant and is not expected to be 0 or -1.

## 6. Build and run on WSL2

Build and run on Ubuntu under WSL2. Keep the build tree on the WSL filesystem. State in the program's usage that the URScript must already be playing, and that another RTDE writer must not already own `input_int_register_42`.

## Out of scope

Uploading or stopping the URScript, reconnecting, memory locking, pinning the thread to a CPU, and linking the Stock Driver or the UR Client.
