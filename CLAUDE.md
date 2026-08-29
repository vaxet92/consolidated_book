# CLAUDE.md

Project instructions for Claude Code. Read this before doing anything.

---

## 1. About this project

This is a **take-home assignment for a Senior C++ Engineer position.**

It aggregates BTCUSDT spot market data from three exchanges (Binance, OKX, Bybit) into one consolidated order book, and publishes derived views over gRPC to three client services.

**The most important thing to understand about this project:** there will be a debrief call where the interviewer asks *why* each decision was made. Working code that Anton cannot explain is worse than simpler code that he can defend completely.

So the goal is not "finish the code fast". The goal is:

1. Anton understands every mechanism in the codebase.
2. Anton can justify every trade-off out loud, including the options that were rejected.
3. The code is correct, tested and documented.

Speed is the least important of these.

---

## 2. How to work — the core rules

### Rule 1: Explain before you write

Before making any substantive code or design change, explain:

- What you are going to change.
- Why you recommend this approach.
- Alternative — mention the main alternative when there is a meaningful trade-off.
- Debrief — what Anton needs to understand and be able to explain in the interview.

Keep the explanation short and concrete. Use small examples when they help.

Then stop and wait for confirmation.

Read-only operations do not require confirmation. This includes searching, inspecting files, reading code, checking build errors, running non-mutating commands, and other analysis.

Formatting-only changes do not require confirmation unless they modify behavior.

### Rule 2: Never make changes without confirmation

Do not edit or create files until Anton explicitly says yes, go, or gives an equivalent approval.

This applies to:

- source code;
- tests;
- CMake/build configuration;
- documentation;
- configuration files;
- scripts.

One confirmed step at a time.

Do not batch multiple independent changes because they appear related. If several changes are needed, propose the sequence first.

After completing the approved step, stop and wait for the next confirmation.

### Rule 3: Small, understandable steps

Prefer one of:

- one file;
- one function;
- one class/component;
- one clear design decision.

If a change would produce more than about 100 lines of new code, split it into smaller steps and propose the breakdown first.

If Anton asks for something large, such as "implement the whole provider", do not immediately write everything. Break the work into logical steps and explain the order.

The goal is not minimum number of steps. The goal is that Anton understands every important decision.

### Rule 4: Highlight important concepts

Not every implementation detail deserves equal attention.

Use:

> **KEY:** the seqlock reader must re-check the counter after copying. Checking only before the copy can allow the reader to observe data that was modified during the read.

Use `**KEY:**` only for:

- correctness-critical behavior;
- subtle concurrency or memory-ordering issues;
- performance trade-offs;
- market-data sequencing/consistency rules;
- design decisions likely to be discussed during the interview.

Do not mark routine code as KEY.

### Rule 5: Teach the mechanism, not just the syntax

When introducing a new mechanism, explain:

1. What problem it solves.
2. How it works.
3. Why this implementation was chosen.
4. What alternatives exist.
5. What can go wrong.

Prefer concrete examples with small numbers over abstract explanations.

For example, when explaining sequence validation, show:

```
last_seq = 100
incoming prev_seq = 100 → valid
incoming prev_seq = 105 → gap → resync
```

Do not assume that knowing the C++ syntax means Anton understands the mechanism.

### Rule 6: Check understanding

After completing a meaningful piece of work, include a short section titled **"You should now be able to explain"**. Each item is a question the interviewer might ask.

Example:

> **You should now be able to explain:**
> - Why the consolidator owns all three books instead of each thread owning its own
> - What happens to the queue when the consolidator is slower than the parsers
> - Why an SPSC queue is enough here, and when you would need MPMC

Keep this to three or four questions.

If Anton cannot explain one of them, explain that mechanism again before moving to the next implementation step.

### Rule 7: Measure before optimizing

For performance-related changes:

- Explain the expected bottleneck first.
- Distinguish measured results from estimates.
- Do not claim a performance improvement without measurement.
- Prefer a simple implementation until profiling or benchmarking shows that optimization is justified.
- After optimization, compare the before/after result.

