# cipollino

GPU vanity address generator for Tor v3 onion services.

- **CUDA** on NVIDIA GPUs (all of them in the machine by default), **Metal** on Apple
  GPUs, and a multithreaded CPU fallback. All three run the same kernel source.
- Any number of prefixes at once; stops after `-n` keys.
- Keys are generated from the OS CSPRNG, the GPU never sees secret material, and every
  key is re-derived and validated on the CPU with libsodium before it is written.

```
$ ./cipollino -n 2 cipoll onionz
device 0: Metal: Apple M5 Pro | 65536 threads x 2049 keys/batch | 2048 MiB scratch (lean) | self-test ok
cipollya765vqktqzicureqn6uje4pk2xn6eck3sbvfmyqwb4x77t3ad.onion
cipollwoeooqjbkoh464hoeao3pjpmfc4lpsjbbzzcpx4l4p2yn4w4id.onion
806M keys tested in 600 ms (1.34G keys/s), 2 keys found
```

Each key is written as a ready-to-use `HiddenServiceDir`:

```
cipollya765v….onion/hostname
cipollya765v….onion/hs_ed25519_public_key
cipollya765v….onion/hs_ed25519_secret_key      (mode 0600, directory 0700)
```

## Building

Needs a C++17 compiler, make, libsodium (>= 1.0.18) and Python 3 for the tests.

```
# Linux + NVIDIA: nvcc on PATH selects the CUDA backend
sudo apt install build-essential libsodium-dev
make

# macOS: Metal backend
brew install libsodium
make
```

`make test` runs the whole test suite on whatever backends the machine has.

The CUDA build targets the GPU in the build machine (`-arch=native`). If your nvcc is
too old to know that flag, name the generation instead, e.g. `make CUDA_ARCH=sm_75`.
Windows is not supported (use WSL 2).

### Docker (NVIDIA)

The host needs only the NVIDIA driver, Docker and nvidia-container-toolkit; no CUDA
toolkit, compiler or libsodium.

```
make docker                          # builds cipollino (255 MB) and cipollino:dev
make docker-test                     # GPU self-test and end-to-end tests in the container
./cipollino-docker -n 2 someprefix   # keys land in ./keys, owned by you
```

`cipollino-docker` is a thin wrapper around

```
docker run --rm --init --gpus all --user "$(id -u):$(id -g)" -v "$PWD/keys:/keys:z" cipollino ARGS
```

and takes `CIPOLLINO_KEYS` (output directory), `CIPOLLINO_GPUS` (docker's `--gpus`
value) and `CIPOLLINO_IMAGE`. A prefix file for `-f` must be visible inside the
container: put it in the keys directory and pass `/keys/<name>`.

The container refuses to start a search whose output directory is not mounted from
outside, because a key written to the container's own filesystem disappears with it.

The image is built with `-arch=all-major`, so it carries machine code for every GPU
generation its nvcc knows. `cipollino:dev` keeps the toolchain and sources (`/src`) for
rebuilding with other flags.

## Usage

```
cipollino [options] PREFIX [PREFIX...]

  -n, --count N       stop after N keys (default 1; 0 = run until interrupted)
  -d, --dir DIR       where to create the <address>.onion directories (default .)
  -f, --file FILE     read more prefixes from FILE, one per line
  -b, --backend NAME  auto (default), cuda, metal or cpu
      --devices LIST  CUDA devices to use, e.g. 0,2 (default: all)
      --threads T     kernel threads per device
      --pairs N       pairs per batch; a batch tests 2N+1 keys per thread
      --scratch MODE  auto, full, lean or half (see Tuning)
      --launch-ms MS  target duration of one kernel launch
      --bench [SECS]  measure throughput, write nothing
      --tune          try thread counts and scratch modes, print the fastest as flags
      --selftest      cross-check the backend against libsodium exhaustively, then exit
  -q, --quiet         print only the addresses found
```

Prefixes use the onion alphabet (`a-z`, `2-7`); upper case and a trailing `.onion` are
accepted. Found addresses go to stdout, everything else to stderr. Exit status is 0 when
the requested number of keys was found, 130 when interrupted first.

Every extra character multiplies the work by 32:

