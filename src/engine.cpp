#include "engine.h"

#include <sodium.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <random>
#include <stdexcept>
#include <thread>
#include <tuple>

#include "ed.h"
#include "keys.h"
#include "onion.h"
#include "platform.h"

namespace {

std::atomic<bool> g_stop(false);

double now()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

template <class F>
void parallel_for(uint32_t count, F fn)
{
    unsigned n = std::min<unsigned>(std::max(1u, std::thread::hardware_concurrency()), std::max(1u, count / 64));
    std::atomic<uint32_t> next(0);
    auto body = [&]() {
        for (;;) {
            uint32_t begin = next.fetch_add(64);
            if (begin >= count) break;
            for (uint32_t i = begin; i < std::min(count, begin + 64); i++) fn(i);
        }
    };
    std::vector<std::thread> pool;
    for (unsigned i = 1; i < n; i++) pool.emplace_back(body);
    body();
    for (std::thread &t : pool) t.join();
}

bool hit_less(const Hit &a, const Hit &b)
{
    return std::tie(a.tid, a.batch, a.code) < std::tie(b.tid, b.batch, b.code);
}

bool hit_equal(const Hit &a, const Hit &b) { return a.tid == b.tid && a.batch == b.batch && a.code == b.code; }

// Offset (in units of 8*B) of a hit from its thread's seed scalar, given how many batches
// the thread had completed since it was seeded when the launch started.
keys::Offset hit_offset(const Hit &h, uint64_t batches_since_seed, uint32_t npairs)
{
    keys::Offset centre = (keys::Offset)(batches_since_seed + h.batch) * (2 * (keys::Offset)npairs + 1);
    keys::Offset i = h.code >> 1;
    return (h.code & 1) ? centre - i : centre + i;
}

bool hit_plausible(const Hit &h, uint32_t nthreads, uint32_t npairs, uint32_t nbatches)
{
    return h.tid < nthreads && h.batch < nbatches && h.code != 1 && (h.code >> 1) <= npairs;
}

std::string human(double v, const char *unit)
{
    static const char *suffix[] = {"", "k", "M", "G", "T", "P", "E"};
    int i = 0;
    while (v >= 1000 && i < 6) {
        v /= 1000;
        i++;
    }
    char buf[64];
    snprintf(buf, sizeof buf, v >= 100 || i == 0 ? "%.0f%s%s%s" : v >= 10 ? "%.1f%s%s%s" : "%.2f%s%s%s", v, suffix[i],
             *unit ? " " : "", unit);
    return buf;
}

std::string duration(double s)
{
    char buf[64];
    if (!(s < 1e15)) return "forever";
    if (s < 1) snprintf(buf, sizeof buf, "%.0f ms", s * 1e3);
    else if (s < 120) snprintf(buf, sizeof buf, "%.1f s", s);
    else if (s < 7200) snprintf(buf, sizeof buf, "%.1f min", s / 60);
    else if (s < 172800) snprintf(buf, sizeof buf, "%.1f h", s / 3600);
    else if (s < 730 * 86400.0) snprintf(buf, sizeof buf, "%.1f days", s / 86400);
    else snprintf(buf, sizeof buf, "%.3g years", s / (365.25 * 86400));
    return buf;
}

// Seeds for one device's kernel threads, in guarded, locked, wiped-on-free memory.
class SeedStore {
public:
    explicit SeedStore(uint32_t n) : p_((uint8_t *)sodium_allocarray(n, 32))
    {
        if (!p_) throw std::runtime_error("cannot allocate secure memory for seeds");
    }
    ~SeedStore() { sodium_free(p_); }
    SeedStore(const SeedStore &) = delete;
    SeedStore &operator=(const SeedStore &) = delete;
    uint8_t *operator[](uint32_t tid) { return p_ + (size_t)tid * 32; }

private:
    uint8_t *p_;
};

// Seed `count` threads starting at `first`: fresh CSPRNG seed, start point by libsodium,
// affine coordinates for the kernel.
std::vector<uint32_t> make_seeds(SeedStore &seeds, uint32_t first, uint32_t count)
{
    std::vector<uint32_t> words((size_t)count * CIP_STATE_WORDS);
    std::atomic<bool> failed(false);
    parallel_for(count, [&](uint32_t i) {
        uint8_t pk[32];
        ed::Affine p;
        keys::random_seed(seeds[first + i]);
        keys::start_point(pk, seeds[first + i]);
        if (!ed::decompress(p, pk)) {
            failed = true;
            return;
        }
        for (int k = 0; k < 10; k++) {
            words[(size_t)i * CIP_STATE_WORDS + k] = p.x.v[k];
            words[(size_t)i * CIP_STATE_WORDS + 10 + k] = p.y.v[k];
        }
    });
    if (failed) throw std::runtime_error("failed to decode a start point (this should be impossible)");
    return words;
}

void seed_threads(Backend &be, SeedStore &seeds, uint32_t first, uint32_t count)
{
    std::vector<uint32_t> words = make_seeds(seeds, first, count);
    be.write_state(first, count, words.data());
}

const char *scratch_name(uint32_t smode)
{
    return smode == CIP_SCRATCH_FULL ? "full" : smode == CIP_SCRATCH_LEAN ? "lean" : "half";
}

// Largest power-of-two batch size up to the backend's preference whose scratch fits its
// memory budget and the kernel's 32-bit scratch index.
uint32_t fit_pairs(const Backend &be, uint32_t nthreads, uint32_t smode)
{
    uint32_t n = be.suggested_pairs();
    while (n > 32 && (scratch_bytes(nthreads, n, smode) > be.scratch_budget() ||
                      scratch_bytes(nthreads, n, smode) / 4 > 0xffffffffull))
        n /= 2;
    return n;
}

void check_config(uint32_t nthreads, uint32_t npairs, uint32_t smode)
{
    if (nthreads == 0 || npairs == 0) throw std::runtime_error("threads and pairs must be positive");
    if (npairs > (1u << 20)) throw std::runtime_error("pairs per batch is too large");
    if (smode == CIP_SCRATCH_HALF && npairs % 2) throw std::runtime_error("--scratch half needs an even --pairs");
    if (scratch_bytes(nthreads, npairs, smode) / 4 > 0xffffffffull)
        throw std::runtime_error("threads * pairs is too large: the scratch buffer is limited to 16 GiB");
}

// Keys per second of a configuration, measured over about `seconds` of launches. Leaves
// the device configured that way with `start` (state words for `nthreads` threads) loaded
// and advanced by an unspecified number of batches.
double measure(Backend &be, uint32_t nthreads, uint32_t npairs, uint32_t smode, const Filter &filter,
               const std::vector<uint32_t> &start, double seconds)
{
    std::vector<Hit> hits;
    be.configure(nthreads, npairs, smode, ed::build_table(npairs));
    be.set_filter(filter);
    be.write_state(0, nthreads, start.data());
    be.launch(1, hits);  // first launch pays for page faults and lazy device set-up
    double t0 = now();
    be.launch(1, hits);
    double one = std::max(now() - t0, 1e-6);
    // Launches no longer than the backend likes them (some drivers punish long ones),
    // as many as it takes to fill the measuring time.
    uint32_t nb = (uint32_t)std::min(std::max(be.suggested_launch_seconds() / one, 1.0), 4096.0);
    uint64_t batches = 0;
    t0 = now();
    do {
        be.launch(nb, hits);
        batches += nb;
    } while (now() - t0 < seconds);
    return (double)nthreads * (2.0 * npairs + 1) * (double)batches / std::max(now() - t0, 1e-9);
}

struct Shared {
    explicit Shared(const Options &o) : opt(o) {}
    const Options &opt;
    Filter filter;
    double p_match = 0;
    bool tty = false;
    std::atomic<uint64_t> tested{0};
    std::atomic<uint64_t> found{0};
    std::atomic<uint64_t> mismatches{0};
    std::atomic<uint64_t> overflowed{0};
    std::atomic<bool> stop{false};
    std::atomic<bool> failed{false};
    std::mutex out_mu;
    double launch_rate = 0;  // sum over devices of keys per second of launch time