> **KEY:** In this project, "faster" is not a sufficient reason to add complexity. The optimization must have a measurable benefit and preserve correctness.

---

## 3. Language

Anton's English is C1. Use simple, common words and short sentences.

- Avoid rare or academic words.
- Prefer "skip old updates" over "conflate", but teach the real term once, because the interviewer will use it.
- When a technical term is unavoidable, define it the first time in one plain sentence.
- Explain with concrete numbers and small examples, not abstract descriptions.

---

## 3.1 C++ style

Follow the Google C++ Style Guide unless the project explicitly defines a different convention.

### Formatting

- Use clang-format for all C++ source files.
- Use the Google-based clang-format style.
- Do not manually format code that clang-format would change.
- Prefer C++20 where supported by the project.
- Use const and constexpr when they express immutability or compile-time values.
- Prefer std::string_view for non-owning string parameters.
- Prefer std::span for non-owning contiguous ranges.
- Prefer RAII and automatic resource management.
- Avoid raw owning pointers.
- Use std::unique_ptr for exclusive ownership and std::shared_ptr only when shared ownership is actually required.
- Prefer references or pointers for non-owning access.
- Prefer enum class over unscoped enum.
- Use [[nodiscard]] where ignoring a return value could hide an error.
- Prefer std::optional for an explicitly optional value rather than sentinel values.
- Use std::expected for recoverable errors when it improves the API and is consistent with the existing project design.

### Naming

Use these conventions consistently:

| Element | Convention | Example |
|---|---|---|
| Namespace | snake_case | `market_data` |
| Class / struct | PascalCase | `VenueBook` |
| Enum class | PascalCase | `BookState` |
| Enum value | kPascalCase | `kLive` |
| Function / method | PascalCase | `ApplyUpdate()` |
| Variable | snake_case | `last_seq` |
| Parameter | snake_case | `provider_config` |
| Data member | snake_case_ | `last_seq_` |
| Constant | kPascalCase | `kMaxDepth` |
| Static data member | kPascalCase | `kDefaultPort` |
| Template parameter | PascalCase | `typename ValueType` |
| Macro | UPPER_SNAKE_CASE | `CHECK_OK()` |

Use descriptive names. Avoid abbreviations unless they are standard domain terms such as BBO, LOB, WS, REST, TCP, or seq.

Prefer:

```cpp
VenueBook venue_book;
BookUpdate book_update;
const auto& update = ...;
```

over:

```cpp
VenueBook vb;
BookUpdate bu;
auto& u = ...;
```

### Headers

- Use #pragma once.
- Include the corresponding header first in the .cpp file.
- Include project headers after standard/library headers.
- Do not rely on transitive includes.
- Forward declare types where appropriate to reduce dependencies.

### Classes

- Keep classes small and focused.
- Prefer composition over inheritance.
- Use inheritance when there is a real polymorphic relationship.
- Make single-argument constructors explicit.
- Mark overriding virtual functions with override.
- Use final only when preventing further derivation is intentional.
- Keep data members private unless a simple aggregate is more appropriate.
- Prefer structs for passive data/DTO types.
- Prefer classes when invariants or behavior need to be enforced.

### Ownership

Make ownership explicit.

```cpp
std::unique_ptr<VenueBook> book;
```

means ownership.

```cpp
VenueBook& book;
```

means non-owning access.

Avoid APIs such as:

```cpp
void Process(VenueBook* book);
```

when ownership semantics are unclear.

### Performance

This is an HFT project, but do not optimize based on assumptions.

- Keep hot-path code simple and measurable.
- Avoid unnecessary allocations and copies.
- Prefer const&, std::string_view, and std::span where appropriate.
- Be aware of allocation, locking, virtual dispatch, cache behavior, and contention.
- Do not introduce custom allocators, lock-free structures, SIMD, or other complexity without a measured reason.
- Every optimization must have a benchmark or profiling result.
- Never trade correctness for an unmeasured optimization.
- Clearly distinguish measured numbers from estimates.

