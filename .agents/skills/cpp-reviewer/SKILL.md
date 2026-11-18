---
name: cpp-reviewer
description: Invoke whenever reviewing C++ (C++20 or C++23) code; performs a deep critical review and recommends improvements aligned with the C++ Core Guidelines and modern STL usage.
---

# Modern C++20/23 Code Review Skill

## Purpose

Perform a **deep, critical code review of modern C++ (C++20 and C++23)** and recommend improvements aligned with the **C++ Core Guidelines** and modern STL usage.

Focus on:

- Correctness
- Safety and undefined behavior
- Lifetime and ownership clarity
- Idiomatic modern C++
- Proper use of the STL
- Simplification opportunities
- Performance pitfalls (only when meaningful)

Actively search for opportunities to **replace handwritten code with STL algorithms, ranges, and containers**.

**Important:** Before reviewing, identify whether the codebase targets C++20 or C++23. Some features (`std::ranges::contains`, `std::ranges::to`, `std::expected`, `views::zip`, `views::chunk`, `views::slide`, `std::print`, `std::mdspan`, `std::move_only_function`, deducing `this`) are **C++23 only** and must not be recommended for C++20 codebases.

---

# Core Review Principles

Prioritize issues in this order:

1. **Correctness**
2. **Safety / Undefined Behavior**
3. **Lifetime / Ownership clarity**
4. **API design**
5. **Code simplification**
6. **Idiomatic C++**
7. **Performance** (never recommend premature optimization)

---

# Review Output Format

Structure every response using this format. Omit sections that do not apply.

## 1. Summary

Short high-level evaluation (3–5 bullet points). Example:

- Code is readable but uses outdated iteration patterns.
- Several opportunities to use `std::ranges`.
- Ownership semantics are unclear in two functions.

---

## 2. Critical Issues

Problems that may cause bugs, undefined behavior, lifetime errors, resource leaks, or concurrency hazards.

For each issue, provide:
- **What:** clear explanation of the problem
- **Why:** why it is dangerous or incorrect
- **Guideline:** C++ Core Guidelines reference (e.g., R.11, ES.65)
- **Fix:** concrete corrected code

---

## 3. Modern C++ Improvements

Suggestions to modernize the code:

- Replace raw loops with algorithms or ranges pipelines
- Use structured bindings
- Replace raw pointers with RAII types
- Simplify APIs using vocabulary types (`std::span`, `std::string_view`, `std::optional`)
- Add `[[nodiscard]]` where appropriate
- Use `constexpr` / `consteval` where applicable

---

## 4. STL Simplification Opportunities

Show how handwritten code can be replaced with algorithms, ranges, or standard containers. Provide before/after code examples. Note the required C++ standard version for each suggestion.

---

## 5. Performance Observations

Include only when the performance issue is meaningful and non-speculative:

- Unnecessary copies or allocations
- Missing move semantics
- Suboptimal container choice
- Missing `reserve` (only when the final size is known or can be estimated)

---

## 6. Refactored Example

Provide a clean, modern C++ rewrite of the most problematic section. Annotate non-obvious changes with comments.

---

# C++ Standard Version Awareness

Always note the minimum required standard for each suggestion.

## Features available in C++20

- `auto`, structured bindings, `constexpr`, `consteval`
- `[[nodiscard]]`, `[[likely]]`, `[[unlikely]]`
- `std::span`, `std::string_view`
- `std::optional`, `std::variant`
- `std::ranges::sort`, `std::ranges::find`, `std::ranges::find_if`, `std::ranges::count_if`, `std::ranges::copy`, `std::ranges::transform`
- `std::views::filter`, `std::views::transform`, `std::views::take`, `std::views::drop`, `std::views::reverse`, `std::views::iota`
- `std::format`
- `std::jthread`, `std::stop_token`
- Concepts (`requires`, `concept`)
- Coroutines (`co_await`, `co_yield`, `co_return`)
- Three-way comparison (`<=>`)

## Features available in C++23 only

- `std::expected` — preferred over error codes for recoverable errors
- `std::ranges::contains` — replaces manual search loops
- `std::ranges::to<Container>` — materializes a range into a container
- `std::mdspan` — multidimensional non-owning view
- `std::move_only_function` — move-only alternative to `std::function`
- `std::print` / `std::println` — type-safe formatted output
- `views::zip`, `views::enumerate`, `views::chunk`, `views::slide`, `views::join_with`
- Deducing `this` (explicit object parameter)

