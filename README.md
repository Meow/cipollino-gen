# cipollino

GPU vanity address generator for Tor v3 onion services.

- **CUDA** on NVIDIA GPUs, **ROCm** on AMD GPUs, **Metal** on Apple GPUs, **Vulkan** on
  anything with a Vulkan driver (NVIDIA, AMD, Intel, Apple), and a multithreaded CPU
  fallback. All of them run the same kernel source, on every GPU in the machine.
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

Needs a C++17 compiler, make, libsodium (>= 1.0.18) and Python 3 for the tests (for
Windows see below). `make` builds every backend whose toolchain it finds:

| backend | needs at build time | at run time |
|---|---|---|
| CUDA | `nvcc` on PATH | NVIDIA driver, CUDA runtime |
| ROCm | `hipcc` (on PATH or in `/opt/rocm/bin`) | ROCm runtime, `/dev/kfd` |
| Metal | macOS | - |
| Vulkan | Vulkan headers, `glslangValidator`, `xxd` | Vulkan loader and a driver with 64-bit shader integers |

```
# Debian/Ubuntu (add the CUDA toolkit or ROCm for those backends)
sudo apt install build-essential libsodium-dev libvulkan-dev glslang-tools xxd
make

# macOS: Metal, plus Vulkan through MoltenVK if these are installed
brew install libsodium vulkan-headers vulkan-loader molten-vk glslang
make
```

`make test` runs the whole test suite on whatever backends the machine has.

The CUDA build targets the GPU in the build machine (`-arch=native`). If your nvcc is
too old to know that flag, name the generation instead, e.g. `make CUDA_ARCH=sm_75`.
The ROCm build takes its targets from `ROCM_ARCH`, e.g. `make ROCM_ARCH=gfx11-generic`.

### Windows

Windows is built with CMake and Visual Studio 2022's compiler instead of make;
`vcpkg.json` lists the libraries. The result is a single `cipollino.exe` with the CUDA,
Vulkan and CPU backends. At run time CUDA needs only the NVIDIA driver and Vulkan only
the loader that every GPU driver installs; on a machine with neither the program still
starts and uses the CPU.

```
:: in an "x64 Native Tools Command Prompt"; install the CUDA toolkit first for that backend
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake ^
  -DVCPKG_TARGET_TRIPLET=x64-windows-static
cmake --build build
ctest --test-dir build
```

`-DCIP_CUDA_ARCH=all` makes a program that is not tied to the build machine's GPU. That is
what `.github/workflows/windows.yml` builds on every push and keeps as the
`cipollino-windows-x64` artifact; the manually started `release.yml` attaches that
program to a draft release.

What differs from the other systems:

- there is no ROCm backend; AMD GPUs are reached through Vulkan;
- Windows has no file modes, so a key directory gets the access list of the directory it
  is created in. Keep the output directory somewhere only you can read, such as your
  profile;
- Windows resets a GPU that does not answer for two seconds. Launches are far shorter by
  default; do not raise `--launch-ms` towards that.

The Windows port is new and has not been run on a GPU under Windows yet.

### Docker

The host needs only its GPU driver and Docker (plus nvidia-container-toolkit for
NVIDIA); no SDK, compiler or libsodium. Two images:

| image | backends | build | size |
|---|---|---|---|
| `cipollino` | CUDA, Vulkan (NVIDIA; AMD and Intel through Mesa) | `make docker` | 590 MB |
| `cipollino:rocm` | ROCm, Vulkan (Mesa) | `make docker-rocm` | 3.1 GB |

```
make docker                          # builds cipollino and cipollino:dev
make docker-test                     # GPU self-test and end-to-end tests in the container
./cipollino-docker -n 2 someprefix   # keys land in ./keys, owned by you

make docker-rocm                     # AMD: cipollino:rocm and cipollino:rocm-dev
CIPOLLINO_IMAGE=cipollino:rocm ./cipollino-docker -n 2 someprefix
```

Both images are also built by GitHub Actions (`.github/workflows/docker.yml`) and
published to the GitHub Container Registry, so nothing has to be built locally:

```
CIPOLLINO_IMAGE=ghcr.io/meow/cipollino-gen      ./cipollino-docker -n 2 someprefix   # CUDA, Vulkan
CIPOLLINO_IMAGE=ghcr.io/meow/cipollino-gen:rocm ./cipollino-docker -n 2 someprefix   # ROCm, Vulkan
```

