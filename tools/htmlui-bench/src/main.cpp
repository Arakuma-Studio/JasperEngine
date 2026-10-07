// svgbench: LunaSVG / ThorVG / nanosvg benchmark driver (see tools/htmlui-bench/README).
//
//   svgbench --lib luna,thorvg,nanosvg --list files.txt --set icons --sizes 24,48,128,512,1024
//            --mode time|render|mutate|memory --out results.csv [--render-dir DIR]
//            [--min-time-ms 200] [--raster-min-time-ms 100] [--cold-sizes 24,48]
//            [--threads N] [--font-dir DIR] [--shard K/N] [--root DIR]
//   svgbench --selftest
//
// Several libraries in one run are interleaved per file (rotating order) so slow drifts of
// the machine affect all of them alike.
#include "Backend.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <vector>

#if defined(__EMSCRIPTEN__)
#include <malloc.h>
#else
#include <malloc.h>
#include <sys/resource.h>
#endif

#include "stb_image_write.h"

#ifndef THORVG_BENCH_NAME
#define THORVG_BENCH_NAME "thorvg"
#endif

namespace {

using Clock = std::chrono::steady_clock;

struct Options {
    std::vector<std::string> libs;
    std::string list, root, set = "set", mode = "time", out, renderDir, fontDir;
    std::vector<int> sizes{24, 48, 128, 512, 1024};
    std::vector<int> coldSizes{24, 48};
    double parseMinMs = 200.0, rasterMinMs = 100.0;
    int threads = 0;
    int shard = 0, shards = 1;
    bool selftest = false;
};

struct Stats {
    size_t n = 0;
    double median = 0, p95 = 0, min = 0, mean = 0;
};

Stats stats(std::vector<double>& v)
{
    Stats s;
    if (v.empty()) return s;
    std::sort(v.begin(), v.end());
    s.n = v.size();
    s.median = v.size() % 2 ? v[v.size() / 2] : 0.5 * (v[v.size() / 2 - 1] + v[v.size() / 2]);
    s.p95 = v[std::min(v.size() - 1, size_t(std::ceil(0.95 * v.size())) - 1)];
    s.min = v.front();
    double sum = 0;
    for (double x : v) sum += x;
    s.mean = sum / v.size();
    return s;
}

// Runs fn repeatedly: a warm-up (>= 2 runs, up to min(20 ms, minMs / 5)), then timed runs until
// minMs of measured time and at least 5 samples (at most 1e6). Returns per-run microseconds.
// fn returns false on failure, which stops the measurement.
bool measure(const std::function<bool()>& fn, double minMs, std::vector<double>& samples,
             const std::function<void()>& between = nullptr)
{
    samples.clear();
    double warmMs = std::min(20.0, minMs / 5.0), spent = 0;
    for (int i = 0; i < 2 || spent < warmMs; i++) {
        auto t0 = Clock::now();
        if (!fn()) return false;
        spent += std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        if (between) between();
        if (i > 10000) break;
    }
    double total = 0;
    while ((total < minMs * 1000.0 || samples.size() < 5) && samples.size() < 1000000) {
        auto t0 = Clock::now();
        bool ok = fn();
        double us = std::chrono::duration<double, std::micro>(Clock::now() - t0).count();
        if (!ok) return false;
        samples.push_back(us);
        total += us;
        if (between) between();
    }
    return true;
}

size_t heapUsed()
{
#if defined(__EMSCRIPTEN__)
    return size_t(mallinfo().uordblks);
#else
    return size_t(mallinfo2().uordblks);
#endif
}

long peakRssKb()
{
#if defined(__EMSCRIPTEN__)
    return 0;
#else
    rusage ru{};
    getrusage(RUSAGE_SELF, &ru);
    return ru.ru_maxrss;
#endif
}

bool readFile(const std::string& path, std::string& out)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

void makeDirs(const std::string& path)
{
    for (size_t i = 1; i <= path.size(); i++) {
        if (i == path.size() || path[i] == '/') mkdir(path.substr(0, i).c_str(), 0755);
    }
}

std::vector<int> parseInts(const std::string& s)
{
    std::vector<int> v;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ',')) if (!item.empty()) v.push_back(std::atoi(item.c_str()));
    return v;
}