| prefix length | keys to test (average) | at 1.5 G keys/s (M5 Pro) | at 15 G keys/s (RTX 4090) |
|---|---|---|---|
| 6 | 1.1e9 | 0.7 s | instant |
| 7 | 3.4e10 | 23 s | 2.3 s |
| 8 | 1.1e12 | 12 min | 73 s |
| 9 | 3.5e13 | 6.5 h | 39 min |
| 10 | 1.1e15 | 8.7 days | 21 h |
| 11 | 3.6e16 | 9 months | 28 days |
| 12 | 1.2e18 | 24 years | 2.4 years |

Searching for several prefixes costs nothing extra per key, so the time to the *first*
hit divides by the number of (equally long) prefixes.

## Speed

Measured with `cipollino --bench`:

| device | backend | keys/s |
|---|---|---|
| NVIDIA RTX 4090 (in Docker, 60 s run) | CUDA | 15.5 G |
| Apple M5 Pro, first ~20 s | Metal | 1.7 G |
| Apple M5 Pro, sustained (laptop in "Automatic" power mode, heat-soaked) | Metal | 1.0 - 1.5 G |
| Apple M5 Pro, 18 cores | CPU fallback | 150 M |

On the 4090 the search runs at the card's 450 W power limit with the memory controller
28% busy (`half` scratch mode), so it is bound by arithmetic, not by memory. An
8-character prefix took 99 s (1.5e12 keys) in a real run.

## How it works

An onion address is `base32(pubkey || checksum || version)`, so a prefix is a condition
on the first bytes of the Ed25519 public key, which are the low bits of the point's `y`
coordinate. The search therefore only ever computes (part of) `y`.

Each GPU thread owns a public point `C = a*B` for its own secret scalar `a`. A batch
tests the `2N+1` keys `C + j*G8`, `j = -N..N`, with `G8 = 8*B` (steps of 8 keep the
scalar a valid clamped Ed25519 scalar), then moves `C` by `(2N+1)*G8`.

With `Q_i = i*G8 = (x_i, y_i)` from a table shared by all threads, the two candidates of
a pair are, in affine twisted Edwards coordinates,

```
y(C + Q_i) = (A + B) / (1 - t)        A = y1*y_i   B = x1*x_i
y(C - Q_i) = (A - B) / (1 + t)        t = d*x1*y1*x_i*y_i = (x1*y1) * k_i
```

What makes it fast:

1. **One inversion per pair.** Both denominators divide `1 - t^2`, so with
   `inv = 1/(1 - t^2)` and `w = t*inv` the candidates are `(A+B)(inv+w)` and
   `(A-B)(inv-w)`. `1 - t^2 = 1 - (x1*y1)^2 * k_i^2` needs one multiplication by a
   table constant.
2. **Montgomery's trick** over the batch: one real field inversion per `N` pairs, three
   multiplications per pair.
3. **The last two multiplications are not done.** Only the first 32 bits of `y` are
   needed to reject a candidate. For a product `f*g` in radix 2^25.5 those depend on the
   low halves of columns 0 and 1 (plain 32-bit multiplies) and on the carry out of bit
   255, which columns 8 and 9 determine except in about one product in 4000. That case
   is detected and falls back to the full product. A candidate costs 20 wide and 20
   narrow multiplies and no carry chain instead of 100 wide ones and a reduction.
4. **A carry chain without the wrap-around pass**: the carry out of bit 255 is estimated
   (to within one) before the chain and folded in up front, so one pass leaves limbs that
   pack into 256 bits with shifts alone.
5. **Radix 2^25.5 with unsigned limbs**: every partial product is a 32x32->64
   multiply-add into a 64-bit column, which is the one thing both NVIDIA (`IMAD.WIDE`)
   and Apple GPUs do well. I measured 8x32 saturated, 5x51 and precomputed-multiple
   variants on the M5; all were slower.

That comes to 8 multiplications, one squaring and two 32-bit probes per pair in the
default `lean` mode: about 4.6 multiplication-equivalents per key (a squaring costs 0.57
of a multiplication on the M5, a probe 0.35), plus 1-2% for the batch inversion.

Multiple prefixes: up to four are compared directly against the 32 bits; more than four
go through a 2 MiB bitmap indexed by the first 24 bits of the key. Survivors are
compared against all prefixes on 64 bits on the GPU and then in full on the CPU.

The kernel (`src/kernel/`) is written in the common subset of CUDA C++, the Metal
Shading Language and plain C++, so one source is compiled three ways.

## Security

