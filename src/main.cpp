// cipollino: GPU vanity address generator for Tor v3 onion services.
#include "backend.h"
#include "ed.h"
#include "engine.h"
#include "keys.h"
#include "onion.h"
#include "platform.h"

#include <algorithm>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sodium.h>
#include <thread>

// The build passes the contents of the VERSION file as a bare token.
#ifndef CIP_VERSION
#define CIP_VERSION unknown
#endif
#define CIP_STRING2(x) #x
#define CIP_STRING(x)  CIP_STRING2(x)

static void usage(FILE *out) {
  fputs(
    "usage: cipollino [options] PREFIX [PREFIX...]\n"
    "\n"
    "Searches for Tor v3 onion addresses that start with any of the given prefixes and\n"
    "writes each one as a ready-to-use HiddenServiceDir: <dir>/<address>.onion/\n"
    "In a prefix, ?d stands for a digit (2-7), ?l for a letter and ?a for either.\n"
    "\n"
    "  -n, --count N       stop after N keys (default 1; 0 = run until interrupted)\n"
    "  -d, --dir DIR       where to create the <address>.onion directories (default .)\n"
    "  -f, --file FILE     read more prefixes from FILE, one per line\n"
    "      --client        search for client authorization keys instead: x25519 key pairs\n"
    "                      whose public key in base32 starts with a prefix. Writes\n"
    "                      <dir>/<KEY>.auth for the service and <dir>/<KEY>.private\n"
    "      --service ADDR  with --client: the onion address the keys are for. Writes\n"
    "                      <dir>/<KEY>.auth_private, ready for ClientOnionAuthDir, in\n"
    "                      place of <KEY>.private\n"
    "  -b, --backend NAME  auto (default), cuda, rocm, metal, vulkan or cpu\n"
    "      --devices LIST  devices to use, numbered as the backend lists them, e.g. 0,2\n"
    "                      (default: all GPUs)\n"
    "      --threads T     kernel threads per device (default: per device)\n"
    "      --pairs N       pairs per batch; a batch tests 2N+1 keys per thread\n"
    "                      (default: as large as the device's memory comfortably allows)\n"
    "      --scratch MODE  auto (default: measure at start-up), full, lean or half: trade\n"
    "                      device memory traffic (64/32/16 bytes per key pair) against\n"
    "                      a little arithmetic\n"
    "      --arith MODE    auto (default), wide or narrow: field multiplication on 32-bit\n"
    "                      or on 24-bit multipliers. Vulkan has both and measures them\n"
    "                      where it cannot tell (AMD GPUs are far faster with narrow)\n"
    "      --filter MODE   auto (default) or bitmap. Up to 12 prefixes are compared with\n"
    "                      each key directly; longer lists, or all with `bitmap`, go\n"
    "                      through bit tables in device memory\n"
    "      --launch-ms MS  target duration of one kernel launch\n"
    "      --bench [SECS]  measure throughput for SECS seconds (default 10), write nothing\n"
    "      --tune          try thread counts and scratch modes, print the fastest as flags\n"
    "      --selftest      cross-check the backend against libsodium exhaustively, then exit\n"
    "      --verify [DIR...]  check key directories instead of searching: each DIR, or every\n"
    "                      <address>.onion inside it (default: inside the --dir directory)\n"
    "  -q, --quiet         print only the addresses found\n"
    "  -V, --version\n"
    "  -h, --help\n",
    out
  );
}

static void on_signal(int) { request_stop(); }

// Known answers for the pieces libsodium does not cover (SHA3-256, address encoding).
static bool known_answers() {
  static const uint8_t sha3_abc[32] = { 0x3a, 0x98, 0x5d, 0xa7, 0x4f, 0xe2, 0x25, 0xb2, 0x04, 0x5c, 0x17,
                                        0x2d, 0x6b, 0xd3, 0x90, 0xbd, 0x85, 0x5f, 0x08, 0x6e, 0x3e, 0x9d,
                                        0x52, 0x5b, 0x46, 0xbf, 0xe2, 0x45, 0x11, 0x43, 0x15, 0x32 };
  uint8_t digest[32], pk[32];
  onion::sha3_256(digest, (const uint8_t *)"abc", 3);
  if (memcmp(digest, sha3_abc, 32) != 0)
    return false;
  for (int i = 0; i < 32; i++)
    pk[i] = (uint8_t)i;
  return onion::address(pk) == "aaaqeayeaudaocajbifqydiob4ibceqtcqkrmfyydenbwha5dyp3kead";
}

