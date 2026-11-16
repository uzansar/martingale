# ADR-001: Fixed-Point Integer Ticks vs. IEEE-754 Floating-Point Arithmetic

## Status
Accepted (Milestone 1 — Weeks 1–2)

## Context
In an electronic exchange matching engine, incoming orders specify limit prices and quantities. A naive implementation uses `double` (IEEE-754 64-bit binary floating-point) for prices. However:
1. Decimal tick sizes (e.g., `$0.01` or `$0.0001`) do not have exact representations in base-2 floating-point (`0.1 + 0.2 != 0.3`).
2. Price-time priority matching relies on exact equality (`==`) and strict total ordering (`<`, `>`) to index price levels in associative or flat structures (`std::map<Price, PriceLevel>`).
3. Floating-point instructions have higher latency and non-associative rounding behavior across compiler optimization levels (`-O2` vs `-O3`), threatening strict determinism.

## Decision
We represent all prices and quantities in the matching engine and order book as **64-bit integer types** (`Price = int64_t` for signed tick offsets/spreads, `Quantity = uint64_t` for unsigned share/contract counts):
- Every instrument defines a minimum tick size (e.g., `1 tick = $0.01` or `10^-4` currency units).
- Conversion between human-readable decimal prices and integer ticks occurs exclusively at the system boundary (Order Gateway ingestion / Market Data display).
- Inside the matching engine, all price comparisons and quantity reductions use exact 64-bit integer ALU operations.

## Consequences
- **Benefits:** 100% deterministic price comparisons, safe hashing/map indexing, single-cycle integer ALU comparisons, and immunity to IEEE-754 representation drift.
- **Trade-offs:** Boundary parsers and analytics layers must know the instrument's tick scaling factor to format human-readable prices.