`latest` and `rocm` follow the default branch; other branches publish `<branch>` and
`<branch>-rocm`.

`cipollino-docker` runs the image as you, with `./keys` mounted and every GPU it finds
passed through: `--gpus all` when Docker has the NVIDIA runtime, `/dev/dri` (Vulkan on
Mesa) and `/dev/kfd` (ROCm) when they exist. On an NVIDIA machine it amounts to

```
docker run --rm --init --gpus all --user "$(id -u):$(id -g)" -v "$PWD/keys:/keys:z" cipollino ARGS
```

It takes `CIPOLLINO_KEYS` (output directory), `CIPOLLINO_GPUS` (docker's `--gpus`
value) and `CIPOLLINO_IMAGE`. A prefix file for `-f` must be visible inside the
container: put it in the keys directory and pass `/keys/<name>`.

The container refuses to start a search whose output directory is not mounted from
outside, because a key written to the container's own filesystem disappears with it.

The CUDA code is built for every architecture nvcc knows (`-arch=all`) and the ROCm code
for the generic target of every GPU family, so the images are not tied to the build
machine's GPU. The `:dev`
images keep the toolchain and sources (`/src`) for rebuilding with other flags.

## Usage

```
cipollino [options] PREFIX [PREFIX...]

  -n, --count N       stop after N keys (default 1; 0 = run until interrupted)
  -d, --dir DIR       where to create the <address>.onion directories (default .)
  -f, --file FILE     read more prefixes from FILE, one per line
  -b, --backend NAME  auto (default), cuda, rocm, metal, vulkan or cpu
      --devices LIST  devices to use, numbered as the backend lists them, e.g. 0,2
                      (default: all GPUs)
      --threads T     kernel threads per device
      --pairs N       pairs per batch; a batch tests 2N+1 keys per thread
      --scratch MODE  auto, full, lean or half (see Tuning)
      --arith MODE    auto, wide or narrow (see Tuning)
      --filter MODE   auto or bitmap (see Tuning)
      --launch-ms MS  target duration of one kernel launch
      --bench [SECS]  measure throughput, write nothing
      --tune          try thread counts and scratch modes, print the fastest as flags
      --selftest      cross-check the backend against libsodium exhaustively, then exit
      --verify [DIR...]  check key directories instead of searching
  -q, --quiet         print only the addresses found
```

Prefixes use the onion alphabet (`a-z`, `2-7`); upper case and a trailing `.onion` are
accepted. Found addresses go to stdout, everything else to stderr. Exit status is 0 when
the requested number of keys was found, 130 when interrupted first.

The progress line ends in an estimate such as `1 key per 6.5 h (50% within 4.5 h, 95%
within 19.5 h)`: the average wait at the current rate, and the waits that half and 95% of
all searches stay under (0.69 and 3.0 times the average). The search has no memory, so
the estimate counts from now however long it has already run.

`--verify` checks key directories instead of searching, whoever wrote them: each `DIR`
given, or every `<address>.onion` inside it, and without arguments those inside the
`--dir` directory. A directory passes if its three files have Tor's headers and lengths,
the public key is the one the secret scalar gives, it is a canonical point of the
prime-order group, a signature made with the secret key verifies, and `hostname` is the
key's address. Modes that let other users in and a scalar that is not clamped are
reported as notes. The exit status is 1 if any directory fails. One thing no check can
see is damage to the second half of the secret key, the nonce prefix: nothing public
depends on it.

Every extra character multiplies the work by 32:

| prefix length | keys to test (average) | at 1.5 G keys/s (M5 Pro) | at 16.5 G keys/s (RTX 4090) |
|---|---|---|---|
| 6 | 1.1e9 | 0.7 s | instant |
| 7 | 3.4e10 | 23 s | 2.1 s |
| 8 | 1.1e12 | 12 min | 67 s |
| 9 | 3.5e13 | 6.5 h | 36 min |
| 10 | 1.1e15 | 8.7 days | 19 h |
| 11 | 3.6e16 | 9 months | 25 days |
| 12 | 1.2e18 | 24 years | 2.2 years |

A device records at most 4096 hits per launch and only counts the rest. For a very short
prefix (up to three or four characters, depending on the device) one launch holds more
than that, so such searches take their launches in pieces sized for half the buffer:
fewer batches, then some of the threads at a time. A piece that reports more hits than it could record all the same is undone (its
threads are put back where they started) and run again in two halves, so no hit is lost.

