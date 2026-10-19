# Minimal RTDE

A Rig for deciding whether a Missed Package is inherent to an RTDE exchange with a UR controller. It uses the same measurement language as the Zeus Communication Check.

## Language

### The measurement

**Client Counter**:
The integer an RTDE client writes into a general-purpose input integer register, incremented once per Exchange Cycle.
_Avoid_: input counter, RTDE counter, PC counter, sequence number

**Script Counter**:
The integer a URScript program writes into a general-purpose output integer register, incremented once per iteration of its own loop.
_Avoid_: output counter, robot counter, controller counter, sequence number

**Echoed Client Counter**:
The Client Counter value a URScript iteration actually read back out of the input register, written to a second output integer register in that same iteration. It is the only direct evidence of what the controller-side program consumed, as opposed to what the client sent or what the register latched.
_Avoid_: echo register, loopback, readback, mirror

**Skew**:
Client Counter minus Script Counter, evaluated at a single sampled instant. The URScript is already running when the exchange starts, so the resting value is a constant and need not be 0 or -1. A fault is a departure from that resting value.
_Avoid_: jitter, drift, offset, misalignment

**Inbound Skew**:
Client Counter minus Echoed Client Counter: the half of the Skew accumulated on the way into the controller.
_Avoid_: input skew, write latency

**Controller Skew**:
Echoed Client Counter minus Script Counter, measured entirely inside one RTDE packet.
_Avoid_: output skew, robot skew

**Missed Package**:
An input write the controller never latched, or an output package the controller skipped and never resent. A Missed Package can produce a nonzero Skew, but a nonzero Skew does not by itself evidence a Missed Package. On the inbound path it is directly observable: a Client Counter value that never appears in the Echoed Client Counter stream was written but never read.
_Avoid_: dropped packet, package drop, lost frame, packet loss

**Jitter**:
Variation in the interval between successive Exchange Cycles. Reserved strictly for timing variation.
_Avoid_: using "jitter" to mean Skew or Missed Package

**Exchange Cycle**:
One pass of the client loop: read one output package, record the Script Counter and the Echoed Client Counter, and write the next Client Counter. Its nominal period is the reciprocal of the configured exchange rate.
_Avoid_: control cycle, tick, iteration

### The artefacts

**Rig**:
The client program and its companion URScript in this folder.
_Avoid_: minimal sample, harness, test bench

**Protocol Client**:
An RTDE client whose process contains the protocol, with no Stock Driver and no UR Client.
_Avoid_: raw client, from-scratch client, socket client, minimal sample

**Stock Driver**:
The unmodified SDU ur_rtde library. Zeus's driver is a customization of this library.
_Avoid_: official driver, UR client

**UR Client**:
Universal Robots' own RTDE client library. A different codebase from the Stock Driver.
_Avoid_: ur_rtde, SDU driver
