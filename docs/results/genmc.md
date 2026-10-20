# GenMC model checking

GenMC v0.19.0, host x86_64.
`ok (N)`: no error in any of the N executions (exhaustive). **race**: a data race on a plain payload
(undefined behaviour in C++). **assert**: an execution in which the protocol returns a wrong value.
`timeout`: not finished within 120 s (no verdict).

| model | variant | RC11, plain payload | RC11, atomic payload | IMM, atomic payload |
|---|---|---|---|---|
| spsc | correct | ok (9) | ok (9) | ok (9) |
| spsc | spsc.publish | **race** | **assert** | **assert** |
| spsc | spsc.consume | **race** | **assert** | **assert** |
| spsc | spsc.release | **race** | ok (9) | **assert** |
| spsc | spsc.reclaim | **race** | ok (9) | ok (9) |
| broadcast | correct | ok (45) | ok (45) | ok (45) |
| broadcast | bcast.intent | **assert** | **assert** | **assert** |
| broadcast | bcast.publish | **assert** | **assert** | **assert** |
| broadcast | bcast.observe | **assert** | **assert** | **assert** |
| broadcast | bcast.validate | **assert** | **assert** | **assert** |
| seqlock | correct | ok (196) | ok (196) | ok (196) |
| seqlock | seqlock.begin | **assert** | **assert** | **assert** |
| seqlock | seqlock.end | **assert** | **assert** | **assert** |
| seqlock | seqlock.read | **assert** | **assert** | **assert** |
| seqlock | seqlock.validate | **assert** | **assert** | **assert** |
| mpsc | correct | ok (42) | ok (42) | ok (42) |
| mpsc | mpsc.commit | **race** | **assert** | **assert** |
| mpsc | mpsc.consume | **race** | **assert** | **assert** |
| mpsc | mpsc.release | **race** | **assert** | **assert** |
| mpsc | mpsc.reclaim | **race** | **assert** | **assert** |
| mpsc | mpsc.headcache | **race** | **assert** | **assert** |
| doorbell | correct | ok (4) | ok (4) | ok (4) |
| doorbell | doorbell.notify | **assert** | **assert** | **assert** |
| doorbell | doorbell.wait | **assert** | **assert** | **assert** |