    void say(const std::string &line)
    {
        std::lock_guard<std::mutex> lock(out_mu);
        if (tty) fprintf(stderr, "\r\033[K");
        fprintf(stderr, "%s\n", line.c_str());
    }
};

class Worker {
public:
    Worker(Backend &be, Shared &sh, int index) : be_(be), sh_(sh), index_(index) {}

    void setup()
    {
        const Options &opt = sh_.opt;
        T_ = opt.threads ? opt.threads : be_.suggested_threads();
        if (T_ == 0) throw std::runtime_error("threads must be positive");
        seeds_.reset(new SeedStore(T_));
        base_.assign(T_, 0);
        std::vector<uint32_t> start = make_seeds(*seeds_, 0, T_);

        // Which scratch mode is fastest depends on how the device's arithmetic throughput
        // compares with its memory bandwidth, so unless told, measure the candidates.
        std::vector<uint32_t> modes = opt.scratch >= 0 ? std::vector<uint32_t>{(uint32_t)opt.scratch} : be_.scratch_candidates();
        struct Trial {
            uint32_t mode, pairs;
            double rate;
        };
        std::vector<Trial> trials;
        std::string report, problem;
        double best = -1;
        for (uint32_t mode : modes) {
            uint32_t n = opt.pairs ? opt.pairs : fit_pairs(be_, T_, mode);
            double rate = 0;
            try {
                check_config(T_, n, mode);
                if (modes.size() > 1) rate = measure(be_, T_, n, mode, sh_.filter, start, 0.2);
            } catch (const std::exception &e) {
                problem = e.what();  // e.g. this mode does not fit the device's memory
                continue;
            }
            if (modes.size() > 1) report += std::string(report.empty() ? "" : ", ") + scratch_name(mode) + " " + human(rate, "keys/s");
            trials.push_back(Trial{mode, n, rate});
            best = std::max(best, rate);
        }
        if (trials.empty()) throw std::runtime_error(problem);
        // Differences under 2% are measurement noise: among the modes that close to the
        // fastest, take the one that needs the least device memory.
        const Trial *pick = nullptr;
        for (const Trial &t : trials)
            if (t.rate >= 0.98 * best &&
                (!pick || scratch_bytes(T_, t.pairs, t.mode) < scratch_bytes(T_, pick->pairs, pick->mode)))
                pick = &t;
        smode_ = pick->mode;
        N_ = pick->pairs;
        if (modes.size() > 1 && !opt.quiet)
            sh_.say("device " + std::to_string(index_) + ": scratch modes: " + report + " -> " + scratch_name(smode_));

        be_.configure(T_, N_, smode_, ed::build_table(N_));
        be_.write_state(0, T_, start.data());

        // Planted needles: derive a handful of this device's candidate keys on the CPU,
        // search for exactly those, and require the GPU to report exactly them.
        std::string why;
        if (!needle_test(3, why) || !needle_test(24, why))
            throw std::runtime_error(be_.describe() + ": start-up self-test failed: " + why);
        be_.set_filter(sh_.filter);
        if (!opt.quiet) {
            char buf[256];
            snprintf(buf, sizeof buf, "device %d: %s | %u threads x %u keys/batch | %.0f MiB scratch (%s) | self-test ok",
                     index_, be_.describe().c_str(), T_, 2 * N_ + 1, scratch_bytes(T_, N_, smode_) / 1048576.0,
                     scratch_name(smode_));
            sh_.say(buf);
        }
    }