Searching for several prefixes at once divides the time to the *first* hit by the number
of (equally long) prefixes, as long as the rate holds. On the M5 Pro it does, up to
thousands of prefixes. How the rate on an RTX 4090 depends on the number of prefixes has
not been measured since the filter was reworked (see Tuning).

## Speed

Measured with `cipollino --bench`:

| device | backend | keys/s |
|---|---|---|
| NVIDIA RTX 4090 | CUDA | 16.6 G |
| NVIDIA RTX 4090 (also driving a desktop) | Vulkan | 14.5 - 15.3 G |
| Apple M5 Pro, first ~20 s | Metal, or Vulkan through MoltenVK | 1.7 G |
| Apple M5 Pro, sustained (laptop in "Automatic" power mode, heat-soaked) | Metal | 1.0 - 1.5 G |
| Apple M5 Pro, 18 cores | CPU fallback | 150 M |
| AMD Radeon 610M (the 2-CU iGPU of a Ryzen 9 9900X) | ROCm 6.4 | 105 M |
| AMD Radeon 610M | Vulkan (RADV, Mesa 25.2) | 90 M |

The 4090 and the Radeon were measured in the Docker images. With `auto`, a native API is
preferred over Vulkan (CUDA, ROCm, Metal, then Vulkan); they are never mixed, as that
would drive one GPU twice.

No discrete AMD or any Intel GPU has been tried.

### What limits the 4090

The card runs the search at its 450 W power limit, so what counts is energy per key, not
instructions per key. Measured at that limit: a 32x32->64 multiply-add costs about three
times an add (0.040 nJ against 0.013), and a byte of scratch traffic to memory about as
much as three multiply-adds (0.13 nJ). Of the roughly 55 nJ a pair of keys costs, about 38
are its ~950 wide multiplies, 12 the other 850 instructions and 5 to 8 the scratch
traffic.

That is why the obvious things do little there. With the scratch traffic removed
altogether (not a working configuration) the kernel reaches 17.7 G keys/s, which is the
ceiling for this algorithm on this card; cutting the scratch further costs more
multiplications than it saves memory. Fewer or longer threads, register limits and block
sizes all sit within a few percent. What did help, 15.2 to 16.6 G keys/s together:

- the scratch slot of the next pair is fetched one pair early, so its cache miss overlaps
  with arithmetic instead of stalling the thread;
- each thread's scratch slot is contiguous instead of spread over eight cache lines;
- the 32x32->64 products are spelled in PTX. Written in C, nvcc hoists the
  zero-extension of loop-invariant operands out of the loop and ptxas then emits a dead
  add after every product (92 per pair in the forward pass);
- twice as many threads per multiprocessor.

Raising the card's power limit (`nvidia-smi -pl`, 500 W at most on this one) is the
remaining knob; I did not try it.

**Use CUDA, not Vulkan, on an NVIDIA GPU that also runs a desktop.** The kernels are
equally fast (Vulkan reaches 16 G keys/s in short runs), but NVIDIA's driver cannot
cheaply preempt a running Vulkan compute dispatch: whenever the compositor wants to
draw, the dispatch in flight continues at about 1/17 of its speed until it ends, and one
that then needs more than several seconds gets the device reset (kernel log: `Xid 109,
CTX SWITCH TIMEOUT`). With 250 ms launches that meant 4.4 s stalls every ten seconds or
so, with 500 ms launches a lost device. The Vulkan backend therefore keeps launches on
NVIDIA to 50 ms, which bounds a stall to under a second; `--launch-ms` overrides that at
your own risk.

### AMD: ROCm against Vulkan

On NVIDIA the Vulkan kernel is as fast as the CUDA one. On the Radeon 610M it ran at 43 M
keys/s against ROCm's 96 M. Three things were behind that, found with loops of single
instructions on that GPU; with them fixed Vulkan reaches 90 M and ROCm 105 M.