std::vector<std::string> split(const std::string& s, char sep)
{
    std::vector<std::string> v;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, sep)) if (!item.empty()) v.push_back(item);
    return v;
}

struct Lib {
    std::string label;
    std::unique_ptr<SvgBackend> backend;
};

std::vector<Lib> createLibs(const Options& o, bool accessible)
{
    BackendOptions bo;
    bo.threads = o.threads;
    bo.fontDir = o.fontDir;
    bo.accessible = accessible;
    std::vector<Lib> libs;
    for (const std::string& name : o.libs) {
        Lib lib;
        if (name == "luna") {
            lunaGlobalInit(bo);
            lib.label = "luna";
            lib.backend = createLunaBackend(bo);
        } else if (name == "thorvg") {
            lib.label = THORVG_BENCH_NAME;
            if (o.threads > 0) lib.label += "-mt" + std::to_string(o.threads);
            lib.backend = createThorVGBackend(bo);
        } else if (name == "nanosvg") {
            lib.label = "nanosvg";
            lib.backend = createNanoSVGBackend(bo);
        } else {
            std::fprintf(stderr, "unknown library '%s' (luna, thorvg, nanosvg)\n", name.c_str());
            std::exit(2);
        }
        libs.push_back(std::move(lib));
    }
    return libs;
}

// Output size for a target width: uniform scale, height rounded (at least 1 px).
void targetSize(const SvgBackend& b, int width, int& w, int& h)
{
    w = width;
    h = std::max(1, int(std::lround(b.intrinsicHeight() * (width / b.intrinsicWidth()))));
}

bool writePng(const std::string& path, int w, int h, const std::vector<uint8_t>& rgba)
{
    size_t slash = path.rfind('/');
    if (slash != std::string::npos) makeDirs(path.substr(0, slash));
    return stbi_write_png(path.c_str(), w, h, 4, rgba.data(), w * 4) != 0;
}

std::string pngName(const std::string& rel)
{
    std::string n = rel;
    if (n.size() > 4 && n.compare(n.size() - 4, 4, ".svg") == 0) n.resize(n.size() - 4);
    return n + ".png";
}

class Csv {
public:
    explicit Csv(const std::string& path)
    {
        f = path.empty() ? stdout : std::fopen(path.c_str(), "w");
        if (!f) {
            std::fprintf(stderr, "cannot write %s\n", path.c_str());
            std::exit(2);
        }
        std::fprintf(f, "lib,set,file,metric,size,w,h,n,median,p95,min,mean,status\n");
    }
    ~Csv()
    {
        if (f != stdout) std::fclose(f);
    }
    void row(const std::string& lib, const std::string& set, const std::string& file, const char* metric, int size,
             int w, int h, const Stats& s, const char* status)
    {
        std::fprintf(f, "%s,%s,%s,%s,%d,%d,%d,%zu,%.3f,%.3f,%.3f,%.3f,%s\n", lib.c_str(), set.c_str(), file.c_str(),
                     metric, size, w, h, s.n, s.median, s.p95, s.min, s.mean, status);
        std::fflush(f);
    }
    void value(const std::string& lib, const std::string& set, const std::string& file, const char* metric, int size,
               double v, const char* status = "ok")
    {
        Stats s;
        s.n = 1;
        s.median = s.p95 = s.min = s.mean = v;
        row(lib, set, file, metric, size, 0, 0, s, status);
    }

private:
    FILE* f;
};