    void run()
    {
        try {
            loop();
        } catch (const std::exception &e) {
            sh_.say(std::string("error: ") + e.what());
            sh_.failed = true;
            sh_.stop = true;
        }
    }

private:
    bool needle_test(unsigned count, std::string &why)
    {
        const uint32_t nb = 2;
        std::mt19937_64 rng{std::random_device{}()};
        std::vector<Hit> want;
        want.push_back(Hit{0, 0, 0});                    // a centre
        want.push_back(Hit{T_ - 1, nb - 1, 2 * N_ + 1});  // centre - N*G8, after a centre move
        want.push_back(Hit{(uint32_t)(rng() % T_), nb - 1, 2 * N_});
        while (want.size() < count) {
            uint32_t i = (uint32_t)(rng() % (N_ + 1));
            want.push_back(Hit{(uint32_t)(rng() % T_), (uint32_t)(rng() % nb), i ? 2 * i + (uint32_t)(rng() & 1) : 0});
        }
        std::sort(want.begin(), want.end(), hit_less);
        want.erase(std::unique(want.begin(), want.end(), hit_equal), want.end());

        std::vector<std::string> needles;
        for (const Hit &h : want) {
            uint8_t pk[32];
            if (!keys::public_key(pk, (*seeds_)[h.tid], hit_offset(h, batches_ - base_[h.tid], N_))) {
                why = "could not derive a needle key";
                return false;
            }
            needles.push_back(onion::address(pk).substr(0, 12));
        }
        be_.set_filter(build_filter(needles));
        std::vector<Hit> got;
        uint32_t n = be_.launch(nb, got);
        batches_ += nb;
        std::sort(got.begin(), got.end(), hit_less);
        if (n != want.size() || got.size() != want.size() || !std::equal(got.begin(), got.end(), want.begin(), hit_equal)) {
            why = "planted " + std::to_string(want.size()) + " known keys, device reported " + std::to_string(n) +
                  (n == want.size() ? " (different ones)" : "");
            return false;
        }
        return true;
    }