// Check key directories: every argument is one, or holds some. Returns a process exit code.
static int run_verify(std::vector<std::string> dirs, const std::string &outdir) {
  if (dirs.empty())
    dirs.push_back(outdir);
  std::vector<std::string> found;
  for (const std::string &d : dirs) {
    if (plat::exists(d + "/hs_ed25519_secret_key")) {
      found.push_back(d);
      continue;
    }
    std::vector<std::string> names;
    std::string err;
    if (!plat::list_subdirs(d, names, err)) {
      fprintf(stderr, "error: %s\n", err.c_str());
      return 1;
    }
    size_t before = found.size();
    for (const std::string &n : names)
      if (n.size() > 6 && n.compare(n.size() - 6, 6, ".onion") == 0)
        found.push_back(d + "/" + n);
    if (found.size() == before) {
      fprintf(stderr, "error: %s is not a key directory and holds none\n", d.c_str());
      return 1;
    }
  }
  size_t bad = 0;
  for (const std::string &d : found) {
    std::string address, err;
    std::vector<std::string> notes;
    if (keys::verify_tor_dir(d, address, notes, err)) {
      // A directory named after another address is not wrong for Tor, but is for its owner.
      size_t slash     = d.find_last_of("/\\");
      std::string name = slash == std::string::npos ? d : d.substr(slash + 1);
      if (name.size() == 62 && name != address + ".onion")
        notes.push_back("the directory is named after another address");
      printf("ok    %s.onion  %s\n", address.c_str(), d.c_str());
      for (const std::string &n : notes)
        printf("      note: %s\n", n.c_str());
    } else {
      printf("FAIL  %s: %s\n", d.c_str(), err.c_str());
      bad++;
    }
  }
  fflush(stdout);
  if (found.size() > 1 || bad)
    fprintf(stderr, "%zu of %zu key directories are good\n", found.size() - bad, found.size());
  return bad ? 1 : 0;
}

static bool parse_u64(const char *s, uint64_t &v) {
  char *end = nullptr;
  if (!s || !*s || *s == '-')
    return false;
  v = strtoull(s, &end, 10);
  return *end == 0;
}