---

# STL Refactoring Patterns

Detect and replace these handwritten patterns.

### Replace Search Loops (C++23: `std::ranges::contains`)

Avoid:
```cpp
for (const auto& v : vec) {
    if (v == target) return true;
}
```

C++20 alternative:
```cpp
std::ranges::find(vec, target) != vec.end();
```

C++23 preferred:
```cpp
std::ranges::contains(vec, target);
```

---

### Replace Manual Counting

Avoid:
```cpp
int count = 0;
for (auto v : vec) {
    if (v > 10) count++;
}
```

Prefer (C++20):
```cpp
auto count = std::ranges::count_if(vec, [](auto v){ return v > 10; });
```

Note: prefer `std::ptrdiff_t` or `auto` for the result type; `count_if` returns `std::ranges::range_difference_t<R>`, not `int`.

---

### Replace Transform Loops

Avoid:
```cpp
std::vector<int> out;
for (auto v : vec) {
    out.push_back(v * 2);
}
```

C++20 alternative:
```cpp
std::vector<int> out;
std::ranges::transform(vec, std::back_inserter(out), [](int v){ return v * 2; });
```

C++23 preferred (lazy, no intermediate allocation):
```cpp
auto out = vec
    | std::views::transform([](int v){ return v * 2; })
    | std::ranges::to<std::vector>();
```

---

### Replace Filter Loops

Avoid:
```cpp
std::vector<int> out;
for (auto v : vec) {
    if (v % 2 == 0) out.push_back(v);
}
```

C++20 alternative:
```cpp
std::vector<int> out;
std::ranges::copy_if(vec, std::back_inserter(out), [](int v){ return v % 2 == 0; });
```

C++23 preferred:
```cpp
auto out = vec
    | std::views::filter([](int v){ return v % 2 == 0; })
    | std::ranges::to<std::vector>();
```

---

### Replace Sorting

Prefer (C++20):
```cpp
std::ranges::sort(vec);
```

For custom comparators:
```cpp
std::ranges::sort(vec, std::greater{});
std::ranges::sort(vec, {}, &MyType::key_field); // projection
```

---

### Replace Index Iteration (when index is not needed)

Avoid when only the element is used:
```cpp
for (size_t i = 0; i < vec.size(); ++i) {
    process(vec[i]);
}
```

Prefer:
```cpp
for (const auto& v : vec) {
    process(v);
}
```

**Important:** When the index is actually needed (e.g., for positional logic or parallel arrays), index-based loops are correct and should not be changed. In C++23, `views::enumerate` provides both index and element cleanly:
```cpp
for (auto [i, v] : std::views::enumerate(vec)) { ... }
```

---

# Containers Guidance

Prefer STL containers over handwritten equivalents.

| Scenario                    | Recommended               |
|-----------------------------|---------------------------|
| Dynamic array               | `std::vector`             |
| Fixed-size array            | `std::array`              |
| Hash-based lookup           | `std::unordered_map`      |
| Sorted/ordered lookup       | `std::map`                |
| Unique sorted elements      | `std::set`                |
| Unique hash-based elements  | `std::unordered_set`      |
| FIFO queue                  | `std::queue`              |
| LIFO stack                  | `std::stack`              |
| Priority queue              | `std::priority_queue`     |

Flag and recommend replacing:

- Custom linked list implementations (prefer `std::vector` + index; `std::list` is rarely the right choice due to cache behavior)
- Raw dynamic arrays (`new T[n]`) — use `std::vector`
- Manual container implementations that duplicate STL behavior

---

# Ownership and Lifetime Rules

Follow RAII. Every resource must have a clear owner.

**Prefer owning types:**
```cpp
std::unique_ptr<T>          // single ownership
std::shared_ptr<T>          // shared ownership (use sparingly; prefer unique_ptr)
std::vector<T>              // owns a dynamic sequence
std::string                 // owns a string
```

**Avoid:**
```cpp
new / delete
malloc / free
owning raw pointers (T*)
```

**For non-owning views (parameters and local aliases only):**
```cpp
std::span<T>         // non-owning view of contiguous sequence
std::string_view     // non-owning view of character sequence
```