// Picks the element whose fill the mutation benchmark changes: the first drawable element
// outside defs/clipPath/mask/symbol/pattern/marker. Uses its id or injects one.
bool injectTargetId(std::string& svg, std::string& id)
{
    static const char* drawables[] = {"path", "circle", "rect", "ellipse", "line", "polyline", "polygon"};
    static const char* containers[] = {"defs", "clipPath", "mask", "symbol", "pattern", "marker"};
    int depth = 0;
    size_t pos = 0;
    while ((pos = svg.find('<', pos)) != std::string::npos) {
        if (svg.compare(pos, 4, "<!--") == 0) {
            pos = svg.find("-->", pos);
            if (pos == std::string::npos) return false;
            continue;
        }
        bool closing = pos + 1 < svg.size() && svg[pos + 1] == '/';
        size_t nameStart = pos + (closing ? 2 : 1);
        size_t nameEnd = nameStart;
        while (nameEnd < svg.size() && (std::isalnum((unsigned char)svg[nameEnd]) || svg[nameEnd] == ':' || svg[nameEnd] == '-')) nameEnd++;
        std::string name = svg.substr(nameStart, nameEnd - nameStart);
        size_t tagEnd = svg.find('>', nameEnd);
        if (tagEnd == std::string::npos) return false;
        bool selfClosing = svg[tagEnd - 1] == '/';
        bool isContainer = std::any_of(std::begin(containers), std::end(containers), [&](const char* c) { return name == c; });
        if (isContainer) {
            if (closing) depth--;
            else if (!selfClosing) depth++;
        } else if (!closing && depth == 0 &&
                   std::any_of(std::begin(drawables), std::end(drawables), [&](const char* d) { return name == d; })) {
            std::string tag = svg.substr(pos, tagEnd - pos);
            size_t idAt = tag.find(" id=\"");
            if (idAt == std::string::npos) idAt = tag.find("\tid=\"");
            if (idAt == std::string::npos) idAt = tag.find("\nid=\"");
            if (idAt != std::string::npos) {
                size_t v0 = idAt + 5, v1 = tag.find('"', v0);
                id = tag.substr(v0, v1 - v0);
                return !id.empty();
            }
            id = "bench-target";
            svg.insert(nameEnd, " id=\"bench-target\"");
            return true;
        }
        pos = tagEnd;
    }
    return false;
}

struct Entry {
    std::string rel, path;
};

std::vector<Entry> loadList(const Options& o)
{
    std::vector<Entry> entries;
    std::ifstream f(o.list);
    if (!f) {
        std::fprintf(stderr, "cannot read list %s\n", o.list.c_str());
        std::exit(2);
    }
    std::string root = o.root;
    if (root.empty()) {
        size_t slash = o.list.rfind('/');
        root = slash == std::string::npos ? "." : o.list.substr(0, slash);
    }
    std::string line;
    size_t index = 0;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        if (index++ % size_t(o.shards) != size_t(o.shard)) continue;
        entries.push_back({line, line[0] == '/' ? line : root + "/" + line});
    }
    return entries;
}

int runTime(const Options& o)
{
    auto libs = createLibs(o, false);
    auto entries = loadList(o);
    Csv csv(o.out);
    std::vector<double> samples;
    std::vector<uint8_t> pixels;
    for (size_t fi = 0; fi < entries.size(); fi++) {
        const Entry& e = entries[fi];
        std::string svg;
        if (!readFile(e.path, svg)) {
            std::fprintf(stderr, "cannot read %s\n", e.path.c_str());
            continue;
        }
        for (size_t li = 0; li < libs.size(); li++) {
            Lib& lib = libs[(fi + li) % libs.size()];
            SvgBackend& b = *lib.backend;
            bool ok = measure([&] { return b.parse(svg); }, o.parseMinMs, samples, [&] { b.reset(); });
            csv.row(lib.label, o.set, e.rel, "parse", 0, 0, 0, stats(samples), ok ? "ok" : "parse_failed");
            if (!ok) continue;
            b.parse(svg);
            for (int size : o.sizes) {
                int w, h;
                targetSize(b, size, w, h);
                ok = b.prepare(w, h) && measure([&] { return b.render(); }, o.rasterMinMs, samples);
                csv.row(lib.label, o.set, e.rel, "raster", size, w, h, stats(samples), ok ? "ok" : "render_failed");
                if (!ok) continue;
                measure([&] { b.readback(pixels); return true; }, std::min(o.rasterMinMs, 20.0), samples);
                csv.row(lib.label, o.set, e.rel, "readback", size, w, h, stats(samples), "ok");
            }
            for (int size : o.coldSizes) {
                // Load + first render at this size (what an engine pays for an image it has not seen).
                b.parse(svg);
                int w, h;
                targetSize(b, size, w, h);
                b.reset();
                ok = measure([&] { return b.parse(svg) && b.prepare(w, h) && b.render(); }, o.rasterMinMs, samples,
                             [&] { b.reset(); });
                csv.row(lib.label, o.set, e.rel, "cold", size, w, h, stats(samples), ok ? "ok" : "failed");
            }
            b.reset();
        }
        if ((fi + 1) % 50 == 0) std::fprintf(stderr, "[%s %d/%d] %zu/%zu\n", o.set.c_str(), o.shard, o.shards, fi + 1, entries.size());
    }
    return 0;
}

