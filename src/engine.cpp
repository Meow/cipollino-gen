#include "engine.h"

#include <sodium.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
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

// The search is for onion addresses or, with `client`, for x25519 keys. The two differ in
// the form of the curve the kernel walks and in the key a candidate scalar stands for.

std::vector<uint32_t> kernel_table(uint32_t npairs, bool client)
{
    return client ? ed::build_table_x25519(npairs) : ed::build_table(npairs);
}

// A thread's centre as the kernel takes it, from the Ed25519 public key of its scalar.
bool centre_words(uint32_t *words, const uint8_t pk[32], bool client)
{
    ed::Affine p;
    if (!ed::decompress(p, pk)) return false;
    fe a = p.x, b = p.y;
    if (client) {
        ed::Montgomery m = ed::to_montgomery(p);
        a = m.u;
        b = m.v;
    }
    for (int k = 0; k < 10; k++) {
        words[k] = a.v[k];
        words[10 + k] = b.v[k];
    }
    return true;
}

// The key of seed + offset, as the 32 bytes the filter looks at.
bool candidate_key(uint8_t key[32], const uint8_t seed[32], keys::Offset m, bool client)
{
    return client ? keys::client_public_key(key, seed, m) : keys::public_key(key, seed, m);
}

// What prefixes are matched against.
std::string key_text(const uint8_t key[32], bool client)
{
    return client ? onion::base32(key, 32) : onion::address(key);
}

// Seed `count` threads starting at `first`: fresh CSPRNG seed, start point by libsodium,
// affine coordinates for the kernel.
std::vector<uint32_t> make_seeds(SeedStore &seeds, uint32_t first, uint32_t count, bool client)
{
    std::vector<uint32_t> words((size_t)count * CIP_STATE_WORDS);
    std::atomic<bool> failed(false);
    parallel_for(count, [&](uint32_t i) {
        uint8_t pk[32];
        keys::random_seed(seeds[first + i]);
        keys::start_point(pk, seeds[first + i]);
        if (!centre_words(&words[(size_t)i * CIP_STATE_WORDS], pk, client)) failed = true;
    });
    if (failed) throw std::runtime_error("failed to decode a start point (this should be impossible)");
    return words;
}

void seed_threads(Backend &be, SeedStore &seeds, uint32_t first, uint32_t count, bool client)
{
    std::vector<uint32_t> words = make_seeds(seeds, first, count, client);
    be.write_state(first, count, words.data());
}

// Put the threads first .. first + count - 1 back where they stand after
// `batches_of(thread)` batches from their seeds: the centres are derived again from the
// secret scalars, as the reference keys of the self-tests are.
template <class F>
void restore_threads(Backend &be, SeedStore &seeds, uint32_t first, uint32_t count, uint32_t npairs, bool client, F batches_of)
{
    std::vector<uint32_t> words((size_t)count * CIP_STATE_WORDS);
    std::atomic<bool> failed(false);
    parallel_for(count, [&](uint32_t i) {
        uint8_t pk[32];
        if (!keys::public_key(pk, seeds[first + i], hit_offset(Hit{first + i, 0, 0}, batches_of(first + i), npairs)) ||
            !centre_words(&words[(size_t)i * CIP_STATE_WORDS], pk, client))
            failed = true;
    });
    if (failed) throw std::runtime_error("failed to derive a thread's position again (this should be impossible)");
    be.write_state(first, count, words.data());
}

// Launches that lose no hit. A device records at most Backend::kHitCap hits per launch
// and only counts the rest, so a launch that reports more has lost some. Its threads are
// then put back where they started and the same keys are run again in two halves, of the
// batches or, once it is down to one batch, of the threads, until every part fits.
struct Lossless {
    Backend &be;
    // restore(first, count, batch): put those threads back to the start of batch `batch`
    // of the launch in progress.
    std::function<void(uint32_t, uint32_t, uint32_t)> restore;
    uint64_t replays = 0;  // launches that had to be run again
    uint64_t dropped = 0;  // hits lost all the same: one thread's single batch overflowed