    void loop()
    {
        const Options &opt = sh_.opt;
        const double target = opt.launch_seconds > 0 ? opt.launch_seconds : be_.suggested_launch_seconds();
        const uint64_t per_batch = (uint64_t)T_ * (2 * (uint64_t)N_ + 1);
        uint32_t nb = 1;
        std::vector<Hit> hits;
        std::vector<uint8_t> burned(T_, 0);
        std::vector<uint32_t> burned_list;
        double busy = 0;
        uint64_t batches_run = 0;

        while (!sh_.stop && !g_stop) {
            double t0 = now();
            uint32_t n = be_.launch(nb, hits);
            double dt = now() - t0;
            if (n > hits.size()) sh_.overflowed += n - hits.size();
            busy += dt;
            batches_run += nb;

            for (const Hit &h : hits) {
                if (sh_.stop) break;
                if (!hit_plausible(h, T_, N_, nb)) {
                    report_mismatch(h, "out-of-range hit record");
                    continue;
                }
                if (burned[h.tid]) continue;  // one key per seed, ever
                keys::Key key;
                std::string why;
                if (!keys::finalize(key, (*seeds_)[h.tid], hit_offset(h, batches_ - base_[h.tid], N_), opt.prefixes, why)) {
                    // A prefix longer than the 64 bits the GPU compares can pass the GPU
                    // filter and still fail here; anything else means the device computed
                    // something the CPU cannot reproduce.
                    if (key.address.empty() || !sh_.filter.matches(key.pk)) report_mismatch(h, why);
                    continue;
                }
                burned[h.tid] = 1;
                burned_list.push_back(h.tid);
                if (opt.bench_seconds > 0) continue;
                deliver(key);
            }
            batches_ += nb;
            sh_.tested += per_batch * nb;

            // Retire every seed that produced a key and start those threads afresh.
            for (uint32_t tid : burned_list) {
                seed_threads(be_, *seeds_, tid, 1);
                base_[tid] = batches_;
                burned[tid] = 0;
            }
            burned_list.clear();

            double ideal = nb * target / std::max(dt, 1e-6);
            nb = (uint32_t)std::lround(std::min(std::max(std::min(ideal, nb * 4.0), 1.0), 16384.0));
        }
        std::lock_guard<std::mutex> lock(sh_.out_mu);
        sh_.launch_rate += (double)per_batch * (double)batches_run / std::max(busy, 1e-9);
    }

    void deliver(const keys::Key &key)
    {
        std::lock_guard<std::mutex> lock(sh_.out_mu);
        const Options &opt = sh_.opt;
        if (opt.want && sh_.found >= opt.want) return;
        std::string err;
        if (!keys::write_tor_dir(opt.outdir, key, err)) {
            if (sh_.tty) fprintf(stderr, "\r\033[K");
            fprintf(stderr, "error: could not save %s.onion: %s\n", key.address.c_str(), err.c_str());
            sh_.failed = true;
            sh_.stop = true;
            return;
        }
        if (sh_.tty) fprintf(stderr, "\r\033[K");
        printf("%s.onion\n", key.address.c_str());
        fflush(stdout);
        if (++sh_.found == opt.want) sh_.stop = true;
    }

    void report_mismatch(const Hit &h, const std::string &why)
    {
        if (sh_.mismatches++ < 5) {
            char buf[512];
            snprintf(buf, sizeof buf,
                     "warning: device %d reported a hit the CPU could not confirm (thread %u, batch %u, code %u: %s); "
                     "it was discarded. Repeated warnings point at a faulty GPU or driver.",
                     index_, h.tid, h.batch, h.code, why.c_str());
            sh_.say(buf);
        }
    }

