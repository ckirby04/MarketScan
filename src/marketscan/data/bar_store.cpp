#include "marketscan/data/bar_store.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace marketscan::bar_store {
namespace {

static_assert(std::endian::native == std::endian::little,
              "the bar cache is a little-endian format; a big-endian host would read prices "
              "rather than fail, so this is refused at compile time");
static_assert(std::numeric_limits<double>::is_iec559,
              "the bar cache stores raw doubles; a host without IEEE-754 would reinterpret them");

struct Header {
    std::uint32_t magic;
    std::uint32_t version;
    std::uint64_t count;
    char ticker[kTickerBytes];
    std::int64_t interval;
    std::int64_t reserved;
};

static_assert(sizeof(Header) == kHeaderBytes, "header layout is the format");

[[noreturn]] void fail(const std::filesystem::path& path, const std::string& why) {
    throw std::runtime_error(path.string() + ": " + why);
}

}  // namespace

Timestamp
daily_close_stamp(std::int64_t day, ExchangeZone zone, int close_hour, int close_minute) noexcept {
    // Built from the local midnight that opens the day rather than by adding a
    // fixed offset to a UTC date, so the stamp tracks daylight saving: the
    // closing bell is 21:00 UTC in summer and 21:00 UTC in winter only if you
    // are wrong about one of them.
    const Timestamp midnight = session_start_of_day(zone, day);
    const std::int64_t seconds =
        static_cast<std::int64_t>(close_hour) * 3600 + static_cast<std::int64_t>(close_minute) * 60;
    return midnight + seconds * calendar::kNanosPerSecond;
}

void write(const std::filesystem::path& path, const BarSeries& series) {
    if (series.ticker.size() >= kTickerBytes) {
        fail(path,
             "ticker '" + series.ticker + "' does not fit in " + std::to_string(kTickerBytes - 1) +
                 " bytes");
    }

    for (std::size_t i = 1; i < series.bars.size(); ++i) {
        if (series.bars[i].ts <= series.bars[i - 1].ts) {
            fail(path,
                 "bars are not strictly increasing in time at index " + std::to_string(i) +
                     "; refusing to sort them, because a writer that produced this has "
                     "misunderstood its input");
        }
    }

    if (std::filesystem::path parent = path.parent_path(); !parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
    }

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        fail(path, "cannot open for writing");
    }

    Header header{};
    header.magic = kMagic;
    header.version = kVersion;
    header.count = series.bars.size();
    std::memcpy(header.ticker, series.ticker.data(), series.ticker.size());
    header.interval = series.interval;

    out.write(reinterpret_cast<const char*>(&header), sizeof(header));
    if (!series.bars.empty()) {
        out.write(reinterpret_cast<const char*>(series.bars.data()),
                  static_cast<std::streamsize>(series.bars.size() * sizeof(StoredBar)));
    }
    if (!out) {
        fail(path, "write failed");
    }
}

namespace {

Header read_header(std::ifstream& in, const std::filesystem::path& path) {
    Header header{};
    in.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (in.gcount() != static_cast<std::streamsize>(sizeof(header))) {
        fail(path, "too short to hold a header; not a MarketScan bar file");
    }
    if (header.magic != kMagic) {
        fail(path, "bad magic; not a MarketScan bar file");
    }
    if (header.version != kVersion) {
        fail(path,
             "format version " + std::to_string(header.version) + ", this build reads " +
                 std::to_string(kVersion));
    }
    if (header.ticker[kTickerBytes - 1] != '\0') {
        fail(path, "ticker field is not terminated; the header is corrupt");
    }
    return header;
}

}  // namespace

BarSeries read(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        fail(path, "cannot open");
    }

    const Header header = read_header(in, path);

    BarSeries series;
    series.ticker = header.ticker;
    series.interval = header.interval;

    if (header.count > (1ULL << 32)) {
        fail(path,
             "claims " + std::to_string(header.count) +
                 " bars, which is not a number of bars anything wrote on purpose");
    }

    series.bars.resize(static_cast<std::size_t>(header.count));
    if (header.count > 0) {
        const auto bytes = static_cast<std::streamsize>(header.count * sizeof(StoredBar));
        in.read(reinterpret_cast<char*>(series.bars.data()), bytes);
        if (in.gcount() != bytes) {
            fail(path,
                 "truncated: header claims " + std::to_string(header.count) +
                     " bars but the file holds " +
                     std::to_string(in.gcount() / static_cast<std::streamsize>(sizeof(StoredBar))));
        }
    }

    // Checked on read as well as on write. A file can be truncated, patched or
    // produced by a version of the writer that predates the check, and every
    // consumer downstream assumes ascending time.
    for (std::size_t i = 1; i < series.bars.size(); ++i) {
        if (series.bars[i].ts <= series.bars[i - 1].ts) {
            fail(path, "bars are not strictly increasing in time at index " + std::to_string(i));
        }
    }
    return series;
}

Summary summarise(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        fail(path, "cannot open");
    }
    const Header header = read_header(in, path);

    Summary summary;
    summary.ticker = header.ticker;
    summary.bars = header.count;
    summary.interval = header.interval;

    if (header.count > 0) {
        StoredBar bar{};
        in.read(reinterpret_cast<char*>(&bar), sizeof(bar));
        summary.first = bar.ts;

        in.seekg(static_cast<std::streamoff>(kHeaderBytes + (header.count - 1) * sizeof(StoredBar)),
                 std::ios::beg);
        in.read(reinterpret_cast<char*>(&bar), sizeof(bar));
        if (in.gcount() == static_cast<std::streamsize>(sizeof(bar))) {
            summary.last = bar.ts;
        }
    }
    return summary;
}

std::vector<std::filesystem::path> list(const std::filesystem::path& directory) {
    std::vector<std::filesystem::path> found;
    std::error_code ec;
    if (!std::filesystem::is_directory(directory, ec)) {
        return found;
    }
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(directory, ec)) {
        if (entry.is_regular_file(ec) && entry.path().extension() == kExtension) {
            found.push_back(entry.path());
        }
    }
    std::sort(found.begin(), found.end());
    return found;
}

}  // namespace marketscan::bar_store