int runRender(const Options& o)
{
    auto libs = createLibs(o, false);
    auto entries = loadList(o);
    Csv csv(o.out);
    std::vector<uint8_t> pixels;
    Stats none;
    for (const Entry& e : entries) {
        std::string svg;
        if (!readFile(e.path, svg)) continue;
        for (Lib& lib : libs) {
            SvgBackend& b = *lib.backend;
            if (!b.parse(svg)) {
                csv.row(lib.label, o.set, e.rel, "render", 0, 0, 0, none, "parse_failed");
                continue;
            }
            for (int size : o.sizes) {
                int w, h;
                targetSize(b, size, w, h);
                bool ok = b.prepare(w, h) && b.render();
                if (ok) {
                    b.readback(pixels);
                    if (!o.renderDir.empty()) {
                        writePng(o.renderDir + "/" + lib.label + "/" + std::to_string(size) + "/" + pngName(e.rel), w, h, pixels);
                    }
                }
                csv.row(lib.label, o.set, e.rel, "render", size, w, h, none, ok ? "ok" : "render_failed");
            }
            b.reset();
        }
    }
    return 0;
}

int runMutate(const Options& o)
{
    auto libs = createLibs(o, true);
    auto entries = loadList(o);
    Csv csv(o.out);
    std::vector<double> samples;
    std::vector<uint8_t> pixels;
    int size = o.sizes.empty() ? 48 : o.sizes.front();
    for (size_t fi = 0; fi < entries.size(); fi++) {
        const Entry& e = entries[fi];
        std::string svg, id;
        if (!readFile(e.path, svg)) continue;
        if (!injectTargetId(svg, id)) {
            for (Lib& lib : libs) csv.row(lib.label, o.set, e.rel, "mutate", size, 0, 0, Stats{}, "no_target");
            continue;
        }
        for (size_t li = 0; li < libs.size(); li++) {
            Lib& lib = libs[(fi + li) % libs.size()];
            SvgBackend& b = *lib.backend;
            int w = 0, h = 0;
            bool ok = b.parse(svg);
            if (ok) {
                targetSize(b, size, w, h);
                ok = b.prepare(w, h) && b.render();
            }
            if (!ok) {
                csv.row(lib.label, o.set, e.rel, "mutate", size, 0, 0, Stats{}, "parse_failed");
                continue;
            }
            uint32_t colors[2] = {0x0000ffff, 0x00ff00ff};
            int k = 0;
            ok = measure([&] { return b.mutateFill(id, colors[k++ & 1]); }, o.rasterMinMs, samples);
            Stats s = stats(samples);
            const char* status = !ok ? "not_found" : (b.mutatesInPlace() ? "in_place" : "reparse");
            csv.row(lib.label, o.set, e.rel, "mutate", size, w, h, s, status);
            if (ok && !o.renderDir.empty() && b.mutateFill(id, 0xff0000ff)) {
                b.readback(pixels);
                writePng(o.renderDir + "/" + lib.label + "/mutate/" + pngName(e.rel), w, h, pixels);
            }
            b.reset();
        }
    }
    return 0;
}

