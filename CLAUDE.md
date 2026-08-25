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

Never write or change code without explaining first. For every step, before touching a file, say:

- **What** you are about to change, in one or two sentences.
- **Why** this way, and what the alternative was.
- **What Anton must understand** about this piece — the mechanism, not the syntax.

Then stop and wait.

### Rule 2: Wait for confirmation on every change

**Do not edit or create any file until Anton says yes.**

This applies to every single change, including small ones. No batching several files "because they go together". One step, one confirmation.

If Anton says "yes" or "go", make that one change and stop again.

### Rule 3: Small steps

One file, or one function, or one clear idea per step. If a step would produce more than about 100 lines of new code, split it and propose the split first.

If Anton asks for something large ("write the whole provider"), break it down yourself and propose the breakdown before starting.

### Rule 4: Highlight what matters

Not everything is equally important. In each explanation, mark the parts that are worth real attention:

> **KEY:** the seqlock reader must re-check the counter *after* copying, not before. If you check first, you can copy data that the writer changed halfway through.

Use `**KEY:**` for anything Anton will need at the debrief, or anything where a small mistake causes a bug that is hard to find.

Do not mark everything. If half the lines are marked KEY, none of them are.

### Rule 5: Check understanding

After finishing a piece of work, give a short list — three or four items — titled **"You should now be able to explain"**. Each item is a question the interviewer might ask.

Example:

> **You should now be able to explain:**
> - Why the consolidator owns all three books instead of each thread owning its own
> - What happens to the queue when the consolidator is slower than the parsers
> - Why an SPSC queue is enough here, and when you would need MPMC

If Anton cannot answer one, explain it again in a different way before moving on.

---

## 3. Language

Anton's English is C1. Use simple, common words and short sentences.

- Avoid rare or academic words.
- Prefer "skip old updates" over "conflate", but teach the real term once, because the interviewer will use it.
- When a technical term is unavoidable, define it the first time in one plain sentence.
- Explain with concrete numbers and small examples, not abstract descriptions.

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