int main(int argc, char **argv) {
  Options opt;
  std::string backend = "auto";
  std::vector<int> devices;
  std::vector<std::string> raw;
  bool selftest = false, tune = false, verify = false;

  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    auto value    = [&](uint64_t &v) {
      if (i + 1 >= argc || !parse_u64(argv[++i], v)) {
        fprintf(stderr, "error: %s needs a non-negative integer\n", a.c_str());
        exit(2);
      }
    };
    auto text = [&]() -> std::string {
      if (i + 1 >= argc) {
        fprintf(stderr, "error: %s needs a value\n", a.c_str());
        exit(2);
      }
      return argv[++i];
    };
    uint64_t v = 0;
    if (a == "-h" || a == "--help") {
      usage(stdout);
      return 0;
    } else if (a == "-V" || a == "--version") {
      puts("cipollino " CIP_STRING(CIP_VERSION));
      return 0;
    } else if (a == "-n" || a == "--count") {
      value(opt.want);
    } else if (a == "-d" || a == "--dir") {
      opt.outdir = text();
    } else if (a == "--client") {
      opt.client = true;
    } else if (a == "--service") {
      opt.service = text();
      if (!onion::normalize_address(opt.service)) {
        fprintf(stderr, "error: --service needs a v3 onion address (56 characters)\n");
        return 2;
      }
    } else if (a == "-f" || a == "--file") {
      std::string path = text();
      std::ifstream in(path);
      if (!in) {
        fprintf(stderr, "error: cannot read %s\n", path.c_str());
        return 2;
      }
      for (std::string line; std::getline(in, line);) {
        size_t b = line.find_first_not_of(" \t\r"), e = line.find_last_not_of(" \t\r");
        if (b == std::string::npos || line[b] == '#')
          continue;
        raw.push_back(line.substr(b, e - b + 1));
      }
    } else if (a == "-b" || a == "--backend") {
      backend = text();
    } else if (a == "--devices") {
      std::string list = text();
      for (size_t pos = 0; pos <= list.size();) {
        size_t comma = std::min(list.find(',', pos), list.size());
        uint64_t d;
        if (!parse_u64(list.substr(pos, comma - pos).c_str(), d)) {
          fprintf(stderr, "error: --devices wants a comma-separated list of indices\n");
          return 2;
        }
        devices.push_back((int)d);
        pos = comma + 1;
      }
    } else if (a == "--threads") {
      value(v);
      opt.threads = (uint32_t)std::min<uint64_t>(v, 1u << 30);
    } else if (a == "--pairs") {
      value(v);
      opt.pairs = (uint32_t)std::min<uint64_t>(v, 1u << 30);
    } else if (a == "--scratch") {
      std::string m = text();
      opt.scratch   = m == "full" ? 2 : m == "lean" ? 1 : m == "half" ? 0 : -1;
      if (opt.scratch < 0 && m != "auto") {
        fprintf(stderr, "error: --scratch wants auto, full, lean or half\n");
        return 2;
      }
    } else if (a == "--arith") {
      std::string m = text();
      opt.arith     = m == "wide" ? 0 : m == "narrow" ? 1 : -1;
      if (opt.arith < 0 && m != "auto") {
        fprintf(stderr, "error: --arith wants auto, wide or narrow\n");
        return 2;
      }
    } else if (a == "--filter") {
      std::string m     = text();
      opt.filter_tables = m == "bitmap";
      if (!opt.filter_tables && m != "auto") {
        fprintf(stderr, "error: --filter wants auto or bitmap\n");
        return 2;
      }
    } else if (a == "--launch-ms") {
      value(v);
      opt.launch_seconds = v / 1000.0;
    } else if (a == "--bench") {
      opt.bench_seconds = 10;
      if (i + 1 < argc && parse_u64(argv[i + 1], v)) {
        opt.bench_seconds = std::max<double>(1, (double)v);
        i++;
      }
    } else if (a == "--selftest") {
      selftest = true;
    } else if (a == "--tune") {
      tune = true;
    } else if (a == "--verify") {
      verify = true;
    } else if (a == "-q" || a == "--quiet") {
      opt.quiet = true;
    } else if (a.size() > 1 && a[0] == '-') {
      fprintf(stderr, "error: unknown option %s\n\n", a.c_str());
      usage(stderr);
      return 2;
    } else {
      raw.push_back(a);
    }
  }

  if (opt.client && (verify || opt.scratch >= 0)) {
    fprintf(stderr, "error: --client does not go with %s\n", verify ? "--verify" : "--scratch");
    return 2;
  }
  if (!opt.client && !opt.service.empty()) {
    fprintf(stderr, "error: --service needs --client\n");
    return 2;
  }

  if (verify) {
    if (sodium_init() < 0 || !known_answers()) {
      fprintf(stderr, "error: libsodium or the address encoding failed its start-up check\n");
      return 1;
    }
    return run_verify(raw, opt.outdir);
  }

  for (std::string &p : raw) {
    std::string err;
    if (!onion::normalize_prefix(p, err, opt.client)) {
      fprintf(stderr, "error: %s\n", err.c_str());
      return 2;
    }
    if (std::find(opt.prefixes.begin(), opt.prefixes.end(), p) == opt.prefixes.end())
      opt.prefixes.push_back(p);
  }
  if (opt.prefixes.empty() && opt.bench_seconds > 0)
    opt.prefixes.push_back("cipollinobench");
  if (opt.prefixes.empty() && !selftest && !tune) {
    usage(stderr);
    return 2;
  }

  if (sodium_init() < 0) {
    fprintf(stderr, "error: libsodium failed to initialise\n");
    return 1;
  }
  try {
    ed::init();
  } catch (const std::exception &e) {
    fprintf(stderr, "error: %s\n", e.what());
    return 1;
  }
  if (!known_answers()) {
    fprintf(stderr, "error: SHA3 / address encoding known-answer test failed; refusing to run\n");
    return 1;
  }

  std::vector<std::unique_ptr<Backend>> backends;

  // Native APIs first, then Vulkan (which reaches every vendor), then the CPU. With
  // "auto" the first backend that finds a device wins; devices are not mixed across
  // backends, as one GPU would then be driven twice.
  struct Choice {
    const char *name;
    std::vector<std::unique_ptr<Backend>> (*create)(const std::vector<int> &, std::string &);
  };

  static const Choice choices[] = {
    { "cuda", create_cuda_backends },
    { "rocm", create_rocm_backends },
    { "metal",
      [](const std::vector<int> &, std::string &e) {
        return create_metal_backends(e);
      } },
    { "vulkan", create_vulkan_backends },
  };
  std::string why;
  bool known = backend == "auto" || backend == "cpu";
  for (const Choice &c : choices) {
    if (backend == c.name)
      known = true;
    if (!backends.empty() || (backend != "auto" && backend != c.name))
      continue;
    std::string e;
    backends = c.create(devices, e);
    if (backends.empty())
      why += std::string(why.empty() ? "" : "; ") + c.name + ": " + e;
  }
  if (!known) {
    fprintf(stderr, "error: unknown backend '%s' (auto, cuda, rocm, metal, vulkan, cpu)\n", backend.c_str());
    return 2;
  }
  if (backends.empty() && backend == "auto")
    fprintf(stderr, "note: no GPU backend available (%s); falling back to the CPU, which is slow.\n", why.c_str());
  if (backends.empty() && (backend == "auto" || backend == "cpu"))
    backends = create_cpu_backends(std::thread::hardware_concurrency());
  if (backends.empty()) {
    fprintf(stderr, "error: %s\n", why.c_str());
    return 1;
  }

  if (selftest) {
    bool ok = true;
    for (auto &be : backends) {
      std::string log;
      bool pass;
      try {
        pass = selftest_exhaustive(*be, log);
      } catch (const std::exception &e) {
        log += std::string("exception: ") + e.what() + "\n";
        pass = false;
      }
      printf("%s\n%s%s\n", be->describe().c_str(), log.c_str(), pass ? "self-test passed" : "SELF-TEST FAILED");
      ok = ok && pass;
    }
    return ok ? 0 : 1;
  }

  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);
  if (tune)
    return run_tune(backends, opt);
  return run_search(backends, opt);
}
