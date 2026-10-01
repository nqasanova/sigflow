# sigflow

A small schema driven messaging framework for signaling protocols, written in C++20.

Messages are described once in a `.msg` schema file. A code generator (`msgc`, written in Python) turns the schema into C++ and Python code, and a C++ runtime library moves those messages between peers over UDP or any byte stream. To have something real to run it on, the repo includes a simulated base station and a simulated UE that go through attach, measurement reporting, handover and detach.

```
schemas/*.msg ──► msgc ──┬──► C++ structs + encode/decode ──► sigflow-enb, sigflow-ue
                         └──► Python dataclasses ──────────► system tests
```

## Why

Most of my projects so far have been in Python, so I wanted something that would make me learn modern C++ properly: ownership, templates and concepts, error handling without exceptions, sockets, and a CMake setup that does more than compile one file.

The topic came from reading about how mobile network protocols work. Message formats are defined once in a specification, and code for different languages and components is generated from that definition instead of being written by hand. This repo is a much smaller version of that idea, with a message format I designed myself and a toy version of the LTE attach and handover procedures on top.

## What it does

- **Schema language and compiler.** Enums, structs, messages, `optional` and `repeated` fields, nested structs. Errors point to the exact line and column. All problems in a file are reported at once.
- **Two code generators from one schema.** C++20 and Python. A test checks that both produce byte identical frames for the same message.
- **Forward compatible wire format.** Tag Length Value encoding, so a decoder skips fields it does not know. An older peer keeps working when a newer one adds a field.
- **No exceptions on the decode path.** Everything returns `Result<T>` or `Status`. Malformed input from the network is an expected situation, not an exceptional one.
- **Transports.** UDP, any stream file descriptor (TCP, pipes, serial) with frame resynchronisation after corrupted data, and an in-memory loopback for tests.
- **Typed dispatch.** `endpoint.on<AttachRequest>(handler)` registers a handler, decoding happens before the handler is called.
- **Build and CI.** CMake presets, generated code integrated into the build, ccache, strict warnings, AddressSanitizer and UBSan builds, GitHub Actions and GitLab CI pipelines, and a Docker Compose setup that runs the simulators in separate containers.

## Example

A schema (`schemas/session.msg`, shortened):

```
namespace ran.session;

enum AttachResult : u8 {
    ACCEPTED = 0;
    REJECTED_UNKNOWN_NETWORK = 1;
    REJECTED_CONGESTION = 2;
}

message AttachResponse = 0x0102 {
    1: AttachResult result;
    2: optional u32 ue_id;
    3: optional u32 serving_cell_id;
    4: optional u16 retry_after_ms;
}
```

Using the generated code in C++:

```cpp
sigflow::Endpoint endpoint(transport);

endpoint.on<ran::session::AttachRequest>([&](const ran::session::AttachRequest& request) {
    ran::session::AttachResponse response;
    response.result = ran::session::AttachResult::kAccepted;
    response.ue_id = 1;
    if (auto status = endpoint.send(response); !status) {
        std::cerr << status.error().message() << "\n";
    }
});

while (running) endpoint.poll(200ms);
```

And the same message from Python:

```python
from sigflow import wire
import session_messages as session

frame = wire.encode_message(session.AttachResponse(result=session.AttachResult.ACCEPTED, ue_id=1), sequence=0)
```

## Demo

```
$ ./build/debug/apps/sigflow-enb &
[  0.000] enb: ready on port 9500 cell 101

$ ./build/debug/apps/sigflow-ue
[  0.000] ue: attach attempt 1 imsi=262011234567890
[  0.002] ue: attached as ue_id=1 on cell 101
[  0.002] ue: at 0 m: serving -70 dBm
[  0.101] ue: at 120 m: serving -75.8 dBm
[  0.201] ue: at 240 m: serving -82.4 dBm
[  0.300] ue: at 360 m: serving -87.2 dBm
[  0.400] ue: at 480 m: serving -90.7 dBm
[  0.499] ue: at 600 m: serving -93.5 dBm
[  0.500] ue: handover to cell 102 (margin 5 dB)
[  0.500] ue: at 720 m: serving -84.2 dBm
[  0.600] ue: at 840 m: serving -78.3 dBm
[  0.699] ue: detached
[  0.699] ue: done, 1 handover(s), now on cell 102
```