    Backend &be_;
    Shared &sh_;
    int index_;
    uint32_t T_ = 0, N_ = 0, smode_ = CIP_SCRATCH_LEAN;
    uint64_t batches_ = 0;                // batches every thread has run so far
    std::vector<uint64_t> base_;          // value of batches_ when each thread was last seeded
    std::unique_ptr<SeedStore> seeds_;
};

bool check_outdir(const std::string &dir, std::string &err)
{
    return plat::make_private_dir(dir, true, err) && plat::dir_writable(dir, err);
}

}  // namespace

void request_stop() { g_stop = true; }

int run_search(std::vector<std::unique_ptr<Backend>> &backends, const Options &opt)
{
    Shared sh(opt);
    sh.filter = build_filter(opt.prefixes);
    sh.p_match = match_probability(opt.prefixes);
    sh.tty = plat::stderr_is_terminal();
    const bool bench = opt.bench_seconds > 0;

    std::string err;
    if (!bench && !check_outdir(opt.outdir, err)) {
        fprintf(stderr, "error: output directory %s\n", err.c_str());
        return 1;
    }

    std::vector<std::unique_ptr<Worker>> workers;
    try {
        for (size_t i = 0; i < backends.size(); i++) {
            workers.emplace_back(new Worker(*backends[i], sh, (int)i));
            workers.back()->setup();
        }
    } catch (const std::exception &e) {
        fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }

    double start = now();
    std::vector<std::thread> threads;
    for (auto &w : workers) threads.emplace_back([&w]() { w->run(); });

    double last_print = start, last_time = start, rate = 0;
    uint64_t last_tested = 0;
    while (!sh.stop && !g_stop) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        double t = now();
        if (bench && t - start >= opt.bench_seconds) sh.stop = true;
        if (opt.quiet || t - last_print < 1.0) continue;
        uint64_t tested = sh.tested;
        double inst = (tested - last_tested) / (t - last_time);
        rate = rate == 0 ? inst : 0.7 * rate + 0.3 * inst;
        last_tested = tested;
        last_time = last_print = t;
        if (tested == 0) continue;
        char buf[512];
        double mean = 1.0 / (sh.p_match * std::max(rate, 1.0));
        snprintf(buf, sizeof buf, "%s | %s tested in %s | found %llu%s | 1 key per %s on average",
                 human(rate, "keys/s").c_str(), human((double)tested, "").c_str(), duration(t - start).c_str(),
                 (unsigned long long)sh.found.load(), opt.want ? ("/" + std::to_string(opt.want)).c_str() : "",
                 duration(mean).c_str());
        std::lock_guard<std::mutex> lock(sh.out_mu);
        if (sh.tty)
            fprintf(stderr, "\r\033[K%s", buf);
        else
            fprintf(stderr, "%s\n", buf);
        fflush(stderr);
    }
    sh.stop = true;
    for (std::thread &t : threads) t.join();
    double elapsed = now() - start;

    if (sh.tty && !opt.quiet) fprintf(stderr, "\r\033[K");
    if (!opt.quiet || bench) {
        // The benchmark reports time spent inside launches only, which is what tuning needs.
        double rate = bench ? sh.launch_rate : sh.tested / std::max(elapsed, 1e-9);
        fprintf(stderr, "%s%s tested in %s (%s)", bench ? "benchmark: " : "", human((double)sh.tested, "keys").c_str(),
                duration(elapsed).c_str(), human(rate, "keys/s").c_str());
        if (!bench) fprintf(stderr, ", %llu key%s found", (unsigned long long)sh.found.load(), sh.found == 1 ? "" : "s");
        fprintf(stderr, "\n");
    }
    if (sh.overflowed)
        fprintf(stderr, "note: %llu hits were dropped because more than %u arrived in one launch; "
                        "prefixes this short do not need a GPU.\n",
                (unsigned long long)sh.overflowed.load(), Backend::kHitCap);
    if (sh.mismatches)
        fprintf(stderr, "warning: %llu device hits failed CPU validation and were discarded.\n",
                (unsigned long long)sh.mismatches.load());
    if (sh.failed) return 1;
    if (bench || opt.want == 0 || sh.found >= opt.want) return 0;
    return 130;  // interrupted before finishing
}

// ---- tuning --------------------------------------------------------------------------