**Where keys come from.** Every GPU thread gets its own 32-byte seed from the OS CSPRNG
(libsodium `randombytes`), expanded exactly like a normal Ed25519 key: SHA-512, clamp,
scalar `a` and nonce prefix. The key that is written is `a + 8m` for a public offset `m`
found by the search, with the seed's nonce prefix. The secret scalar is thus a uniformly
random clamped scalar, conditioned only on its public key having the prefix.

**One key per seed.** Two keys derived from one seed would have scalars a known distance
apart and share a nonce prefix, which links them and can leak both keys once both sign.
So a seed is retired the moment it yields a key and that thread is reseeded; a second
hit from the same thread in the same launch is discarded.

**The GPU holds no secrets.** It is given only the public points `a*B`. Seeds live in
libsodium guarded memory on the host and are wiped on exit.

**Nothing the GPU says is trusted.** A hit is only a position `(thread, batch, index)`.
For each one the host, using libsodium alone:

1. rebuilds the scalar from the seed and offset and checks it is still clamped,
2. recomputes the public key from that scalar,
3. checks the key is a canonical point of the prime-order group,
4. derives the address and compares it with the requested prefixes as text,
5. signs a message with the expanded key the way Tor does and verifies the signature.

Only then is the key written. A hit that fails is discarded with a warning.

**The GPU is checked too**, so that it does not silently *miss* keys:

- At every start, each device is asked to find a few dozen keys that the CPU derived
  from that device's own seeds (first/last thread, both signs, after a centre move). It
  must report exactly those.
- `--selftest` enumerates every key of small configurations with libsodium and requires
  the device to report exactly the matching set, for all scratch modes and filter modes.
- SHA3-256 and address encoding (the two things libsodium does not provide) are checked
  against known answers at start, and against Python's `hashlib` in the tests.

`tests/test_cli.py` additionally re-verifies written keys with an independent pure-Python
Ed25519: address, clamping, public key from scalar, group order, sign/verify.

Not addressed: side channels on the machine running the search, and the search machine
itself being compromised. Generate keys on a machine you trust.

## Tuning

Defaults are per backend; `--tune` measures alternatives on your hardware.

- `--scratch` trades device memory traffic for arithmetic. `full` stores two field
  elements per pair, `lean` one (and pays one extra squaring), `half` one per two pairs
  (half a multiplication more). Fast GPUs can outrun their memory bandwidth, so on CUDA
  the three are measured at start-up; the fastest is used, or the one needing least
  memory among those within 2% of it. On an RTX 4090 `full` saturates the memory bus
  (11.7 G keys/s) while `lean` and `half` reach 15.5-16 G. On Apple GPUs `lean` wins.
- `--threads`: CUDA defaults to 1024 per multiprocessor (131072 on a 4090), Metal to
  65536. More threads cost memory; too few leave the GPU idle.
- `--pairs`: bigger batches amortise the per-batch inversion (1.5% overhead at 1024);
  memory is `threads * pairs * 32` bytes in `lean` mode.
- `make CUDA_AOS=1` switches the CUDA scratch layout from one-word-per-warp-access to
  one-slot-per-thread. On Apple GPUs that layout is clearly better; on an RTX 4090 the
  two are within noise in `half` mode and the default is 8% faster in `lean`.

On a laptop the GPU is thermally limited within a minute; macOS's High Power mode, where
available, raises the sustained rate.

## Tests

```
make test          # everything this machine can run
make docker-test   # the GPU part inside the container (NVIDIA)
```

- `tests/test_field.py`: field arithmetic against Python big integers, including the
  worst-case limb values of every operand class and the documented output bounds.
- `--selftest` on the CPU, Metal and (when present) CUDA backends, and on the CUDA
  backend compiled against `tests/cuda_sim.h`.
- `tests/test_cli.py`: end-to-end runs, re-verified in Python.

## Layout

```
src/kernel/defs.h      target abstraction (CUDA / Metal / CPU)
src/kernel/field.h     GF(2^255-19)
src/kernel/search.h    the per-thread search loop
src/backend_*.{cu,mm,cpp}
src/engine.cpp         seeding, launches, hit validation, self-tests
src/keys.cpp           all secret-key handling (libsodium)
src/ed.cpp             public-point helpers and the kernel table
src/filter.cpp         prefixes -> GPU filter tables
src/onion.cpp          SHA3-256, address encoding
Dockerfile, docker/, cipollino-docker    container build, entry-point guard, run wrapper
```
