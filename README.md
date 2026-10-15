# pico

The **original** Raspberry Pi Pico W for sysl — the board's own entry points, so that a program can
be written in sysl and hosted by the C SDK.

*An independent sysl package for the Raspberry Pi Pico. It is not affiliated with or endorsed by Raspberry Pi Ltd.*

```sysl
import sh.sysl.pico.*
import sysl.time.DurationUnits

@export("main")
run() -> int =
    if !init()
        print("the wireless chip did not start")
        return 1

    wait_for_terminal()
    print("hello from sysl, on an RP2040")

    loop
        led(true)
        sleep(120.ms)
        led(false)
        sleep(880.ms)
```

That is a whole program. There is no C in it, and no C in the project that builds it: sysl exports
`main` and the SDK's `crt0` calls it.

```hocon
dependencies {
  pico { git = "github.com/sysl-lang/pico", version = "0.1.0" }
}
```

Build it for **`thumbv6m-freestanding`**, which is Armv6-M. That is not either of the targets a Pico
2 uses, and getting it wrong is not a link error — an Armv8-M image on an M0+ faults at whatever
executes first.

```
sysl build-c --target thumbv6m-freestanding .
```

## It is `pico2` for the older board, with one real difference

`sh.sysl.pico2` is the same package for the Pico 2, and the two are near-identical on purpose: what
they declare is the **SDK's** surface rather than the chip's, pico-sdk covers both parts with one
API, and the CYW43 wireless chip is the same silicon on both. The Wi-Fi half — joining a network,
scanning, DHCP, DNS, http and https — is the same code and the same four C files.

**The difference is `atomic.c`, and it is about the core rather than the board.**

The RP2040's Cortex-M0+ is Armv6-M, which has no `ldrex`/`strex`. LLVM therefore cannot lower an
`atomicrmw` to an inline retry loop the way it does on the Pico 2's Cortex-M33, and emits a call to
`__atomic_fetch_add_4` instead — a symbol a freestanding link does not have. This package supplies
that call and its relatives.

**It reaches exactly one thing in sysl: `&sync T`.** The atomic retain/release pair is emitted only
for a program holding one, so an ordinary program on this board never references any of it and pays
nothing.

**Disabling interrupts would be the obvious implementation and is wrong.** `PRIMASK` is per-core and
this chip has two cores, so masking is correct against this core's handlers and buys nothing at all
against the other one. On a reference count a lost increment is a premature free — the object goes
while somebody still holds it — and it surfaces arbitrarily far from the mistake. So `atomic.c` takes
one of the chip's hardware **spinlocks** instead, through the SDK's `spin_lock_blocking`, which takes
the lock *and* masks interrupts because both are needed.

The SDK's own `pico_atomic` was the first thing tried and is not enough: it defines the generic
size-taking forms (`__atomic_load`, `__atomic_compare_exchange`, …) and no sized variant and no
fetch-add at all, which is not what LLVM emits. The four here are built on its
`__atomic_compare_exchange` and take the same `PICO_SPINLOCK_ID_ATOMIC` lock, so the two cannot be
inside the critical section at once.

## The register map is a different package

`sh.sysl.rp2040` is the chip's registers — base addresses, offsets, field masks — generated from
Raspberry Pi's SVD. This package is the *SDK*, which is a different thing: a program reaching a
peripheral the SDK does not wrap wants that one, and a program that just wants `sleep` and the LED
wants this one. They compose.

**Do not reach for `sh.sysl.rp2350`.** The two chips share a vendor and an APB base and almost no
addresses; a driver written against the wrong map compiles and addresses the wrong silicon.

## What is not here

**Nothing is testable off the target.** There are no `@test` functions, because there is nothing a
host could run: every function ends in a call to a board. The one exception is that `atomic.c`'s
logic is ordinary C and could be exercised on a host — it is not, because the thing worth testing
about it is the cross-core exclusion, which needs the two cores.

**A plain Pico, without the W, is mostly served by this package and not entirely.** `init` starts the
wireless chip and answers whether it came up; the LED constants are the CYW43's pins. On a
non-wireless board the LED is GPIO 25, an ordinary RP2040 pin, and `sh.sysl.rp2040` is what reaches
it.

## Licence

ISC.