int run_tune(std::vector<std::unique_ptr<Backend>> &backends, const Options &opt)
{
    const Filter filter = build_filter({"cipollinotune"});
    for (size_t dev = 0; dev < backends.size(); dev++) {
        Backend &be = *backends[dev];
        uint32_t base = opt.threads ? opt.threads : be.suggested_threads();
        std::vector<uint32_t> counts;
        for (uint32_t t : {base / 2, base, base * 2, base * 4})
            if (t > 0) counts.push_back(t);
        printf("device %zu: %s\n%10s %8s %8s %12s %14s\n", dev, be.describe().c_str(), "threads", "pairs", "scratch",
               "memory", "keys/s");
        try {
            SeedStore seeds(counts.back());
            std::vector<uint32_t> start = make_seeds(seeds, 0, counts.back());
            double best = 0;
            uint32_t best_t = 0, best_n = 0, best_mode = 0;
            for (uint32_t t : counts)
                for (uint32_t mode : {CIP_SCRATCH_FULL, CIP_SCRATCH_LEAN, CIP_SCRATCH_HALF}) {
                    if (g_stop) return 130;
                    uint32_t n = opt.pairs ? opt.pairs : fit_pairs(be, t, mode);
                    double rate = 0;
                    std::string note;
                    try {
                        check_config(t, n, mode);
                        rate = measure(be, t, n, mode, filter, start, 1.0);
                    } catch (const std::exception &e) {
                        note = e.what();
                    }
                    printf("%10u %8u %8s %8.0f MiB %14s %s\n", t, n, scratch_name(mode),
                           scratch_bytes(t, n, mode) / 1048576.0, rate > 0 ? human(rate, "").c_str() : "-", note.c_str());
                    fflush(stdout);
                    if (rate > best) {
                        best = rate;
                        best_t = t;
                        best_n = n;
                        best_mode = mode;
                    }
                }
            if (best > 0)
                printf("fastest: --threads %u --pairs %u --scratch %s   (%s)\n\n", best_t, best_n, scratch_name(best_mode),
                       human(best, "keys/s").c_str());
        } catch (const std::exception &e) {
            fprintf(stderr, "error: %s\n", e.what());
            return 1;
        }
    }
    return 0;
}

// ---- exhaustive self-test ------------------------------------------------------------