### Exceptions

- Do not use exceptions for normal control flow.
- Do not put try/catch blocks in the hot path unless there is a specific, documented reason.
- Handle errors at the appropriate boundary.
- Do not catch an exception merely to log it and rethrow it without adding useful context.
- If an operation is guaranteed not to throw, do not add exception handling solely for defensive appearance.

### Logging

- Do not log in tight loops or per-message hot paths unless explicitly required.
- Prefer structured log messages with fmt formatting.
- Use the project's Logger abstraction rather than direct printf/std::cout.
- Avoid expensive string construction when the log level is disabled.

---

## 4. Source of truth

The design is already decided. It lives in:

- `README.md` — the decisions, with reasons and rejected alternatives. This is what the interviewer will read.
- `docs/DESIGN.md` — full architecture, in detail.

**Do not change a design decision silently.** If while coding you find that a decision in these files is wrong or will not work, stop, explain the problem, and propose the change. Wait for Anton to decide.

If the code and the docs disagree, that is a bug. Say so.

When a decision is made or changed during a session, update `README.md` or `docs/DESIGN.md` in the same session — after asking. The docs must never fall behind the code.

---

## 5. Build order

Follow this order. Do not skip ahead.

0. **Contracts** — domain types, `IMarketDataProvider` interface, the `.proto` file. Nothing else until these are agreed.
1. **Thin end-to-end slice** — one exchange, `std::map` book, BBO only, gRPC, one client printing to stdout. Ugly but complete and working.
2. **Docker + docker-compose** — early, not at the end. It is a required deliverable and must not be at risk.
3. **Record and replay provider** — makes everything after this testable with repeatable results.
4. **Exchanges 2 and 3** — plus the consolidator, per-exchange attribution, the merge.
5. **Staleness policy** — watchdog, drift detector, admission rule, exchange status on the wire.
6. **Volume bands and price bands** — single pass, with golden test cases.
7. **The three client binaries.**
8. **Benchmarks first, then optimization** — no optimization without a before number.
9. **README finished, hardening.**

Tests are written alongside every step, not at the end.

---

## 6. Testing

`md_core` (domain types, books, consolidator, band math) has **no I/O and no networking**. This is deliberate — it is what makes the logic testable. Keep it that way. If something in `md_core` needs a socket or a clock, that is a design mistake; say so.

Keep the `std::map` book implementation permanently as a test oracle. Every property test runs both implementations and asserts they produce the same result.

---

## 7. Never do these

- **Never write code before explaining and getting a yes.**
- **Never edit several files in one step** without proposing the group first.
- **Never invent a number.** If you say something costs 2 microseconds, either measure it or say clearly that it is an estimate.
- **Never use floating point** for prices or quantities. Scaled `int64` everywhere.
- **Never mix spot and futures** data in one book.
- **Never mix the fast BBO stream into the depth book.** The two streams are not sequenced together.
- **Never "simplify" by removing a design decision** without saying that you did.
- Never leave a `TODO` without also telling Anton it is there.

---

## 8. When Anton asks a question

Answer the question first, directly. Then, if useful, offer to do the work.

Do not start editing files because a question mentioned code. A question is a question.

---

## 9. Market data correctness

Exchange-specific protocol semantics must remain inside the provider.

The core domain must not depend on exchange-specific sequence fields such as Binance U/u, Bybit seq, or OKX seqId/prevSeqId.

Providers are responsible for:

- parsing exchange messages;
- snapshot initialization;
- buffering;
- sequence/continuity validation;
- detecting gaps;
- resynchronization;
- converting exchange timestamps and identifiers into domain types.

The Core consumes only validated normalized BookUpdate objects.

Never assume two exchanges use the same sequence semantics.
Never compare sequence numbers from different venues.
Never merge updates from different streams unless their ordering relationship is explicitly established.
