/* The atomic read-modify-writes an Armv6-M core cannot do for itself.
 *
 * **This file has no counterpart in `sh.sysl.pico2`, and the reason is the core rather than the
 * board.** The RP2350's Cortex-M33 has `ldrex`/`strex`, so LLVM lowers an `atomicrmw` to an inline
 * retry loop and nothing outside the compiler is involved. The RP2040's Cortex-M0+ is Armv6-M and
 * has neither instruction, so the same IR becomes a call to `__atomic_fetch_add_4` — a symbol a
 * freestanding link does not have.
 *
 * **It reaches exactly one thing in sysl: `&sync T`.** The atomic retain/release pair is emitted
 * only for a program that holds one, so an ordinary program on this board never references any of
 * this and pays nothing for the file being here.
 *
 * ## Why not just disable interrupts
 *
 * Because **`PRIMASK` is per-core and this chip has two of them.** Masking interrupts is the classic
 * single-core critical section and it is genuinely correct against *this* core's handlers — but core
 * 0 can be halfway through a read-modify-write with interrupts off while core 1 executes the same
 * three instructions on the same word. The update that lands second wins and the other is lost.
 *
 * On a reference count a lost increment is a **premature free**: the object goes while somebody
 * still holds it, and the failure surfaces at whatever touches the memory next, arbitrarily far from
 * the mistake. So masking is necessary and not sufficient, and what this does is **both** — take one
 * of the chip's hardware spinlocks to keep the other core out, and mask interrupts to keep this
 * core's own handlers out. Either alone is a deadlock or a lost update.
 *
 * ## Why this includes no SDK header
 *
 * The first version of this file included `pico/sync.h` and called `spin_lock_blocking`, which is
 * what the SDK's own `pico_atomic` does. **It made every consumer of this package pass the SDK's
 * include paths to `sysl build-c`, even a blink that never touches `&sync`** — measured, and it fails
 * at the first build with `'pico/sync.h' file not found`. A package that cannot be compiled without
 * its consumer knowing where the SDK keeps its headers is a bad package.
 *
 * Calling the SDK's function without its header is not available either: `spin_lock_blocking` is
 * `__force_inline static`, so there is no symbol to declare and link against.
 *
 * What is left is the hardware, which is four lines and fully specified. A SIO spinlock is a word
 * that **reads non-zero if the read acquired it and zero if somebody else holds it**, and is released
 * by writing any value at all. That is the whole protocol.
 *
 * ## Why the SDK's own `pico_atomic` is not enough
 *
 * It was the first thing checked. It defines the **generic** size-taking forms — `__atomic_load`,
 * `__atomic_store`, `__atomic_exchange`, `__atomic_compare_exchange`, `__atomic_is_lock_free`,
 * `__atomic_test_and_set` — and no sized variant and no fetch-add at all. What LLVM emits for a sysl
 * retain is `__atomic_fetch_add_4`, which is neither, so the link fails with `pico_atomic` present.
 * The sized forms below are the missing half rather than a replacement, and taking the same lock is
 * what keeps the two halves from being inside the critical section at once.
 *
 * ## Ordering
 *
 * The `model` arguments are ignored, which is sound here and would not be everywhere. Two
 * single-issue in-order cores behind a shared bus, with the spinlock acquire and release themselves
 * ordering the accesses either side of them, leaves nothing for a memory model to describe.
 */

#include <stdbool.h>
#include <stdint.h>

/* `SIO_BASE` is 0xd0000000 and `SIO_SPINLOCK0_OFFSET` is 0x100, from the RP2040 datasheet's memory
 * map — the same two numbers `sh.sysl.rp2040` carries as `sio.BASE` and `sio.SPINLOCK0_OFFSET`. The
 * 32 locks are consecutive words from there.
 *
 * **The id matches the SDK's `PICO_SPINLOCK_ID_ATOMIC`, which is 13 on this part** (it is 11 on the
 * RP2350, which is one more reason this file is not shared with that package). Matching it is what
 * makes this and `pico_atomic` mutually exclusive rather than two locks over one word. It is
 * overridable for a project that has renumbered its locks, which the SDK allows.
 */
