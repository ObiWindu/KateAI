# RAM baseline and budget evidence

Measured on `feature/harness_complete`, Linux x86_64, Release build
(`-O3 -DNDEBUG`), Qt 6.12.0, 2026-11-25.

Command:

```sh
./build/bin/test_memory_budget
ctest --test-dir build --output-on-failure -R '^test_memory_budget$'
```

## Scenarios and observed peak/current-RSS deltas

| Scenario | Result | Budget |
|---|---:|---:|
| Restore 2,000 messages × 4 KB | 5,320,704 bytes (~5.1 MB) | ≤ 40 MB |
| 200 assistant messages with 100 KB tool arguments | transcript bounded to 75 messages / 7,480,186 counted chars | transcript must prune |
| 8 MB stored tool result, read a 20,000-char page | 0 bytes RSS delta after the page | ≤ 16 MB |
| Scan 3,000 files into the project graph | 131,072 bytes (~0.13 MB) | ≤ 25 MB; ≤ 1,000 nodes |

The write-heavy scenario also confirms:

- `actionRepeatEntries` is 0 after restore (legacy full-signature state is not
  resurrected),
- `transcriptDiverged` is true after pruning,
- the retained model history is 201 messages, while the display transcript is
  bounded to 75 messages / about 7.1 MiB of counted content.

## Performance note

These are RSS-budget tests, not microbenchmarks. The existing timing-sensitive
suites (`test_livecards`, `test_overflow`, `test_historyrestore`,
`test_toolcard`) remain green, so the changes did not regress their measured
paths.
