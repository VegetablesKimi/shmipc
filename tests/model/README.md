# GenMC models

Small C11 models of each channel protocol, checked exhaustively with [GenMC](https://github.com/MPI-SWS/genmc) under two memory models:

- **RC11**, the repaired C/C++11 model, which is what compilers must respect;
- **IMM**, a hardware-level model covering the behaviours of Arm, Power and x86.

| model | mirrors | threads | what is checked |
|---|---|---|---|
| `spsc.c` | `SpscQueue` | 1 producer, 1 consumer, 2 slots, 3 messages | order and values; no race on slots |
| `broadcast.c` | `Broadcast` | 1 writer, 1 reader, 2 records, 3 writes | an accepted copy is untorn and genuine |
| `seqlock.c` | `Seqlock` | 1 writer (2 versions), 1 reader | an accepted snapshot is consistent |
| `mpsc.c` | `Mpsc` | 2 producers × 2 records, 1 consumer, 2-record ring | per-producer order and values; no race on payloads |
| `doorbell.c` | `Doorbell` + `Blocking` | 1 notifier, 1 waiter | no lost wakeup |

Each ordering site is a macro named after its `OrderSite` (`ORD_SPSC_RELEASE`, ...). `run_models.sh` runs the correct model and one variant per site weakened to `memory_order_relaxed`, in three configurations:

- **RC11, plain payload:** a missing edge shows up as a data race, which the C++ standard makes undefined behaviour.
- **RC11, atomic payload:** a missing edge shows up as a wrong value. RC11 forbids load buffering, so this column can pass where the hardware does not.
- **IMM, atomic payload:** the hardware view.

## Running

The `model-check` GitHub workflow builds GenMC (cached) and runs everything on each change to the channels or models; results go to `docs/results/genmc.md`. Locally:

```bash
docker build -t shmipc-genmc -f docker/Dockerfile.genmc docker   # needs ~16 GB RAM for the LLVM build
docker run --rm -v "$PWD":/src shmipc-genmc tests/model/run_models.sh
```

## Fidelity

The models are hand-written mirrors, not the C++ code itself. GenMC verifies C/LLVM, and running it on the template code would mean modelling `std::atomic_ref` and the rest of the library. To keep the two from drifting:

- each model names the `OrderSite` of every ordering it uses;
- the experiment table in CORRECTNESS.md §5 covers the same sites, so a site added to the code without a model shows up as a missing row;
- sizes are deliberately tiny (2 slots, 2–3 messages). Model checking is exhaustive *within* those bounds, and the protocols do not depend on capacity beyond "a slot can be reused".

What the models leave out: variable-length records and padding (covered by the RapidCheck property tests), crash recovery (covered by the fault-injection and chaos tests), and the futex itself (`doorbell.c` models its compare-and-sleep contract).