Never return `std::span` or `std::string_view` that refers to a local variable or temporary.

---

# Lifetime Hazards to Detect

Flag all of these patterns.

### Dangling `string_view` or `span`

```cpp
// WRONG: string_view returned from function refers to local temporary
std::string_view foo() {
    std::string s = "hello";
    return s; // dangling: s is destroyed on return
}
```

### Dangling range views

```cpp
// WRONG: view outlives the container it refers to
auto view = vec | std::views::filter([](int v){ return v > 0; });
vec.clear(); // view is now dangling
```

### Returning references to locals (UB)

```cpp
// WRONG: undefined behavior
int& foo() {
    int x = 0;
    return x; // x is destroyed on return
}
```

### Temporaries bound to views

```cpp
// WRONG: getVector() returns a temporary; view dangles immediately
auto view = getVector() | std::views::filter(...);
```

Materialization fix:
```cpp
auto vec = getVector(); // extend lifetime
auto view = vec | std::views::filter(...);
```

---

# Iterator Invalidation Checks

Detect when iterators or pointers are invalidated.

### Modifying a vector during iteration (UB)

```cpp
// WRONG: push_back may reallocate, invalidating 'it'
for (auto it = v.begin(); it != v.end(); ++it) {
    v.push_back(10);
}
```

Recommend:
- Collect items to add separately, then append after iteration
- Call `v.reserve(n)` before iteration if size is known (prevents reallocation, but does not make in-loop mutation safe)
- Use index-based iteration only when reallocation is prevented

### Erasing during iteration

```cpp
// WRONG: erasing invalidates iterators after the erased element
for (auto it = v.begin(); it != v.end(); ++it) {
    if (*it == 0) v.erase(it);
}
```

Prefer the erase-remove idiom (C++20):
```cpp
std::erase_if(v, [](int x){ return x == 0; });
```

---

# Ranges Pitfalls

### Dangling pipeline from temporary (see Lifetime section above)

### Accidental copies in range-for

```cpp
for (auto v : vec)   // copies each element
```

Prefer:
```cpp
for (const auto& v : vec)  // no copy
for (auto& v : vec)        // when mutation is needed
```

### Unnecessary materialization

Avoid converting a view to `std::vector` before passing it to an algorithm that accepts any range:
```cpp
// Unnecessary: sort accepts ranges directly in C++20
auto vec = view | std::ranges::to<std::vector>(); // C++23
std::ranges::sort(vec);

// Prefer: if you need a sorted, owning container, this is fine;
// but if you only need to iterate, keep it as a view.
```

---

# Concurrency Review Rules

Detect the following issues.

### Data races

Shared mutable data accessed from multiple threads without synchronization is undefined behavior.

### Missing mutex protection

Reads and writes to shared state must be consistently protected by the same mutex.

### Incorrect atomic usage

`std::atomic` operations are individually atomic but sequences of operations are not. Flag incorrect assumptions about compound atomicity:
```cpp
// WRONG: two separate atomic operations — not an atomic read-modify-write
if (flag.load()) flag.store(false);

// Correct: use compare_exchange for conditional update
bool expected = true;
flag.compare_exchange_strong(expected, false);
```

### Deadlock risk

Flag inconsistent lock ordering, or `std::lock_guard` used in a way that cannot be unlocked before a blocking operation.

### Thread lifetime issues

```cpp
// WRONG: t is destroyed without join or detach — terminates the program
std::thread t(foo);
```

Prefer `std::jthread` (C++20), which automatically joins on destruction and supports cooperative cancellation via `std::stop_token`:
```cpp
std::jthread t([](std::stop_token st) {
    while (!st.stop_requested()) { /* work */ }
});
```

---

# Error Handling Guidance

### C++23: prefer `std::expected` for recoverable errors

```cpp
std::expected<Value, Error> parse(std::string_view input);
```

This makes the error path explicit in the type system without exceptions or output parameters.

### C++20 and earlier: use `std::optional` for absence, exceptions for unexpected failures

```cpp
std::optional<Value> find(const Key& k); // absence is expected
```

**Avoid:**
- Returning error codes stored in `int` or `bool` with out-parameters
- Sentinel values (e.g., returning `-1` for "not found")
- Global error flags (`errno`-style)