#ifndef SYSL_PICO_SPINLOCK_ID
#define SYSL_PICO_SPINLOCK_ID 13
#endif

#define SPINLOCK \
    (*(volatile uint32_t *)(0xd0000000u + 0x100u + 4u * (uint32_t)SYSL_PICO_SPINLOCK_ID))

/* Mask interrupts, then take the lock. In that order: taking it first would leave a window in which
 * an interrupt on this core could arrive while the lock is held and try to take it again, which on a
 * spinlock is not a delay but a deadlock — the handler would spin forever waiting for a lock only
 * the code it interrupted can release.
 */
static inline uint32_t enter(void) {
    uint32_t primask;

    __asm__ volatile("mrs %0, primask\n\tcpsid i" : "=r"(primask) :: "memory");

    while (SPINLOCK == 0u) { }

    return primask;
}

/* Release, then restore. The write releases whatever its value is. `msr primask` puts back what was
 * there rather than unconditionally enabling, so a caller that already had interrupts masked keeps
 * them masked and nested critical sections nest.
 */
static inline void leave(uint32_t primask) {
    SPINLOCK = 0u;

    __asm__ volatile("msr primask, %0" :: "r"(primask) : "memory");
}

/* The two LLVM emits for a retain and a release. Both answer the value *before* the operation, which
 * is what the names promise and what sysl's release relies on: it compares the answer against 1 to
 * decide whether it took the last reference.
 */
uint32_t __atomic_fetch_add_4(volatile void *mem, uint32_t delta, int model) {
    (void)model;

    volatile uint32_t *p = (volatile uint32_t *)mem;
    uint32_t save = enter();
    uint32_t was = *p;

    *p = was + delta;
    leave(save);

    return was;
}

uint32_t __atomic_fetch_sub_4(volatile void *mem, uint32_t delta, int model) {
    (void)model;

    volatile uint32_t *p = (volatile uint32_t *)mem;
    uint32_t save = enter();
    uint32_t was = *p;

    *p = was - delta;
    leave(save);

    return was;
}

/* The sized load and store, which an `atomic load`/`atomic store` of a word lowers to. */
uint32_t __atomic_load_4(const volatile void *mem, int model) {
    (void)model;

    const volatile uint32_t *p = (const volatile uint32_t *)mem;
    uint32_t save = enter();
    uint32_t v = *p;

    leave(save);

    return v;
}

void __atomic_store_4(volatile void *mem, uint32_t v, int model) {
    (void)model;

    volatile uint32_t *p = (volatile uint32_t *)mem;
    uint32_t save = enter();

    *p = v;
    leave(save);
}

uint32_t __atomic_exchange_4(volatile void *mem, uint32_t v, int model) {
    (void)model;

    volatile uint32_t *p = (volatile uint32_t *)mem;
    uint32_t save = enter();
    uint32_t was = *p;

    *p = v;
    leave(save);

    return was;
}

/* Compare-exchange, which a weak reference's upgrade needs: it has to raise a count only if the
 * count is not already zero, and that is a compare-and-swap rather than an add.
 *
 * **It writes the value it *found* back through `expected` when it fails**, which is what lets a
 * caller retry without re-reading — and forgetting it is how a CAS loop turns into a spin that never
 * converges.
 */
bool __atomic_compare_exchange_4(volatile void *mem, void *expected, uint32_t desired,
                                 bool weak, int success, int failure) {
    (void)weak; (void)success; (void)failure;

    volatile uint32_t *p = (volatile uint32_t *)mem;
    uint32_t *want = (uint32_t *)expected;
    uint32_t save = enter();
    uint32_t was = *p;
    bool ok = was == *want;

    if (ok) *p = desired;
    else    *want = was;

    leave(save);

    return ok;
}
