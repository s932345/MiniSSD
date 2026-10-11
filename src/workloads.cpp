#include "minissd/workloads.hpp"

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <random>
#include <sstream>
#include <stdexcept>

namespace minissd {
namespace {
using Bytes = std::vector<std::uint8_t>;
using Oracle = std::vector<std::optional<Bytes>>;

void check(bool condition, std::string_view message) {
    if (!condition) { throw std::runtime_error(std::string(message)); }
}
void expect(FtlStatus result, FtlError expected = FtlError::None) {
    check(result.error == expected && result.nand_error == NandError::None,
          "unexpected I/O result during workload");
}
Bytes pattern(std::mt19937& random, std::size_t bytes) {
    Bytes result(bytes);
    for (auto& byte : result) { byte = static_cast<std::uint8_t>(random() & 0xFFU); }
    return result;
}
void verify_all(const PageFtl& ftl, const Oracle& oracle) {
    const auto& geometry = ftl.nand().geometry();
    std::vector<bool> mapped(ftl.physical_page_count(), false);
    std::size_t valid = 0;
    for (std::size_t lba = 0; lba < oracle.size(); ++lba) {
        Bytes bytes{0xDE, 0xAD};
        if (!oracle[lba]) {
            expect(ftl.read(lba, bytes), FtlError::UnwrittenLba);
            check(bytes == Bytes({0xDE, 0xAD}) && !ftl.physical_address(lba), "unwritten LBA mismatch");
            continue;
        }
        expect(ftl.read(lba, bytes));
        check(bytes == *oracle[lba], "data integrity mismatch");
        const auto address = ftl.physical_address(lba).value();
        const auto index = address.block * geometry.pages_per_block + address.page;
        check(index < mapped.size() && !mapped[index], "mapping collision");
        mapped[index] = true;
        check(ftl.page_info(address) == PhysicalPageInfo{PageState::Valid, lba}, "reverse mapping mismatch");
        ++valid;
    }
    FtlStats counted;
    for (std::size_t block = 0; block < geometry.block_count; ++block) {
        for (std::size_t page = 0; page < geometry.pages_per_block; ++page) {
            const auto info = ftl.page_info({block, page}).value();
            const auto index = block * geometry.pages_per_block + page;
            check((info.state == PageState::Valid) == mapped[index], "live-page accounting mismatch");
            if (info.state == PageState::Free) {
                ++counted.free_pages;
                Bytes raw;
                check(ftl.nand().read_page(block, page, raw) == NandError::None &&
                      raw == Bytes(geometry.bytes_per_page, 0xFF), "free page is not erased");
            } else if (info.state == PageState::Valid) { ++counted.valid_pages; }
            else { ++counted.invalid_pages; }
            if (info.state != PageState::Valid) { check(!info.lba, "stale page has a logical owner"); }
            if (ftl.reserved_block() == block) { check(info.state != PageState::Valid, "reserve contains live data"); }
        }
    }
    check(counted == ftl.stats() && counted.valid_pages == valid, "page statistics mismatch");
    check(counted.free_pages + counted.valid_pages + counted.invalid_pages == ftl.physical_page_count(),
          "physical capacity accounting mismatch");
}

std::string compiler_name() {
#if defined(_MSC_VER)
    return "MSVC " + std::to_string(_MSC_VER);
#elif defined(__clang__)
    return "Clang " __clang_version__;
#elif defined(__GNUC__)
    return "GCC " __VERSION__;
#else
    return "unknown";
#endif
}
std::string build_name() {
#if defined(NDEBUG)
    return "Release";
#else
    return "Debug";
#endif
}
void json_environment(std::ostream& output) {
    output << "\"compiler\":\"" << json_escape(compiler_name())
           << "\",\"build\":\"" << build_name() << "\",\"storage\":\"RAM simulator\"";
}
void json_metrics(std::ostream& output, const FtlMetrics& metrics) {
    output << "{\"host_writes\":" << metrics.host_writes
           << ",\"nand_programs\":" << metrics.nand_programs
           << ",\"nand_erases\":" << metrics.nand_erases
           << ",\"gc_cycles\":" << metrics.gc_cycles
           << ",\"gc_moved_pages\":" << metrics.gc_moved_pages
           << ",\"gc_reclaimed_pages\":" << metrics.gc_reclaimed_pages
           << ",\"gc_failures\":" << metrics.gc_failures
           << ",\"trim_requests\":" << metrics.trim_requests << '}';
}
}  // namespace

ValidationReport run_validation(std::uint32_t seed, std::size_t operations) {
    if (operations == 0 || operations > 1000000) {
        throw std::invalid_argument("operations must be between 1 and 1000000");
    }
    PageFtl ftl({8, 8, 128}, 40);
    Oracle oracle(40);
    std::mt19937 random(seed);
    ValidationReport report;
    report.seed = seed; report.operations = operations;
    for (std::size_t step = 0; step < operations; ++step) {
        try {
            const auto lba = static_cast<std::size_t>(random() % 42);
            const auto operation = random() % 10;
            if (operation < 6) {
                auto bytes = pattern(random, 128);
                if (random() % 13 == 0) { bytes.pop_back(); }
                const auto expected = lba >= oracle.size() ? FtlError::LbaOutOfRange
                    : bytes.size() != 128 ? FtlError::DataSizeMismatch : FtlError::None;
                expect(ftl.write(lba, bytes), expected);
                if (expected == FtlError::None) { oracle[lba] = bytes; ++report.writes; }
                else { ++report.rejected; }
            } else if (operation < 8) {
                Bytes bytes{0x42};
                const auto expected = lba >= oracle.size() ? FtlError::LbaOutOfRange
                    : !oracle[lba] ? FtlError::UnwrittenLba : FtlError::None;
                expect(ftl.read(lba, bytes), expected);
                check(bytes == (expected == FtlError::None ? *oracle[lba] : Bytes({0x42})), "read output mismatch");
                if (expected == FtlError::None) { ++report.reads; } else { ++report.rejected; }
            } else if (operation == 8) {
                const auto expected = lba >= oracle.size() ? FtlError::LbaOutOfRange : FtlError::None;
                expect(ftl.trim(lba), expected);
                if (expected == FtlError::None) { oracle[lba].reset(); ++report.trims; }
                else { ++report.rejected; }
            } else {
                ++report.manual_gc_requests;
                const auto result = ftl.collect_garbage();
                check(result.ok() || result.error == FtlError::NoReclaimableBlock, "manual GC failed");
            }
            verify_all(ftl, oracle);
            report.logical_checks += oracle.size();
        } catch (const std::exception& error) {
            throw std::runtime_error("validation seed=" + std::to_string(seed) +
                " step=" + std::to_string(step) + ": " + error.what());
        }
    }
    report.metrics = ftl.metrics();
    return report;
}

std::vector<BenchmarkReport> run_benchmarks(std::uint32_t seed, std::size_t writes) {
    if (writes == 0 || writes > 1000000) {
        throw std::invalid_argument("writes must be between 1 and 1000000");
    }
    std::vector<BenchmarkReport> reports;
    for (const bool sequential : {true, false}) {
        PageFtl ftl;
        Oracle oracle(ftl.logical_page_count());
        std::mt19937 random(seed);
        for (std::size_t lba = 0; lba < oracle.size(); ++lba) {
            oracle[lba] = pattern(random, 4096);
            expect(ftl.write(lba, *oracle[lba]));
        }
        const auto baseline = ftl.metrics();
        std::vector<double> latencies;
        latencies.reserve(writes);
        const auto start = std::chrono::steady_clock::now();
        for (std::size_t step = 0; step < writes; ++step) {
            const auto lba = sequential ? step % oracle.size()
                : static_cast<std::size_t>(random() % oracle.size());
            auto bytes = pattern(random, 4096);
            const auto call_start = std::chrono::steady_clock::now();
            expect(ftl.write(lba, bytes));
            const auto call_end = std::chrono::steady_clock::now();
            latencies.push_back(std::chrono::duration<double, std::micro>(call_end - call_start).count());
            oracle[lba] = std::move(bytes);
        }
        const auto end = std::chrono::steady_clock::now();
        // Integrity verification is outside the measured interval.
        verify_all(ftl, oracle);
        const auto metrics = ftl.metrics();
        check(metrics.host_writes - baseline.host_writes == writes, "benchmark write accounting mismatch");
        const auto elapsed = std::chrono::duration<double>(end - start).count();
        check(elapsed > 0, "benchmark clock did not advance");
        std::sort(latencies.begin(), latencies.end());
        const auto percentile = [&latencies](std::size_t percent) {
            return latencies[(latencies.size() * percent + 99) / 100 - 1];
        };
        BenchmarkReport report;
        report.workload = sequential ? "sequential_updates" : "random_updates";
        report.seed = seed; report.measured_writes = writes;
        report.nand_programs = metrics.nand_programs - baseline.nand_programs;
        report.gc_cycles = metrics.gc_cycles - baseline.gc_cycles;
        report.gc_moved_pages = metrics.gc_moved_pages - baseline.gc_moved_pages;
        report.elapsed_seconds = elapsed;
        report.writes_per_second = static_cast<double>(writes) / elapsed;
        report.throughput_mib_s = static_cast<double>(writes) * 4096.0 / elapsed / (1024.0 * 1024.0);
        report.latency_p50_us = percentile(50); report.latency_p95_us = percentile(95);
        report.latency_p99_us = percentile(99);
        report.write_amplification = static_cast<double>(report.nand_programs) / static_cast<double>(writes);
        reports.push_back(report);
    }
    return reports;
}

std::string json_escape(std::string_view text) {
    std::ostringstream output;
    for (const char character : text) {
        const auto byte = static_cast<unsigned char>(character);
        if (character == '"' || character == '\\') { output << '\\' << character; }
        else if (byte < 0x20) { output << "\\u" << std::hex << std::setfill('0') << std::setw(4) << static_cast<unsigned>(byte) << std::dec; }
        else { output << character; }
    }
    return output.str();
}

std::string validation_json(const ValidationReport& report) {
    std::ostringstream output;
    output << "{\"status\":\"pass\",\"kind\":\"validation\",";
    json_environment(output);
    output << ",\"seed\":" << report.seed << ",\"operations\":" << report.operations
           << ",\"logical_pages\":40,\"physical_pages\":64,\"page_bytes\":128"
           << ",\"writes\":" << report.writes << ",\"reads\":" << report.reads
           << ",\"trims\":" << report.trims << ",\"rejected\":" << report.rejected
           << ",\"manual_gc_requests\":" << report.manual_gc_requests
           << ",\"logical_checks\":" << report.logical_checks << ",\"metrics\":";
    json_metrics(output, report.metrics);
    output << "}\n";
    return output.str();
}

std::string benchmark_json(const std::vector<BenchmarkReport>& reports) {
    std::ostringstream output;
    output << std::setprecision(10) << "{\"status\":\"pass\",\"kind\":\"benchmark\",";
    json_environment(output);
    output << ",\"logical_pages\":192,\"physical_pages\":256,\"page_bytes\":4096,"
           << "\"timing_scope\":\"RAM simulator; setup and final verification excluded; loop includes payload generation\",\"workloads\":[";
    bool first = true;
    for (const auto& report : reports) {
        if (!first) { output << ','; } first = false;
        output << "{\"name\":\"" << report.workload << "\",\"seed\":" << report.seed
               << ",\"measured_writes\":" << report.measured_writes
               << ",\"nand_programs\":" << report.nand_programs << ",\"gc_cycles\":" << report.gc_cycles
               << ",\"gc_moved_pages\":" << report.gc_moved_pages
               << ",\"elapsed_seconds\":" << report.elapsed_seconds
               << ",\"writes_per_second\":" << report.writes_per_second
               << ",\"throughput_mib_s\":" << report.throughput_mib_s
               << ",\"latency_p50_us\":" << report.latency_p50_us
               << ",\"latency_p95_us\":" << report.latency_p95_us
               << ",\"latency_p99_us\":" << report.latency_p99_us
               << ",\"write_amplification\":" << report.write_amplification << '}';
    }
    output << "]}\n";
    return output.str();
}

}  // namespace minissd