- **Multiplies.** On this GPU (RDNA 2) a 32x32 multiply, either half of the product,
  takes as long as 4.5 additions; a multiply of two values below 2^24 takes as long as
  one, and can absorb an addition. ROCm's compiler turns `column += (u64)a * b` into one
  slow instruction (`v_mad_u64_u32`). Mesa's, which is what Vulkan means on Linux, needs
  two slow multiplies and a two-instruction carry for it, in Mesa 26.2 as in 25.2: 3.97
  ns per field multiplication against 1.67. So the Vulkan backend has a second
  multiplication (`src/kernel/field13.h`), which cuts every limb into two halves of 13
  bits: 400 fast multiply-adds into 32-bit columns and 200 instructions of splitting and
  carrying, 2.16 ns. With it a key pair takes about 6000 instructions, and at 90 M keys/s
  the GPU has no more to give (it runs 276 G simple instructions a second); ROCm's
  kernel is the shorter one, hence the rest of the gap.
- **Threads.** Vulkan has no way to ask how wide a GPU is and gave integrated ones 16384
  threads. This one is fastest at 1024 to 2048 and a third slower at 16384, so on
  integrated GPUs the count is now measured at start-up.
- **Scratch layout.** Keeping a thread's scratch slot contiguous instead of spreading it
  over eight places is never slower on this GPU and up to a third faster, depending on
  thread count and scratch mode, on both APIs. ROCm also does 5% better with batches of
  512 pairs than of 1024. Both are the defaults now.

The 13-bit multiplication is only for compilers that need it. An Apple M5 Pro runs it at
0.42 of the speed of the wide one, an NVIDIA GPU should need about twice the energy per
key (not measured), and under ROCm it is slower too (2.61 ns). There it is also wrong:
LLVM's AMD backend takes a sum of products of two values masked to 13 bits for a dot
product of bytes and emits `v_dot4_u32_u8` (`handleMulOperand` in `SIISelLowering.cpp`
accepts an operand whose second byte it could not analyse). ROCm 6.4's compiler and Mesa's
LLVM path with LLVM 20.1 both do it, and the code is unchanged in LLVM's main branch. So
ROCm keeps the wide multiplication, and on Vulkan the start-up self-test decides: a
multiplication whose kernel does not find the keys planted for it is dropped (`not using
the narrow arithmetic: ...`) and the other one used. That is what happens under
`RADV_DEBUG=llvm`. Which of the two AMD's own Windows driver wants is not known yet; both
are measured there.

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
4. **Scratch fetched a pair ahead** (CUDA). The backward pass reads back one stored
   field element per pair from gigabytes of scratch; it asks for the next pair's while
   working on the current one.
5. **A carry chain without the wrap-around pass**: the carry out of bit 255 is estimated
   (to within one) before the chain and folded in up front, so one pass leaves limbs that
   pack into 256 bits with shifts alone.
6. **Radix 2^25.5 with unsigned limbs**: every partial product is a 32x32->64
   multiply-add into a 64-bit column, which is the one thing both NVIDIA (`IMAD.WIDE`)
   and Apple GPUs do well. I measured 8x32 saturated, 5x51 and precomputed-multiple
   variants on the M5; all were slower.

That comes to 8 multiplications, one squaring and two 32-bit probes per pair in the
default `lean` mode: about 4.6 multiplication-equivalents per key (a squaring costs 0.57
of a multiplication on the M5, a probe 0.35), plus 1-2% for the batch inversion.

Multiple prefixes: up to four are compared directly against the 32 bits, five to twelve
the same way with all twelve slots of the launch parameters. Longer lists go through bit
tables: every key is looked up in an 8 KiB table on its first 16 bits, small enough to
stay in the cache next to the cores, and the one key in 65536 per prefix that passes is
looked up in two 2 MiB tables that cover all 32 bits. Survivors are compared against all
prefixes on 64 bits on the GPU and then in full on the CPU.

The kernel (`src/kernel/`) is written in the common subset of CUDA/HIP C++, the Metal
Shading Language, Vulkan GLSL and plain C++ (a handful of macros cover casts, buffers
and inlining), so one source is compiled by nvcc, hipcc, the Metal compiler,
glslang and the host compiler. The field multiplication exists twice, on 32-bit and on
24-bit multipliers (see "AMD: ROCm against Vulkan"); the Vulkan backend carries a kernel
for each.

## Security

**Where keys come from.** Every GPU thread gets its own 32-byte seed from the OS CSPRNG
(libsodium `randombytes`), expanded exactly like a normal Ed25519 key: SHA-512, clamp,
scalar `a` and nonce prefix. The key that is written is `a + 8m` for a public offset `m`
found by the search, with the seed's nonce prefix. The secret scalar is thus a uniformly
random clamped scalar, conditioned only on its public key having the prefix.

