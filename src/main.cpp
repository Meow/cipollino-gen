// cipollino: GPU vanity address generator for Tor v3 onion services.
#include <sodium.h>

#include <algorithm>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <thread>

#include "backend.h"
#include "ed.h"
#include "engine.h"
#include "onion.h"

static void usage(FILE *out)
{
    fputs(
        "usage: cipollino [options] PREFIX [PREFIX...]\n"
        "\n"
        "Searches for Tor v3 onion addresses that start with any of the given prefixes and\n"
        "writes each one as a ready-to-use HiddenServiceDir: <dir>/<address>.onion/\n"
        "\n"
        "  -n, --count N       stop after N keys (default 1; 0 = run until interrupted)\n"
        "  -d, --dir DIR       where to create the <address>.onion directories (default .)\n"
        "  -f, --file FILE     read more prefixes from FILE, one per line\n"
        "  -b, --backend NAME  auto (default), cuda, metal or cpu\n"
        "      --devices LIST  CUDA devices to use, e.g. 0,2 (default: all)\n"
        "      --threads T     kernel threads per device (default: per device)\n"
        "      --pairs N       pairs per batch; a batch tests 2N+1 keys per thread\n"
        "                      (default: as large as the device's memory comfortably allows)\n"
        "      --scratch MODE  auto (default: measure at start-up), full, lean or half: trade\n"
        "                      device memory traffic (64/32/16 bytes per key pair) against\n"
        "                      a little arithmetic\n"
        "      --launch-ms MS  target duration of one kernel launch\n"
        "      --bench [SECS]  measure throughput for SECS seconds (default 10), write nothing\n"
        "      --tune          try thread counts and scratch modes, print the fastest as flags\n"
        "      --selftest      cross-check the backend against libsodium exhaustively, then exit\n"
        "  -q, --quiet         print only the addresses found\n"
        "  -h, --help\n",
        out);
}

static void on_signal(int) { request_stop(); }

// Known answers for the pieces libsodium does not cover (SHA3-256, address encoding).
static bool known_answers()
{
    static const uint8_t sha3_abc[32] = {0x3a, 0x98, 0x5d, 0xa7, 0x4f, 0xe2, 0x25, 0xb2, 0x04, 0x5c, 0x17,
                                         0x2d, 0x6b, 0xd3, 0x90, 0xbd, 0x85, 0x5f, 0x08, 0x6e, 0x3e, 0x9d,
                                         0x52, 0x5b, 0x46, 0xbf, 0xe2, 0x45, 0x11, 0x43, 0x15, 0x32};
    uint8_t digest[32], pk[32];
    onion::sha3_256(digest, (const uint8_t *)"abc", 3);
    if (memcmp(digest, sha3_abc, 32) != 0) return false;
    for (int i = 0; i < 32; i++) pk[i] = (uint8_t)i;
    return onion::address(pk) == "aaaqeayeaudaocajbifqydiob4ibceqtcqkrmfyydenbwha5dyp3kead";
}

static bool parse_u64(const char *s, uint64_t &v)
{
    char *end = nullptr;
    if (!s || !*s || *s == '-') return false;
    v = strtoull(s, &end, 10);
    return *end == 0;
}

int main(int argc, char **argv)
{
    Options opt;
    std::string backend = "auto";
    std::vector<int> devices;
    std::vector<std::string> raw;
    bool selftest = false, tune = false;

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto value = [&](uint64_t &v) {
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
        } else if (a == "-n" || a == "--count") {
            value(opt.want);
        } else if (a == "-d" || a == "--dir") {
            opt.outdir = text();
        } else if (a == "-f" || a == "--file") {
            std::string path = text();
            std::ifstream in(path);
            if (!in) {
                fprintf(stderr, "error: cannot read %s\n", path.c_str());
                return 2;
            }
            for (std::string line; std::getline(in, line);) {
                size_t b = line.find_first_not_of(" \t\r"), e = line.find_last_not_of(" \t\r");
                if (b == std::string::npos || line[b] == '#') continue;
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
            opt.scratch = m == "full" ? 2 : m == "lean" ? 1 : m == "half" ? 0 : -1;
            if (opt.scratch < 0 && m != "auto") {
                fprintf(stderr, "error: --scratch wants auto, full, lean or half\n");
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

    for (std::string &p : raw) {
        std::string err;
        if (!onion::normalize_prefix(p, err)) {
            fprintf(stderr, "error: %s\n", err.c_str());
            return 2;
        }
        if (std::find(opt.prefixes.begin(), opt.prefixes.end(), p) == opt.prefixes.end()) opt.prefixes.push_back(p);
    }
    if (opt.prefixes.empty() && opt.bench_seconds > 0) opt.prefixes.push_back("cipollinobench");
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
    std::string err, cuda_err, metal_err;
    if (backend == "auto" || backend == "cuda") backends = create_cuda_backends(devices, cuda_err);
    if (backends.empty() && (backend == "auto" || backend == "metal")) backends = create_metal_backends(metal_err);
    if (backends.empty() && backend == "auto") {
        fprintf(stderr, "note: no GPU backend available (cuda: %s; metal: %s); falling back to the CPU, which is slow.\n",
                cuda_err.c_str(), metal_err.c_str());
    }
    if (backends.empty() && (backend == "auto" || backend == "cpu"))
        backends = create_cpu_backends(std::thread::hardware_concurrency());
    if (backends.empty()) {
        err = backend == "cuda" ? cuda_err : backend == "metal" ? metal_err : "unknown backend '" + backend + "'";
        fprintf(stderr, "error: %s\n", err.c_str());
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
    if (tune) return run_tune(backends, opt);
    return run_search(backends, opt);
}