    // Runs batches b0 .. b0 + nb - 1 of the launch on threads first .. first + count - 1
    // and appends the hits to `out`, their batch numbers counted from the launch's start.
    void run(uint32_t first, uint32_t count, uint32_t b0, uint32_t nb, std::vector<Hit> &out)
    {
        std::vector<Hit> part;
        uint32_t n = be.launch(nb, part, first, count);
        if (n > part.size() && (nb > 1 || count > 1)) {
            replays++;
            restore(first, count, b0);
            if (nb > 1) {
                run(first, count, b0, nb / 2, out);
                run(first, count, b0 + nb / 2, nb - nb / 2, out);
            } else {
                run(first, count / 2, b0, 1, out);
                run(first + count / 2, count - count / 2, b0, 1, out);
            }
            return;
        }
        if (n > part.size()) dropped += n - part.size();
        for (Hit h : part) {
            h.batch += b0;
            out.push_back(h);
        }
    }
};

const char *scratch_name(uint32_t smode)
{
    return smode == CIP_SCRATCH_FULL ? "full" : smode == CIP_SCRATCH_HALF ? "half" : "lean";
}

const char *arith_name(uint32_t arith) { return arith == CIP_ARITH_NARROW ? "narrow" : "wide"; }

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

// Keys per second of a configuration, with the field arithmetic the backend was last set
// to, measured over about `seconds` of launches. Leaves the device configured that way with
// `start` (state words for `nthreads` threads) loaded and advanced by an unspecified number
// of batches.
double measure(Backend &be, uint32_t nthreads, uint32_t npairs, uint32_t smode, const Filter &filter,
               const std::vector<uint32_t> &start, double seconds)
{
    std::vector<Hit> hits;
    be.configure(nthreads, npairs, smode, kernel_table(npairs, smode == CIP_SCRATCH_X25519));
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
    std::atomic<uint64_t> replays{0};
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
        std::vector<uint32_t> counts = opt.threads ? std::vector<uint32_t>{opt.threads} : be_.thread_candidates();
        std::vector<uint32_t> modes = opt.scratch >= 0 ? std::vector<uint32_t>{(uint32_t)opt.scratch} : be_.scratch_candidates();
        if (opt.client) modes = {CIP_SCRATCH_X25519};
        std::vector<uint32_t> ariths = opt.arith >= 0 ? std::vector<uint32_t>{(uint32_t)opt.arith} : be_.arith_candidates();
        const uint32_t most = *std::max_element(counts.begin(), counts.end());
        if (*std::min_element(counts.begin(), counts.end()) == 0) throw std::runtime_error("threads must be positive");
        seeds_.reset(new SeedStore(most));
        const std::vector<uint32_t> start = make_seeds(*seeds_, 0, most, opt.client);

        // What the backend cannot know about its device is measured: thread count, field
        // arithmetic, scratch mode, in that order. A kernel is only as good as the driver's
        // compiler, so the configuration that comes out of it has to pass the self-test,
        // and an arithmetic that does not (or does not compile) is dropped while there is
        // another to fall back to.
        for (;;) {
            arith_ = ariths[0];
            std::string why;
            try {
                be_.set_arith(arith_);
                pick_threads(counts, modes[0], start);
                pick_arith(ariths, modes[0], start);
                pick_scratch(modes, start);

                base_.assign(T_, 0);
                batches_ = 0;
                be_.configure(T_, N_, smode_, kernel_table(N_, opt.client));
                be_.write_state(0, T_, start.data());
                // Planted needles: derive a handful of this device's candidate keys on the
                // CPU, search for exactly those, and require the GPU to report exactly them.
                if (needle_test(3, why) && needle_test(24, why)) break;
                why = "start-up self-test failed: " + why;
            } catch (const std::exception &e) {
                if (ariths.size() < 2) throw;
                why = e.what();
            }
            if (ariths.size() < 2) throw std::runtime_error(be_.describe() + ": " + why);
            if (!opt.quiet)
                sh_.say("device " + std::to_string(index_) + ": not using the " + arith_name(arith_) + " arithmetic: " + why);
            ariths.erase(std::find(ariths.begin(), ariths.end(), arith_));
        }

        be_.set_filter(sh_.filter);
        if (!opt.quiet) {
            char buf[256];
            snprintf(buf, sizeof buf, "device %d: %s | %u threads x %u keys/batch | %.0f MiB scratch (%s)%s | self-test ok",
                     index_, be_.describe().c_str(), T_, 2 * N_ + 1, scratch_bytes(T_, N_, smode_) / 1048576.0,
                     scratch_name(smode_), arith_ == CIP_ARITH_NARROW ? " | narrow arithmetic" : "");
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
    // Keys per second of a candidate configuration; 0, with the reason in `problem`, if
    // the device cannot run it (e.g. it does not fit the device's memory).
    double trial(uint32_t threads, uint32_t pairs, uint32_t mode, const std::vector<uint32_t> &start, std::string &problem)
    {
        try {
            check_config(threads, pairs, mode);
            return measure(be_, threads, pairs, mode, sh_.filter, start, 0.2);
        } catch (const std::exception &e) {
            problem = e.what();
            return 0;
        }
    }

    void report(const char *what, const std::string &rates, const std::string &choice)
    {
        if (!sh_.opt.quiet) sh_.say("device " + std::to_string(index_) + ": " + what + ": " + rates + " -> " + choice);
    }

    // How many threads a device keeps busy, and how much scratch memory it can cycle
    // through without stalling, is not something every API tells. The candidates come in
    // ascending order and the rate has one peak, so the search ends once it has fallen 10%
    // below the best. Differences under 2% are measurement noise: among the counts that
    // close to the fastest, take the lowest.
    void pick_threads(const std::vector<uint32_t> &counts, uint32_t mode, const std::vector<uint32_t> &start)
    {
        T_ = counts[0];
        if (counts.size() < 2) return;
        std::vector<double> rates;
        std::string list, problem;
        double best = 0;
        for (uint32_t t : counts) {
            rates.push_back(trial(t, sh_.opt.pairs ? sh_.opt.pairs : fit_pairs(be_, t, mode), mode, start, problem));
            list += std::string(list.empty() ? "" : ", ") + std::to_string(t) + " " + (rates.back() > 0 ? human(rates.back(), "keys/s") : "-");
            best = std::max(best, rates.back());
            if (rates.back() < 0.9 * best) break;
        }
        if (best <= 0) throw std::runtime_error(problem);
        T_ = 0;
        for (size_t i = 0; i < rates.size(); i++)
            if (rates[i] >= 0.98 * best && (T_ == 0 || counts[i] < T_)) T_ = counts[i];
        report("threads", list, std::to_string(T_));
    }

    // Which field arithmetic is faster depends on the device's multipliers and on what its
    // compiler makes of them. To replace the backend's first choice another has to be 2%
    // faster, which is more than the measurement's noise.
    void pick_arith(const std::vector<uint32_t> &ariths, uint32_t mode, const std::vector<uint32_t> &start)
    {
        if (ariths.size() < 2) return;
        const uint32_t n = sh_.opt.pairs ? sh_.opt.pairs : fit_pairs(be_, T_, mode);
        std::string list, problem;
        double best = 0;
        for (uint32_t a : ariths) {
            be_.set_arith(a);
            double rate = trial(T_, n, mode, start, problem);
            list += std::string(list.empty() ? "" : ", ") + arith_name(a) + " " + (rate > 0 ? human(rate, "keys/s") : "-");
            if (rate > 1.02 * best) {
                best = rate;
                arith_ = a;
            }
        }
        be_.set_arith(arith_);
        if (best <= 0) throw std::runtime_error(problem);
        report("field arithmetic", list, arith_name(arith_));
    }

    // Which scratch mode is fastest depends on how the device's arithmetic throughput
    // compares with its memory bandwidth. Among the modes within 2% of the fastest, take
    // the one that needs the least device memory.
    void pick_scratch(const std::vector<uint32_t> &modes, const std::vector<uint32_t> &start)
    {
        struct Trial {
            uint32_t mode, pairs;
            double rate;
        };
        std::vector<Trial> trials;
        std::string list, problem;
        double best = 0;
        for (uint32_t mode : modes) {
            uint32_t n = sh_.opt.pairs ? sh_.opt.pairs : fit_pairs(be_, T_, mode);
            double rate = 0;
            if (modes.size() > 1) {
                rate = trial(T_, n, mode, start, problem);
                if (rate <= 0) continue;
                list += std::string(list.empty() ? "" : ", ") + scratch_name(mode) + " " + human(rate, "keys/s");
            } else {
                check_config(T_, n, mode);
            }
            trials.push_back(Trial{mode, n, rate});
            best = std::max(best, rate);
        }
        if (trials.empty()) throw std::runtime_error(problem);
        const Trial *pick = nullptr;
        for (const Trial &t : trials)
            if (t.rate >= 0.98 * best &&
                (!pick || scratch_bytes(T_, t.pairs, t.mode) < scratch_bytes(T_, pick->pairs, pick->mode)))
                pick = &t;
        smode_ = pick->mode;
        N_ = pick->pairs;
        if (modes.size() > 1) report("scratch modes", list, scratch_name(smode_));
    }

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
            if (!candidate_key(pk, (*seeds_)[h.tid], hit_offset(h, batches_ - base_[h.tid], N_), sh_.opt.client)) {
                why = "could not derive a needle key";
                return false;
            }
            needles.push_back(key_text(pk, sh_.opt.client).substr(0, 12));
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
        const uint64_t per_thread = 2 * (uint64_t)N_ + 1;

        // Hits are rare for any prefix worth a GPU. For the short ones the launches are
        // kept small enough that half the hit buffer is expected to do: fewer batches, and
        // below one batch a window of the threads at a time. Chance can still overflow the
        // buffer; such a launch is then run again in parts (Lossless).
        const double per_batch_hits = (double)T_ * (double)per_thread * sh_.filter.density;
        const double budget = Backend::kHitCap / 2.0;
        const uint32_t window = per_batch_hits > budget ? (uint32_t)std::max(1.0, T_ * budget / per_batch_hits) : T_;
        const double nb_limit = window < T_ ? 1.0 : std::min(16384.0, std::max(1.0, budget / std::max(per_batch_hits, 1e-300)));

        Lossless lossless{be_, [this](uint32_t first, uint32_t count, uint32_t batch) {
                              restore_threads(be_, *seeds_, first, count, N_, sh_.opt.client,
                                              [&](uint32_t t) { return batches_ - base_[t] + batch; });
                          }};
        uint32_t nb = 1;
        std::vector<Hit> hits;
        std::vector<uint8_t> burned(T_, 0);
        std::vector<uint32_t> burned_list;
        double busy = 0;
        uint64_t keys_run = 0;

        while (!sh_.stop && !g_stop) {
            double dt = 0;
            // One round moves every thread by nb batches, a window of threads at a time.
            for (uint32_t first = 0; first < T_ && !sh_.stop && !g_stop; first += window) {
                const uint32_t count = std::min(window, T_ - first);
                hits.clear();
                double t0 = now();
                lossless.run(first, count, 0, nb, hits);
                dt += now() - t0;
                keys_run += (uint64_t)count * per_thread * nb;

                for (const Hit &h : hits) {
                    if (sh_.stop) break;
                    if (!hit_plausible(h, T_, N_, nb) || h.tid - first >= count) {
                        report_mismatch(h, "out-of-range hit record");
                        continue;
                    }
                    if (burned[h.tid]) continue;  // one key per seed, ever
                    keys::Key key;
                    keys::ClientKey pair;
                    std::string why;
                    const keys::Offset m = hit_offset(h, batches_ - base_[h.tid], N_);
                    if (!(opt.client ? keys::finalize_client(pair, (*seeds_)[h.tid], m, opt.prefixes, why)
                                     : keys::finalize(key, (*seeds_)[h.tid], m, opt.prefixes, why))) {
                        // A prefix longer than the 64 bits the GPU compares can pass the GPU
                        // filter and still fail here; anything else means the device computed
                        // something the CPU cannot reproduce.
                        const bool derived = opt.client ? !pair.text.empty() : !key.address.empty();
                        if (!derived || !sh_.filter.matches(opt.client ? pair.pk : key.pk)) report_mismatch(h, why);
                        continue;
                    }
                    burned[h.tid] = 1;
                    burned_list.push_back(h.tid);
                    if (opt.bench_seconds > 0) continue;
                    if (opt.client)
                        deliver(pair);
                    else
                        deliver(key);
                }
                sh_.tested += (uint64_t)count * per_thread * nb;

                // Retire every seed that produced a key and start those threads afresh:
                // they run next in the following round.
                for (uint32_t tid : burned_list) {
                    seed_threads(be_, *seeds_, tid, 1, opt.client);
                    base_[tid] = batches_ + nb;
                    burned[tid] = 0;
                }
                burned_list.clear();
            }
            batches_ += nb;
            busy += dt;

            double ideal = nb * target / std::max(dt, 1e-6);
            nb = (uint32_t)std::lround(std::min(std::max(std::min(ideal, nb * 4.0), 1.0), nb_limit));
        }
        sh_.replays += lossless.replays;
        sh_.overflowed += lossless.dropped;
        std::lock_guard<std::mutex> lock(sh_.out_mu);
        sh_.launch_rate += (double)keys_run / std::max(busy, 1e-9);
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

    void deliver(const keys::ClientKey &key)
    {
        std::lock_guard<std::mutex> lock(sh_.out_mu);
        const Options &opt = sh_.opt;
        if (opt.want && sh_.found >= opt.want) return;
        std::string name, err;
        if (!keys::write_client_key(opt.outdir, key, opt.service, name, err)) {
            if (sh_.tty) fprintf(stderr, "\r\033[K");
            fprintf(stderr, "error: could not save %s: %s\n", name.c_str(), err.c_str());
            sh_.failed = true;
            sh_.stop = true;
            return;
        }
        if (sh_.tty) fprintf(stderr, "\r\033[K");
        printf("%s\n", name.c_str());
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
    uint32_t T_ = 0, N_ = 0, smode_ = CIP_SCRATCH_LEAN, arith_ = CIP_ARITH_WIDE;
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
    sh.filter = build_filter(opt.prefixes, opt.filter_tables);
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
        // The wait for the next key is exponentially distributed: half of all waits are
        // shorter than ln 2 times the average, 95% shorter than ln 20 times.
        double mean = 1.0 / (sh.p_match * std::max(rate, 1.0));
        snprintf(buf, sizeof buf, "%s | %s tested in %s | found %llu%s | 1 key per %s (50%% within %s, 95%% within %s)",
                 human(rate, "keys/s").c_str(), human((double)tested, "").c_str(), duration(t - start).c_str(),
                 (unsigned long long)sh.found.load(), opt.want ? ("/" + std::to_string(opt.want)).c_str() : "",
                 duration(mean).c_str(), duration(mean * std::log(2.0)).c_str(), duration(mean * std::log(20.0)).c_str());
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
    if (sh.replays && !opt.quiet)
        fprintf(stderr, "note: %llu launches held more hits than the %u a device records and were run again in parts.\n",
                (unsigned long long)sh.replays.load(), Backend::kHitCap);
    if (sh.overflowed)
        fprintf(stderr, "note: %llu hits were dropped: a single thread found more than %u in one batch.\n",
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
        // Where the backend has a range of its own to search, that is the one to show.
        if (!opt.threads && be.thread_candidates().size() > 1) counts = be.thread_candidates();
        std::sort(counts.begin(), counts.end());
        const std::vector<uint32_t> ariths = opt.arith >= 0 ? std::vector<uint32_t>{(uint32_t)opt.arith} : be.arith_candidates();
        const std::vector<uint32_t> modes = opt.client ? std::vector<uint32_t>{CIP_SCRATCH_X25519}
                                                       : std::vector<uint32_t>{CIP_SCRATCH_FULL, CIP_SCRATCH_LEAN, CIP_SCRATCH_HALF};
        printf("device %zu: %s\n%10s %8s %8s %8s %12s %14s\n", dev, be.describe().c_str(), "threads", "pairs", "scratch",
               "arith", "memory", "keys/s");
        try {
            SeedStore seeds(counts.back());
            std::vector<uint32_t> start = make_seeds(seeds, 0, counts.back(), opt.client);
            double best = 0;
            uint32_t best_t = 0, best_n = 0, best_mode = 0, best_arith = 0;
            for (uint32_t arith : ariths)
                for (uint32_t t : counts)
                    for (uint32_t mode : modes) {
                        if (g_stop) return 130;
                        uint32_t n = opt.pairs ? opt.pairs : fit_pairs(be, t, mode);
                        double rate = 0;
                        std::string note;
                        try {
                            check_config(t, n, mode);
                            be.set_arith(arith);
                            rate = measure(be, t, n, mode, filter, start, 1.0);
                        } catch (const std::exception &e) {
                            note = e.what();
                        }
                        printf("%10u %8u %8s %8s %8.0f MiB %14s %s\n", t, n, scratch_name(mode), arith_name(arith),
                               scratch_bytes(t, n, mode) / 1048576.0, rate > 0 ? human(rate, "").c_str() : "-", note.c_str());
                        fflush(stdout);
                        if (rate > best) {
                            best = rate;
                            best_t = t;
                            best_n = n;
                            best_mode = mode;
                            best_arith = arith;
                        }
                    }
            if (best > 0)
                printf("fastest: --threads %u --pairs %u --scratch %s%s%s   (%s)\n\n", best_t, best_n, scratch_name(best_mode),
                       ariths.size() > 1 ? " --arith " : "", ariths.size() > 1 ? arith_name(best_arith) : "",
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

bool exhaustive_case(Backend &be, uint32_t T, uint32_t N, uint32_t nb, uint32_t smode, uint32_t arith, std::string &log)
{
    char tag[96];
    const bool client = smode == CIP_SCRATCH_X25519;
    snprintf(tag, sizeof tag, "T=%u N=%u batches=%u %s arith=%s", T, N, nb,
             client ? "x25519" : (std::string("scratch=") + scratch_name(smode)).c_str(), arith_name(arith));
    be.set_arith(arith);
    be.configure(T, N, smode, kernel_table(N, client));
    SeedStore seeds(T);
    seed_threads(be, seeds, 0, T, client);
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
        if (!candidate_key(c.pk, seeds[c.pos.tid], hit_offset(c.pos, 0, N), client)) bad = true;
        c.address = key_text(c.pk, client);
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
    for (int i = 0; i < 8; i++) wide.push_back(random_prefix(2));
    wide.push_back(stolen(4));
    lists.push_back(wide);
    // The direct mode with twelve slots: one more prefix than the four-slot mode takes,
    // and a full house with real candidates in the first, the fifth and the last slot.
    lists.push_back({random_prefix(3), random_prefix(3), random_prefix(3), random_prefix(3), stolen(6)});
    std::vector<std::string> dozen;
    for (int i = 0; i < 12; i++) dozen.push_back(i == 0 || i == 4 || i == 11 ? stolen(5 + i) : random_prefix(3));
    lists.push_back(dozen);
    // Half of all keys: in the larger cases that is more hits than a launch can record,
    // and the launch has to be run again in halves of its batches and of its threads.
    std::vector<std::string> dense;
    for (int i = 0; i < 16; i++) dense.push_back(std::string(1, "abcdefghijklmnopqrstuvwxyz234567"[2 * i + (int)(rng() & 1)]));
    lists.push_back(dense);
    // Wildcards: a candidate's address with two characters replaced by their class and one
    // by "any", next to a prefix of wildcards alone and one with a wildcard past 64 bits.
    auto wild = [&](std::string s) {
        std::string w;
        for (size_t i = 0; i < s.size(); i++)
            w += i == 1 || i == 4 ? (s[i] >= 'a' ? "?l" : "?d") : i == 2 ? "?a" : std::string(1, s[i]);
        return w;
    };
    lists.push_back({wild(stolen(6)), "?d?d" + random_prefix(1), wild(stolen(4)) + "?l?l?l?l?l?l?l?l?l?l?d"});

    Lossless lossless{be, [&](uint32_t first, uint32_t count, uint32_t batch) {
                          restore_threads(be, seeds, first, count, N, client, [&](uint32_t) { return (uint64_t)batch; });
                      }};
    for (size_t li = 0; li < lists.size(); li++) {
        const std::vector<std::string> &prefixes = lists[li];
        Filter f = build_filter(prefixes);
        std::vector<Hit> want;
        bool wildcards = false;
        for (const std::string &p : prefixes) wildcards = wildcards || p.find('?') != std::string::npos;
        for (const Candidate &c : all) {
            bool text = false;
            for (const std::string &p : prefixes)
                // The kernel contract is the first 64 bits; 12 characters fit inside them.
                if (onion::prefix_matches(onion::prefix_head(p, 12), c.address)) text = true;
            bool kernel = f.matches(c.pk);
            // For a wildcard the kernel compares only the bits all its characters share.
            if (kernel && !text && !wildcards) {
                log += std::string(tag) + ": filter matches a key no prefix matches\n";
                return false;
            }
            if (text && !kernel) {
                // Only legitimate for prefixes of 13+ characters: bits 60..63 also count.
                bool longer = false;
                for (const std::string &p : prefixes)
                    if (onion::prefix_sets(p).size() > 12 && onion::prefix_matches(onion::prefix_head(p, 12), c.address))
                        longer = true;
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
        const uint64_t replays_before = lossless.replays;
        lossless.run(0, T, 0, nb, got);
        std::sort(got.begin(), got.end(), hit_less);
        if (got.size() != want.size() || !std::equal(got.begin(), got.end(), want.begin(), hit_equal)) {
            char buf[256];
            snprintf(buf, sizeof buf, "%s: prefix list %zu (%s mode): expected %zu hits, device reported %zu%s\n", tag, li,
                     f.mode == CIP_MODE_BITMAP ? "bitmap" : "direct", want.size(), got.size(),
                     got.size() == want.size() ? " but different ones" : "");
            log += buf;
            return false;
        }
        if ((want.size() > Backend::kHitCap) != (lossless.replays > replays_before) || lossless.dropped) {
            log += std::string(tag) + ": prefix list " + std::to_string(li) + ": a launch was run again without need, or not when needed\n";
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
        uint8_t pk[32];
        uint32_t want[CIP_STATE_WORDS], got[CIP_STATE_WORDS];
        if (!keys::public_key(pk, seeds[t], hit_offset(Hit{t, nb, 0}, 0, N)) || !centre_words(want, pk, client)) return false;
        for (int half = 0; half < 2; half++) {
            fe a;
            for (int k = 0; k < 10; k++) a.v[k] = final_state[(size_t)t * CIP_STATE_WORDS + 10 * half + k];
            a = ed::canonical(a);
            for (int k = 0; k < 10; k++) got[10 * half + k] = a.v[k];
        }
        if (memcmp(got, want, sizeof want) != 0) {
            log += std::string(tag) + ": centre of thread " + std::to_string(t) + " is wrong after the launch\n";
            return false;
        }
    }
    log += std::string(tag) + ": ok (" + std::to_string(all.size()) + " keys, " + std::to_string(lists.size()) + " prefix lists" +
           (lossless.replays ? ", " + std::to_string(lossless.replays) + " launches run again" : "") + ")\n";
    return true;
}

}  // namespace

bool selftest_exhaustive(Backend &be, std::string &log)
{
    // Every arithmetic the backend has kernels for, not only those it would pick.
    for (uint32_t arith : {CIP_ARITH_WIDE, CIP_ARITH_NARROW}) {
        try {
            be.set_arith(arith);
        } catch (const std::exception &) {
            continue;
        }
        for (uint32_t smode : {CIP_SCRATCH_FULL, CIP_SCRATCH_LEAN, CIP_SCRATCH_HALF, CIP_SCRATCH_X25519}) {
            uint32_t odd = smode == CIP_SCRATCH_HALF ? 1 : 0;  // half mode needs an even batch size
            if (!exhaustive_case(be, 5, 1 + odd, 3, smode, arith, log) || !exhaustive_case(be, 33, 7 + odd, 2, smode, arith, log) ||
                !exhaustive_case(be, 64, 48, 3, smode, arith, log) || !exhaustive_case(be, 300, 16, 1, smode, arith, log))
                return false;
        }
    }
    return true;
}