The UE drives along a road from cell 101 towards cell 102. Signal strength follows a log distance path loss model. Once the neighbour cell is at least 3 dB stronger than the serving cell, the base station sends a handover command (a simplified version of the LTE A3 event).

## Building

Requirements: a C++20 compiler (GCC 11+ or Clang 15+), CMake 3.25+, Ninja and Python 3.9+. GoogleTest is downloaded automatically if it is not installed. ccache is used when available.

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

Other presets: `release`, `asan` (AddressSanitizer + UBSan) and `ci` (warnings are errors).

Running the simulators in two containers on a virtual network:

```sh
docker compose up --build --abort-on-container-exit --exit-code-from ue
```

## Wire format

Every message travels in a frame. All integers are big endian.

```
offset  size  field
0       2     magic 0x5346 ("SF")
2       1     protocol version (1)
3       1     flags (reserved)
4       2     message id
6       2     sequence number
8       4     payload length
12      N     payload
12+N    2     CRC-16/CCITT-FALSE over header and payload
```

The payload is a sequence of fields:

```
tag (u8) | length (u16) | value
```

Integers have a fixed width per type. Signed values use two's complement. Strings and byte arrays are raw bytes. Nested structs are encoded recursively and stored as the value of their field. Repeated fields simply appear more than once.

## Project layout

```
include/sigflow/     public headers of the runtime library
src/                 runtime implementation (framing, TLV, transports, endpoint)
tools/msgc/          the schema compiler: lexer, parser, validator, C++ and Python backends
python/sigflow/      Python runtime used by the generated Python code
schemas/             message definitions for the simulators
apps/                base station and UE simulators
tests/unit/          GoogleTest unit tests for the C++ code
tests/system/        tests that start the real binaries and talk to them over UDP
cmake/               warning flags and the sigflow_generate_messages() helper
```

## Design notes

Some decisions that are not obvious from the code alone:

- **Result instead of exceptions.** A truncated datagram or a wrong field length is normal on a network. `Result<T>` makes every caller deal with it explicitly. A hand written type is used because `std::expected` needs C++23. Exceptions are still used for real programming errors, like registering two handlers for the same message.
- **Fixed width integers instead of varints.** Varints would save a few bytes, but fixed widths keep the decoder simple and make hex dumps readable by eye. Signal levels are sent as fixed point integers (tenths of a dB) rather than floats, so both sides round the same way.
- **The decoder is strict about length.** A `u16` field that arrives with 4 bytes is rejected, not truncated. A length mismatch almost always means the two peers use different versions of the schema, and that should be visible.
- **Attach is idempotent.** UDP can lose the response, so the UE retransmits. The base station recognises a repeated IMSI and returns the same `ue_id` instead of creating a second context.
- **Generated code stays simple.** The C++ generator emits `writer.put(tag, field)` for every field and lets templates pick the encoding at compile time. Adding a new field type means changing the runtime, not the generator.
- **Incremental builds.** msgc only rewrites files whose content changed and CMake tracks a stamp file. Editing a comment in a schema regenerates the code but does not recompile anything.

## Tests

- 52 C++ unit tests, including a CRC check against the standard check value, every single bit flip in a frame being detected, stream reassembly with random chunk sizes, and a small fuzz loop that feeds random and mutated input into every decoder
- 35 Python tests for the schema compiler (parser, validator, both backends, CLI)
- 7 system tests that start `sigflow-enb` and talk to it from the C++ UE and from a Python UE built on the generated Python code
- a golden frame shared between the C++ and Python tests, so both generators are pinned to the same bytes

CI runs everything with GCC and Clang, and once more under AddressSanitizer and UBSan.

## Limitations and ideas

- The UDP server replies to the most recent sender. Enough for the simulators, but a real base station would keep per-peer state above the transport.
- There is a stream transport, but no TCP listen/accept helper yet.
- Fuzzing is a simple random loop. A libFuzzer target per decoder would find more.
- More backends. The backend interface is two functions, so another target language (for example Rust) would be a single new module in `tools/msgc/msgc/backends/`.

## License

MIT
