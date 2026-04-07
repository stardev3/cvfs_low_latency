# CVFS (Custom Virtual File System)

CVFS is a **low-latency, concurrency-safe virtual file system simulation** in C/C++ that emphasizes **systems performance engineering**: predictable critical sections, careful shared-state management, and runtime measurement of operation latency. It models core Linux VFS ideas (inodes + per-FD state) while staying close to low-level implementation details (explicit buffers, offsets, and fixed-size tables).

This version preserves the original CLI behavior/architecture while adding:

- **Thread-safety**: fine-grained locks for metadata, per-FD state, and per-inode data
- **Performance measurement**: opt-in microsecond latency logging for key operations
- **Testing support**: optional script mode for deterministic runs and an optional multithreaded stress test for concurrency

---

## Key highlights (Added)

- **Fine-grained locking**: avoids a single global lock; isolates contention to metadata, per-FD state, or per-inode state.
- **Latency instrumentation**: microsecond logging (`std::chrono`) for Create/Read/Write/Delete to support real measurement-driven iteration.
- **Concurrent access stress testing**: configurable multi-thread test to validate behavior under contention.
- **Tail-latency awareness**: captures worst-case behavior under load (not just averages), which is the right lens for low-latency systems.
- **Deterministic testing**: script-driven CLI runs to produce repeatable logs and comparable results across runs/machines.

---

## System Architecture

- **INODE list (DILB)**: fixed count (`MAXINODE`) inode nodes are created at startup; free inodes are those with `FileType == 0`.
- **UFDT (User File Descriptor Table)**: array of pointers to `FILETABLE`, indexed by file descriptor.
- **FILETABLE**: per-open-FD state (`readoffset`, `writeoffset`, `mode`, `count`, `ptrinode`).

### Concurrency model (fine-grained locking)

CVFS uses three levels of locking:

- **Global metadata lock**: protects superblock counters + UFDT slot allocation/free + inode selection during create/open/delete.
- **Per-FD lock**: protects a single UFDT slot and its `FILETABLE` offsets for that FD.
- **Per-inode lock**: protects inode metadata and the file `Buffer` for reads/writes/truncation/unlink.

Lock order: **UFDT(fd) → inode** (to avoid deadlocks).

---

## Build (Windows + MinGW)

Open PowerShell in this folder:

`directory_path`

Compile:

```powershell
g++ -std=c++17 -O2 "main.cpp" -o cvfs.exe
```

If you see:
`cannot open output file cvfs.exe: Permission denied`

It usually means `cvfs.exe` is **still running** (locked). Fix:

```powershell
taskkill /IM cvfs.exe /F 2>$null
del .\cvfs.exe -ErrorAction SilentlyContinue
g++ -std=c++17 -O2 "main.cpp" -o cvfs.exe
```

---

## Run

### Interactive CLI

```powershell
.\cvfs.exe
```

You should see:

```text
DILB created successfully

Marvellous VFS : >
```

---

## Performance measurement (opt-in)

Perf logging is **opt-in** and prints to **stderr** so the default CLI stdout output stays unchanged.

Enable:

```powershell
$env:CVFS_PERF="1"
.\cvfs.exe
```

Example perf lines:

```text
[PERF] WriteFile latency: 12 us
```

Measured operations:
- `CreateFile`
- `WriteFile`
- `ReadFile`
- `DeleteFile` (via `rm`)

### Note on measurement resolution
On fast in-memory operations you may see many `0 us` measurements. In that case, focus on **tail latency** (max) and/or switch to nanosecond reporting if you extend the project.

---

## Optional test modes (do not affect the CLI)

### `--mt-test` (multithreaded stress test; no CLI)

Runs a concurrent read/write workload using the same CVFS APIs.

```powershell
.\cvfs.exe --mt-test
```

You can also parameterize it:

```powershell
.\cvfs.exe --mt-test <writers> <readers> <writes_per_writer> <reads_per_reader>
```

Example:

```powershell
.\cvfs.exe --mt-test 4 4 2000 500
```