int runMemory(const Options& o)
{
    if (o.libs.size() != 1) {
        std::fprintf(stderr, "--mode memory takes exactly one --lib (one process per library and set)\n");
        return 2;
    }
    long rssStart = peakRssKb();
    auto libs = createLibs(o, false);
    auto entries = loadList(o);
    Csv csv(o.out);
    Lib& lib = libs.front();
    SvgBackend& b = *lib.backend;
    int size = o.sizes.empty() ? 512 : o.sizes.back();
    for (const Entry& e : entries) {
        std::string svg;
        if (!readFile(e.path, svg)) continue;
        size_t h0 = heapUsed();
        if (!b.parse(svg)) {
            csv.value(lib.label, o.set, e.rel, "heap_parse_bytes", 0, 0, "parse_failed");
            continue;
        }
        size_t h1 = heapUsed();
        int w, h;
        targetSize(b, size, w, h);
        b.prepare(w, h);
        size_t h2 = heapUsed();
        b.render();
        size_t h3 = heapUsed();
        csv.value(lib.label, o.set, e.rel, "heap_parse_bytes", 0, double(h1) - double(h0));
        csv.value(lib.label, o.set, e.rel, "heap_render_bytes", size, double(h1) - double(h0) + double(h3) - double(h2));
        b.reset();
    }
    csv.value(lib.label, o.set, "*", "rss_start_kb", 0, double(rssStart));
    csv.value(lib.label, o.set, "*", "rss_peak_kb", size, double(peakRssKb()));
    return 0;
}

// --selftest: three known documents, each library, compared against each other and against
// expected pixels.
int runSelftest(const Options& o)
{
    struct Case {
        const char* name;
        const char* svg;
        int x, y;
        uint8_t rgba[4]; // expected premultiplied pixel at (x, y) at 64 px
    };
    static const Case cases[] = {
        {"rect-circle",
         "<svg xmlns='http://www.w3.org/2000/svg' width='32' height='32' viewBox='0 0 32 32'>"
         "<rect x='2' y='2' width='28' height='28' fill='#ff0000'/>"
         "<circle cx='16' cy='16' r='8' fill='none' stroke='#0000ff' stroke-width='4'/></svg>",
         8, 8, {255, 0, 0, 255}},
        {"linear-gradient",
         "<svg xmlns='http://www.w3.org/2000/svg' width='64' height='64'>"
         "<defs><linearGradient id='g' x1='0%' y1='0%' x2='100%' y2='0%'>"
         "<stop offset='0' stop-color='#000000'/><stop offset='1' stop-color='#00ff00'/></linearGradient></defs>"
         "<rect width='64' height='64' fill='url(#g)' opacity='0.5'/></svg>",
         32, 32, {0, 64, 0, 128}},
        {"lucide-stroke",
         "<svg xmlns='http://www.w3.org/2000/svg' width='24' height='24' viewBox='0 0 24 24' fill='none' "
         "stroke='#000000' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'>"
         "<path d='M5 12h14'/><path d='M12 5v14'/></svg>",
         32, 32, {0, 0, 0, 255}},
    };
    Options so = o;
    so.libs = {"luna", "thorvg", "nanosvg"};
    thorvgGlobalInit(BackendOptions{});
    auto libs = createLibs(so, false);
    int failures = 0;
    for (const Case& c : cases) {
        std::vector<std::vector<uint8_t>> images;
        for (Lib& lib : libs) {
            SvgBackend& b = *lib.backend;
            std::vector<uint8_t> px;
            int w = 0, h = 0;
            bool ok = b.parse(c.svg);
            if (ok) {
                targetSize(b, 64, w, h);
                ok = w == 64 && h == 64 && b.prepare(w, h) && b.render();
            }
            if (ok) b.readback(px);
            size_t covered = 0;
            for (size_t i = 3; i < px.size(); i += 4) covered += px[i] != 0;
            const uint8_t* p = ok ? &px[(size_t(c.y) * 64 + c.x) * 4] : nullptr;
            int err = 0;
            for (int k = 0; p && k < 4; k++) err = std::max(err, std::abs(int(p[k]) - int(c.rgba[k])));
            bool pass = ok && covered > 0 && err <= 8;
            std::printf("%-16s %-8s parse+render %-4s covered %5zu px  probe (%d,%d) = %3d %3d %3d %3d expected %3d %3d %3d %3d  %s\n",
                        c.name, lib.label.c_str(), ok ? "ok" : "FAIL", covered, c.x, c.y, p ? p[0] : 0, p ? p[1] : 0,
                        p ? p[2] : 0, p ? p[3] : 0, c.rgba[0], c.rgba[1], c.rgba[2], c.rgba[3], pass ? "PASS" : "FAIL");
            failures += !pass;
            if (!o.renderDir.empty() && ok) writePng(o.renderDir + "/selftest/" + lib.label + "-" + c.name + ".png", 64, 64, px);
            images.push_back(std::move(px));
            b.reset();
        }
        // Pairwise agreement: fewer than 2 % of pixels with a channel difference > 16
        // (the libraries antialias differently on edges, nothing else may differ).
        for (size_t a = 0; a < images.size(); a++) {
            for (size_t b = a + 1; b < images.size(); b++) {
                if (images[a].size() != images[b].size() || images[a].empty()) {
                    std::printf("%-16s %s vs %s: size mismatch FAIL\n", c.name, libs[a].label.c_str(), libs[b].label.c_str());
                    failures++;
                    continue;
                }
                size_t diff = 0, n = images[a].size() / 4;
                for (size_t i = 0; i < n; i++) {
                    int m = 0;
                    for (int k = 0; k < 4; k++) m = std::max(m, std::abs(int(images[a][i * 4 + k]) - int(images[b][i * 4 + k])));
                    diff += m > 16;
                }
                double frac = double(diff) / n;
                bool pass = frac < 0.02;
                std::printf("%-16s %s vs %s: %.2f %% pixels differ > 16  %s\n", c.name, libs[a].label.c_str(),
                            libs[b].label.c_str(), frac * 100.0, pass ? "PASS" : "FAIL");
                failures += !pass;
            }
        }
    }
    libs.clear();
    thorvgGlobalTerm();
    std::printf("selftest: %s (%d failure%s)\n", failures ? "FAILED" : "passed", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}

void usage()
{
    std::fprintf(stderr,
                 "usage: svgbench --lib luna,thorvg,nanosvg --list files.txt [--root DIR] [--set NAME]\n"
                 "                [--sizes 24,48,128,512,1024] [--mode time|render|mutate|memory]\n"
                 "                [--out results.csv] [--render-dir DIR] [--min-time-ms 200]\n"
                 "                [--raster-min-time-ms 100] [--cold-sizes 24,48] [--threads N]\n"
                 "                [--font-dir DIR] [--shard K/N]\n"
                 "       svgbench --selftest [--render-dir DIR]\n");
}

} // namespace