**One key per seed.** Two keys derived from one seed would have scalars a known distance
apart and share a nonce prefix, which links them, gives away all of them when one leaks,
and gives away both of any two that ever sign the same message.
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
  memory among those within 2% of it. On an RTX 4090 `lean` wins (16.6 G keys/s against
  15.0 for `half` and 13.9 for `full`, which saturates the memory bus), on Apple GPUs too.
- `--threads`: CUDA defaults to 2048 per multiprocessor (262144 on a 4090), ROCm to 1024
  per compute unit as the runtime counts them, Metal to 65536. Vulkan cannot ask a device
  how wide it is: it uses 131072 for discrete GPUs and measures 1024 to 16384 at start-up
  for integrated ones. More threads cost memory; too few leave the GPU idle.
- `--pairs`: bigger batches amortise the per-batch inversion (1.5% overhead at 1024, 3%
  at 512); memory is `threads * pairs * 32` bytes in `lean` mode.
- `--arith` chooses the field multiplication: `wide` builds 64-bit columns from 32x32
  products, `narrow` uses 13-bit half-limbs and 24-bit multiplies. Only the Vulkan
  backend has both. On AMD GPUs, and on GPUs nobody has tried, it measures the two at
  start-up; NVIDIA and Apple GPUs get `wide`.
- `--filter bitmap` sends even a short prefix list through the bit tables instead of the
  direct comparison, to compare the two with `--bench`. The direct comparison costs
  about 70 simple instructions per key pair with twelve slots and touches no memory; a
  table costs a memory access per key. An earlier version looked every key up in a 2 MiB
  table as soon as there were more than four prefixes, and ran at two thirds of its
  usual rate on an RTX 4090 that way; on the M5 Pro neither that nor the present filter
  costs anything measurable (5 000 prefixes: 1.6 G keys/s, was 1.16).
- The scratch layout is a build option per backend (`CUDA_AOS`, `ROCM_AOS`, `VULKAN_AOS`,
  1 = each thread's slot contiguous, 0 = one word per thread side by side). All use 1:
  it is 10% faster on the 4090 under CUDA in `lean` mode, up to a third faster on the
  Radeon 610M under ROCm and Vulkan, and makes no difference on the M5 Pro. The 4090
  under Vulkan has not been measured with it.

On a laptop the GPU is thermally limited within a minute; macOS's High Power mode, where
available, raises the sustained rate.

## Tests

```
make test          # everything this machine can run
make docker-test        # the GPU part inside the container: CUDA and Vulkan
make docker-rocm-test   # ROCm and Vulkan in the ROCm container
```

- `tests/test_field.py`: field arithmetic against Python big integers, including the
  worst-case limb values of every operand class and the documented output bounds, for
  both multiplications.
- `tools/gen_field13.py --check`: `field13.h` is what its generator writes, which also
  means the generator's bounds hold for it.
- `--selftest` on the CPU backend and every GPU backend in the build (with every
  multiplication the backend has), on the CUDA backend compiled against
  `tests/cuda_sim.h`, and on a CPU backend built with the narrow multiplication. One of
  its prefix lists matches half of all keys, which overflows the hit buffer and has to
  come out complete all the same.
- `tests/test_cli.py`: end-to-end runs, re-verified in Python, and `--verify` against
  intact and damaged key directories.

## Layout

```
src/kernel/defs.h      target abstraction (CUDA / Metal / CPU)
src/kernel/field.h     GF(2^255-19)
src/kernel/field13.h   its multiplication on 24-bit multipliers, written by tools/gen_field13.py
src/kernel/search.h    the per-thread search loop
src/kernel/entry_*     per-API kernel entry points (Metal, Vulkan)
src/backend_*.{cu,mm,cpp}   one per API; backend_cuda.cu is also the ROCm backend
src/engine.cpp         seeding, launches, hit validation, self-tests
src/keys.cpp           all secret-key handling (libsodium)
src/platform.cpp       file and console calls, for POSIX and Windows
src/ed.cpp             public-point helpers and the kernel table
src/filter.cpp         prefixes -> GPU filter tables
src/onion.cpp          SHA3-256, address encoding
Dockerfile, Dockerfile.rocm, docker/, cipollino-docker    container builds, entry-point guard, run wrapper
CMakeLists.txt, cmake/, vcpkg.json    the Windows build
```
