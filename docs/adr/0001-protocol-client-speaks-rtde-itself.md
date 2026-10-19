# The Protocol Client speaks RTDE itself

The obvious client for this Rig is the UR Client described in Universal Robots' documentation, or the Stock Driver already used by the Zeus Rig. Both can produce or hide a Missed Package. The UR Client may replace a pending Client Counter before it is sent, and a background read can keep only the latest output package. The Stock Driver is already what the other sample measures. This Rig's client therefore implements the RTDE handshake and the data packages itself, so a Missed Package that remains cannot be attributed to either library.

## Considered options

- UR Client, with the loop paced by a blocking read of the output stream.
- UR Client, paced by the client's own 250 Hz clock.
- Protocol Client on a TCP socket. Chosen.

## Consequences

- A bug in the Protocol Client can look like a Missed Package.
- The exchange loop is one thread and sends one input package per received output package, so a later Client Counter cannot replace an earlier one inside the client.
- The thread asks for the maximum `SCHED_FIFO` priority and continues at whatever priority the kernel grants. The terminal says when the highest priority was not granted.