int main(int argc, char** argv)
{
    Options o;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                usage();
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--lib") o.libs = split(next(), ',');
        else if (a == "--list") o.list = next();
        else if (a == "--root") o.root = next();
        else if (a == "--set") o.set = next();
        else if (a == "--sizes") o.sizes = parseInts(next());
        else if (a == "--cold-sizes") o.coldSizes = parseInts(next());
        else if (a == "--mode") o.mode = next();
        else if (a == "--out") o.out = next();
        else if (a == "--render-dir") o.renderDir = next();
        else if (a == "--min-time-ms") o.parseMinMs = std::atof(next().c_str());
        else if (a == "--raster-min-time-ms") o.rasterMinMs = std::atof(next().c_str());
        else if (a == "--threads") o.threads = std::atoi(next().c_str());
        else if (a == "--font-dir") o.fontDir = next();
        else if (a == "--shard") {
            auto parts = split(next(), '/');
            if (parts.size() != 2) {
                usage();
                return 2;
            }
            o.shard = std::atoi(parts[0].c_str());
            o.shards = std::max(1, std::atoi(parts[1].c_str()));
        } else if (a == "--selftest") o.selftest = true;
        else {
            usage();
            return 2;
        }
    }
    if (o.selftest) return runSelftest(o);
    if (o.libs.empty() || o.list.empty()) {
        usage();
        return 2;
    }
    BackendOptions bo;
    bo.threads = o.threads;
    bo.fontDir = o.fontDir;
    bool usesThorVG = std::find(o.libs.begin(), o.libs.end(), "thorvg") != o.libs.end();
    if (usesThorVG) thorvgGlobalInit(bo);
    int rc;
    if (o.mode == "time") rc = runTime(o);
    else if (o.mode == "render") rc = runRender(o);
    else if (o.mode == "mutate") rc = runMutate(o);
    else if (o.mode == "memory") rc = runMemory(o);
    else {
        usage();
        rc = 2;
    }
    if (usesThorVG) thorvgGlobalTerm();
    return rc;
}