namespace {

struct Candidate {
    Hit pos;
    uint8_t pk[32];
    std::string address;
};

bool exhaustive_case(Backend &be, uint32_t T, uint32_t N, uint32_t nb, uint32_t smode, std::string &log)
{
    char tag[96];
    snprintf(tag, sizeof tag, "T=%u N=%u batches=%u scratch=%s", T, N, nb, scratch_name(smode));
    be.configure(T, N, smode, ed::build_table(N));
    SeedStore seeds(T);
    seed_threads(be, seeds, 0, T);
    std::vector<uint32_t> initial((size_t)T * CIP_STATE_WORDS);
    be.read_state(0, T, initial.data());

    // Every key the kernel will look at, straight from the secret scalars via libsodium.
    std::vector<Candidate> all;
    for (uint32_t t = 0; t < T; t++)
        for (uint32_t b = 0; b < nb; b++)
            for (uint32_t code = 0; code <= 2 * N + 1; code++) {
                if (code == 1) continue;
                Candidate c;
                c.pos = Hit{t, b, code};
                all.push_back(c);
            }
    std::atomic<bool> bad(false);
    parallel_for((uint32_t)all.size(), [&](uint32_t i) {
        Candidate &c = all[i];
        if (!keys::public_key(c.pk, seeds[c.pos.tid], hit_offset(c.pos, 0, N))) bad = true;
        c.address = onion::address(c.pk);
    });
    if (bad) {
        log += std::string(tag) + ": could not derive reference keys\n";
        return false;
    }

    std::mt19937_64 rng(0x5eed + T * 131 + N);
    auto random_prefix = [&](size_t len) {
        std::string s;
        for (size_t i = 0; i < len; i++) s += "abcdefghijklmnopqrstuvwxyz234567"[rng() % 32];
        return s;
    };
    auto stolen = [&](size_t len) { return all[rng() % all.size()].address.substr(0, len); };

    std::vector<std::vector<std::string>> lists;
    lists.push_back({random_prefix(1)});
    lists.push_back({stolen(7)});
    lists.push_back({random_prefix(1), random_prefix(2), stolen(5), stolen(12)});
    lists.push_back({stolen(13), stolen(20), stolen(56)});  // beyond the GPU's 64 bits
    std::vector<std::string> many;
    for (int i = 0; i < 6; i++) many.push_back(random_prefix(2));
    for (int i = 0; i < 40; i++) many.push_back(random_prefix(3));
    for (size_t len = 4; len <= 16; len++) many.push_back(stolen(len));
    lists.push_back(many);
    std::vector<std::string> wide;  // short prefixes exercise the bitmap expansion
    for (int i = 0; i < 5; i++) wide.push_back(random_prefix(1));
    wide.push_back(stolen(4));
    lists.push_back(wide);

    for (size_t li = 0; li < lists.size(); li++) {
        const std::vector<std::string> &prefixes = lists[li];
        Filter f = build_filter(prefixes);
        std::vector<Hit> want;
        for (const Candidate &c : all) {
            bool text = false;
            for (const std::string &p : prefixes)
                // The kernel contract is the first 64 bits; 12 characters fit inside them.
                if (c.address.compare(0, std::min<size_t>(p.size(), 12), p, 0, std::min<size_t>(p.size(), 12)) == 0)
                    text = true;
            bool kernel = f.matches(c.pk);
            if (kernel && !text) {
                log += std::string(tag) + ": filter matches a key no prefix matches\n";
                return false;
            }
            if (text && !kernel) {
                // Only legitimate for prefixes of 13+ characters: bits 60..63 also count.
                bool longer = false;
                for (const std::string &p : prefixes)
                    if (p.size() > 12 && c.address.compare(0, 12, p, 0, 12) == 0) longer = true;
                if (!longer) {
                    log += std::string(tag) + ": filter misses " + c.address + "\n";
                    return false;
                }
            }
            if (kernel) want.push_back(c.pos);
        }
        std::sort(want.begin(), want.end(), hit_less);

        be.write_state(0, T, initial.data());
        be.set_filter(f);
        std::vector<Hit> got;
        uint32_t n = be.launch(nb, got);
        std::sort(got.begin(), got.end(), hit_less);
        if (n != want.size() || got.size() != want.size() || !std::equal(got.begin(), got.end(), want.begin(), hit_equal)) {
            char buf[256];
            snprintf(buf, sizeof buf, "%s: prefix list %zu (%s mode): expected %zu hits, device reported %u%s\n", tag, li,
                     f.mode == CIP_MODE_DIRECT ? "direct" : "bitmap", want.size(), n,
                     n == want.size() ? " but different ones" : "");
            log += buf;
            return false;
        }
        if (want.empty() && li != 0) {  // every list but the first contains a prefix taken from a real candidate
            log += std::string(tag) + ": test produced no hits to compare (bug in the self-test)\n";
            return false;
        }
    }

    // After nb batches each centre must be seed + nb*(2N+1) steps.
    std::vector<uint32_t> final_state((size_t)T * CIP_STATE_WORDS);
    be.read_state(0, T, final_state.data());
    for (uint32_t t = 0; t < T; t++) {
        uint8_t pk[32], got[32];
        ed::Affine p, q;
        if (!keys::public_key(pk, seeds[t], hit_offset(Hit{t, nb, 0}, 0, N)) || !ed::decompress(p, pk)) return false;
        for (int k = 0; k < 10; k++) {
            q.x.v[k] = final_state[(size_t)t * CIP_STATE_WORDS + k];
            q.y.v[k] = final_state[(size_t)t * CIP_STATE_WORDS + 10 + k];
        }
        ed::compress(got, q);
        if (memcmp(got, pk, 32) != 0) {
            log += std::string(tag) + ": centre of thread " + std::to_string(t) + " is wrong after the launch\n";
            return false;
        }
    }
    log += std::string(tag) + ": ok (" + std::to_string(all.size()) + " keys, " + std::to_string(lists.size()) + " prefix lists)\n";
    return true;
}

}  // namespace

bool selftest_exhaustive(Backend &be, std::string &log)
{
    for (uint32_t smode : {CIP_SCRATCH_FULL, CIP_SCRATCH_LEAN, CIP_SCRATCH_HALF}) {
        uint32_t odd = smode == CIP_SCRATCH_HALF ? 1 : 0;  // half mode needs an even batch size
        if (!exhaustive_case(be, 5, 1 + odd, 3, smode, log) || !exhaustive_case(be, 33, 7 + odd, 2, smode, log) ||
            !exhaustive_case(be, 64, 48, 3, smode, log) || !exhaustive_case(be, 300, 16, 1, smode, log))
            return false;
    }
    return true;
}