**Note:** Exception-based error handling is appropriate when failure is genuinely unexpected and recovery happens at a different stack level. Blanket use of `noexcept` throughout an interface is a code smell unless the functions truly cannot fail.

---

# Performance Anti-Patterns

Flag these patterns (only when the impact is non-trivial).

### Unnecessary copies in function parameters

```cpp
// Copies the entire vector
void foo(std::vector<int> v);

// Prefer: non-owning view for read-only access
void foo(std::span<const int> v);

// Or: const ref if you need the full vector interface
void foo(const std::vector<int>& v);
```

**Note on pass-by-value:** Pass by value is correct when the function needs to own a copy anyway (e.g., storing into a member). In that case, passing by value and moving is preferred (C++ Core Guidelines F.15–F.18).

### Missing `reserve` before known-size push_back loops

```cpp
std::vector<int> v;
for (int i = 0; i < n; ++i) v.push_back(compute(i));
```

If `n` is known before the loop:
```cpp
v.reserve(n);
```

Do not recommend `reserve` when the final size is not known.

### Poor container choice

- Prefer `std::unordered_map` over `std::map` when ordering is not needed (O(1) vs O(log n) lookup)
- Avoid `std::list` unless you have measured pointer stability or splice-heavy workloads; cache behavior is poor
- Prefer `std::vector` of indices or flat structures over pointer-heavy trees for small collections

### Unnecessary allocations in loops

Moving repeated heap allocations outside the loop, or reusing buffers via `clear()` + `resize()` instead of constructing a new container each iteration.

---

# API Design Guidance

Encourage:
- **`const` correctness** — mark all member functions that do not mutate state as `const`
- **Value semantics** — prefer types that are copyable and movable; avoid surprising aliasing
- **Clear ownership** — APIs should make it obvious whether they take ownership or borrow
- **Small, focused interfaces** — prefer free functions over large method collections (C++ Core Guidelines I.2)
- **`[[nodiscard]]`** — apply to functions whose return value must not be silently ignored (e.g., factory functions, `expected`, `optional`, error codes)

**Prefer view types for input parameters:**

```cpp
// Avoid: forces the caller to use std::vector specifically, and copies
void process(std::vector<int> v);

// Prefer: accepts any contiguous range, no copy
void process(std::span<const int> v);

// Prefer: accepts any string-like without allocation
void log(std::string_view msg);
```

**Prefer explicit output parameters or return values over in-out parameters:**
```cpp
// Unclear: is result an input, an output, or both?
void compute(int input, int& result);

// Clear:
int compute(int input);
std::expected<int, Error> compute(int input); // C++23
```

---

# Core Guidelines Quick Reference

Cite these when relevant.

| Guideline | Rule |
|-----------|------|
| R.1       | Manage resources automatically using RAII |
| R.5       | Prefer scoped objects; avoid heap allocation when unnecessary |
| R.11      | Avoid calling `new` and `delete` explicitly |
| F.15      | Prefer simple and conventional ways of passing information |
| F.16      | For "in" parameters, pass cheaply-copied types by value and others by reference to `const` |
| F.18      | For "will-move-from" parameters, pass by `X&&` and `std::move` the parameter |
| I.2       | Avoid non-`const` global variables |
| ES.23     | Prefer the `{}` initializer syntax |
| ES.47     | Use `nullptr` rather than `0` or `NULL` |
| ES.65     | Don't dereference an invalid pointer |
| C.20      | If you can avoid defining any default operations, do |
| C.21      | If you define or `=delete` any default operation, define or `=delete` them all (Rule of Five) |
| CP.1      | Assume your code will run as part of a multi-threaded program |

---

# Review Tone

Be:
- **Constructive** — explain why something is a problem, not just that it is
- **Concise** — flag real issues; don't nitpick style uniformly enforced by a formatter
- **Technically precise** — cite the standard or guideline; avoid vague claims
- **Pragmatic** — prefer minimal, high-impact changes over wholesale rewrites

Avoid:
- Recommending C++23 features to a C++20 codebase without acknowledging the constraint
- Flagging index-based loops when the index is genuinely needed
- Recommending `reserve` without knowing whether the size is predictable
- Treating every performance observation as a critical issue

**Goal:** Guide code toward modern idiomatic C++, clear ownership semantics, strong STL usage, and safe, maintainable design — without unnecessary churn.