The test prints a configuration line to stderr:

```text
[MT] writers=4 readers=4 writes_per_writer=2000 reads_per_reader=500
```

### `--mt-test-verify` (Added: stress test + integrity checksum; no CLI)

Runs the same workload as `--mt-test`, plus an integrity verification step that computes a deterministic checksum of the final file buffer **under the inode lock**.

```powershell
.\cvfs.exe --mt-test-verify <writers> <readers> <writes_per_writer> <reads_per_reader>
```

Example:

```powershell
.\cvfs.exe --mt-test-verify 4 4 2000 500
```

Expected stderr includes:

```text
[MT] verify=true
[MT][CHECK] ... size_matches=true
[MT][VERIFY] fnv1a64=0x... bytes=...
```

### `--script <file>` (deterministic CLI runs; no interactivity)

Runs the *same* CLI parser loop but reads commands from a file. This is intended for perf capture and repeatable testing.

```powershell
.\cvfs.exe --script perf_input.txt
```

Notes:
- `write <FileName>` expects an extra line of data in the script (the next line is taken as the write payload).
- EOF ends the run cleanly.

---

## CLI commands (complete reference)

### `help`
Prints the command list.

```text
help
```

### `man <command>`
Shows usage help for a specific command.

```text
man create
man read
man write
```

### `clear`
Clears the console.

```text
clear
```

### `ls`
Lists all existing files (inodes with `FileType != 0`).

```text
ls
```

### `create <FileName> <Permission>`
Creates a new regular file and opens it (returns a file descriptor).

```text
create demo.txt 3
```

**Permissions:**
- `1` = Read only
- `2` = Write only
- `3` = Read + Write

### `open <FileName> <Mode>`
Opens an existing file with a mode (returns a new file descriptor).

```text
open demo.txt 1
open demo.txt 2
open demo.txt 3
```

### `close <FileName>`
Closes the file (by name).

```text
close demo.txt
```

### `closeall`
Closes open files (per current implementation behavior).

```text
closeall
```

### `write <FileName>`
Writes data into the file. After running the command, you will be prompted to enter data.

```text
write demo.txt
Enter the Data :
hello world
```

### `read <FileName> <Bytes>`
Reads `<Bytes>` bytes from the file and prints them.

```text
read demo.txt 5
```

### `lseek <FileName> <ChangeInOffset> <StartPoint>`
Moves the read/write offset.

StartPoint values:
- `0` = START
- `1` = CURRENT
- `2` = END

Examples:

```text
lseek demo.txt 0 0
lseek demo.txt 10 1
lseek demo.txt -3 2
```

### `truncate <FileName>`
Clears file contents and resets offsets.

```text
truncate demo.txt
```

### `rm <FileName>`
Deletes the file (unlink). Frees the buffer on final unlink.

```text
rm demo.txt
```

### `stat <FileName>`
Shows file metadata (by name).

```text
stat demo.txt
```

### `fstat <FD>`
Shows file metadata (by file descriptor).

```text
fstat 3
```

### `exit`
Terminates the virtual file system.

```text
exit
```

---

## Typical interactive session

```text
Marvellous VFS : > create a.txt 3
Marvellous VFS : > write a.txt
Enter the Data :
hello
Marvellous VFS : > read a.txt 5
Marvellous VFS : > stat a.txt
Marvellous VFS : > rm a.txt
Marvellous VFS : > exit
```

---

## Capturing logs for analysis (PowerShell)

### Capture interactive CLI + perf (prints while saving)

```powershell
cd "directory_path"
$env:CVFS_PERF="1"
.\cvfs.exe 2>&1 | Tee-Object -FilePath .\run1.log
```

### Capture a script run + perf (fully non-interactive)

```powershell
cd "directory_path"
$env:CVFS_PERF="1"
.\cvfs.exe --script perf_input.txt 2>&1 | Tee-Object -FilePath .\script_run.log
```

---

## Measured scalability (example results)

The following table is from an actual run sweep (with `CVFS_PERF=1`) using:

`--mt-test <writers> <readers> <writes_per_writer> <reads_per_reader>`

```text
threads  w  r  op        n      avg_us  min  p50   p95    max
2        1  1  WriteFile 2000   0.000   0    0.000 0.000  0
2        1  1  ReadFile  500    0.000   0    0.000 0.000  0
4        2  2  WriteFile 4000   0.228   0    0.000 0.000  910
4        2  2  ReadFile  1000   0.000   0    0.000 0.000  0
8        4  4  WriteFile 8000   0.111   0    0.000 0.000  885
8        4  4  ReadFile  2000   0.923   0    0.000 0.000  954
16       8  8  WriteFile 16000  0.129   0    0.000 0.000  1060
16       8  8  ReadFile  4000   0.000   0    0.000 0.000  0
```

Interpretation:
- Many ops quantize to `0 us` (very fast); the most informative metric here is the **tail latency** (max).
- Under contention, `WriteFile` tail latency reached about **~1 ms** in this sweep.

---

## Performance summary (Added)

The table above highlights two important realities of low-latency measurement:

- **Typical-case latency is extremely small**: many operations show `0 us` at microsecond precision. This implies either sub-microsecond execution or measurement quantization at µs granularity (or both).
- **Tail latency is the differentiator under concurrency**: as thread count and contention increase, the **max latency** for `WriteFile` reached roughly **~0.9–1.06 ms** in the shown sweep. This is consistent with lock contention + OS scheduling effects and is the right metric to watch for systems workloads where p99/max matters.

Scalability behavior in this sweep:
- **Work scales with threads** (more total operations completed), while **median/p95 remained at 0 µs** due to µs quantization.
- **Worst-case latency increases under contention**, which is expected for a mutex-based design and provides a concrete baseline for optimization work (e.g., reader-writer locks for read-heavy workloads).

---

## Design trade-offs (Added)

This project intentionally chooses **correctness + clarity + measurable performance** over premature complexity:

- **Why mutexes (not lock-free)**:
  - Lock-free designs can reduce contention but introduce significant complexity (ABA, memory reclamation, subtle correctness hazards).
  - For an interview-grade systems project, mutex-based synchronization provides a clean correctness story and allows you to reason explicitly about critical sections and shared-state invariants.

- **Why fine-grained locking (not a single global lock)**:
  - A single lock is simple but collapses concurrency; it makes contention the dominant cost as threads increase.
  - Fine-grained locks keep unrelated operations from blocking each other and make bottlenecks observable and localizable (metadata vs per-FD vs per-inode).

- **Performance vs simplicity**:
  - The architecture stays intentionally low-level (fixed-size tables, explicit buffers/offsets) to keep overhead predictable and cache-friendly.
  - Measurement is built-in so you can validate changes with data rather than assumptions.

### Future improvements (Added)

- **Reader-writer locking**: use shared locks for read-heavy workloads (many readers, few writers) to reduce contention.
- **Lock elision / reduced lock scope**: shrink critical sections and avoid holding metadata locks across work that can be done under a narrower per-inode/per-FD lock.
- **Higher-resolution timing**: optionally emit nanosecond timing or collect histograms (p50/p95/p99) to reduce the “0 us” quantization effect.
- **Cache optimizations**: improve locality for frequently accessed metadata and reduce copying in read/write paths where safe.
- **More correctness assertions**: optional internal invariants/consistency checks in test modes (e.g., offsets never exceed file bounds, final file size matches writes).

---

## Outcome (Added)

CVFS demonstrates the kind of engineering you’d apply to real low-latency systems:

- **Thread-safe shared-state design** with explicit invariants and deadlock-avoidant lock ordering
- **Measurement-driven performance work** (latency instrumentation + tail-latency focus under contention)
- **Deterministic + concurrent testing hooks** that let you reproduce results and reason about scalability

In short: it’s a **thread-safe, low-latency VFS simulation** built to be explainable, testable, and performance-aware—aligned with the expectations of high-performance systems interviews.

