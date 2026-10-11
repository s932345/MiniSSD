#include "minissd/workloads.hpp"

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

void expect(minissd::NandError actual, minissd::NandError expected) {
    if (actual != expected) {
        throw std::runtime_error(std::string(minissd::describe(actual)));
    }
}

void expect(minissd::FtlStatus status, minissd::FtlError expected = minissd::FtlError::None) {
    if (status.error != expected) {
        throw std::runtime_error(std::string(minissd::describe(status.error)) +
                                 ": " + std::string(minissd::describe(status.nand_error)));
    }
}

void show_bytes(const std::vector<std::uint8_t>& data) {
    for (const auto byte : data) {
        std::cout << std::hex << std::uppercase << std::setfill('0')
                  << std::setw(2) << static_cast<unsigned int>(byte) << ' ';
    }
    std::cout << std::dec << '\n';
}

void show_page(const minissd::NandFlash& nand, std::size_t block, std::size_t page) {
    std::vector<std::uint8_t> data;
    expect(nand.read_page(block, page, data), minissd::NandError::None);
    std::cout << "Block " << block << ", Page " << page << ": ";
    show_bytes(data);
}

std::vector<std::uint8_t> payload(std::string_view text, std::size_t size) {
    std::vector<std::uint8_t> data(size, 0);
    for (std::size_t index = 0; index < std::min(text.size(), size); ++index) {
        data[index] = static_cast<std::uint8_t>(text[index]);
    }
    return data;
}

void run_nand_demo() {
        // Tiny geometry makes every byte and the erase side effect visible.
        minissd::NandFlash nand({2, 4, 16});
        std::cout << "MiniSSD - Virtual NAND demo\n"
                  << "2 blocks x 4 pages x 16 bytes = " << nand.capacity_bytes()
                  << " bytes (teaching configuration)\n\n";

        std::cout << "[1] New NAND reads as erased bytes (FF).\n";
        show_page(nand, 0, 0);

        std::cout << "\n[2] Program A into Page 0 and KEEP into Page 1.\n";
        expect(nand.program_page(0, 0, payload("A", 16)), minissd::NandError::None);
        expect(nand.program_page(0, 1, payload("KEEP", 16)), minissd::NandError::None);
        expect(nand.program_page(1, 0, payload("OTHER BLOCK", 16)),
               minissd::NandError::None);
        show_page(nand, 0, 0);
        show_page(nand, 0, 1);

        std::cout << "\n[3] Try to overwrite A with B in the same page.\n";
        const auto overwrite = nand.program_page(0, 0, payload("B", 16));
        expect(overwrite, minissd::NandError::PageAlreadyProgrammed);
        std::cout << "Expected rejection: " << minissd::describe(overwrite) << '\n';
        show_page(nand, 0, 0);

        std::cout << "\n[4] Erase Block 0: both A and KEEP disappear.\n";
        expect(nand.erase_block(0), minissd::NandError::None);
        show_page(nand, 0, 0);
        show_page(nand, 0, 1);
        std::cout << "The other block is preserved:\n";
        show_page(nand, 1, 0);

        std::cout << "\n[5] The erased page can now be programmed with B.\n";
        expect(nand.program_page(0, 0, payload("B", 16)), minissd::NandError::None);
        show_page(nand, 0, 0);
        std::uint64_t erase_count = 0;
        expect(nand.get_erase_count(0, erase_count), minissd::NandError::None);
        std::cout << "Block 0 erase count: " << erase_count << '\n';

        std::cout << "\nWhy FTL is needed: write updated data to a free page,\n"
                  << "then update the logical-to-physical mapping. GC must move\n"
                  << "live pages before erasing their block.\n";
}

void show_lba(const minissd::PageFtl& ftl, std::size_t lba) {
    std::vector<std::uint8_t> data;
    expect(ftl.read(lba, data));
    const auto address = ftl.physical_address(lba).value();
    std::cout << "LBA " << lba << " -> Block " << address.block
              << ", Page " << address.page << ": ";
    show_bytes(data);
}

void show_stats(const minissd::PageFtl& ftl) {
    const auto stats = ftl.stats();
    std::cout << "Physical pages: Free=" << stats.free_pages
              << " Valid=" << stats.valid_pages << " Invalid=" << stats.invalid_pages << '\n';
}

void show_page_states(const minissd::PageFtl& ftl) {
    const auto& geometry = ftl.nand().geometry();
    for (std::size_t block = 0; block < geometry.block_count; ++block) {
        for (std::size_t page = 0; page < geometry.pages_per_block; ++page) {
            const auto info = ftl.page_info({block, page}).value();
            std::cout << "  Block " << block << ", Page " << page
                      << ": " << minissd::describe(info.state);
            if (info.lba) { std::cout << " (LBA " << *info.lba << ')'; }
            std::cout << '\n';
        }
    }
}

void run_ftl_demo() {
    minissd::PageFtl ftl({2, 3, 16}, 3, minissd::GcMode::Disabled);
    std::cout << "\nMiniSSD - Page-mapped FTL demo\n"
              << "3 logical pages / 6 physical pages; one LBA = 16 bytes\n\n";

    std::cout << "[1] Write A to LBA 0, KEEP to LBA 1.\n";
    expect(ftl.write(0, payload("A", 16)));
    expect(ftl.write(1, payload("KEEP", 16)));
    const auto original = ftl.physical_address(0).value();
    show_lba(ftl, 0);
    show_lba(ftl, 1);
    show_stats(ftl);

    std::cout << "\n[2] Update LBA 0 to B: program a free page, then switch the mapping.\n";
    expect(ftl.write(0, payload("B", 16)));
    show_lba(ftl, 0);
    show_lba(ftl, 1);
    show_stats(ftl);
    show_page_states(ftl);

    std::cout << "\n[3] A is still physically present, but its page is Invalid.\n";
    show_page(ftl.nand(), original.block, original.page);
    std::cout << "Host reads LBA 0 and receives B. No block was erased.\n";

    std::cout << "\n[4] Repeated updates consume the remaining free pages.\n";
    for (const auto version : {"V1", "V2", "V3"}) {
        expect(ftl.write(2, payload(version, 16)));
    }
    show_lba(ftl, 2);
    show_stats(ftl);

    std::cout << "\n[5] Try V4 when full: reject the write and preserve V3.\n";
    const auto full = ftl.write(2, payload("V4", 16));
    expect(full, minissd::FtlError::NoFreePage);
    std::cout << "Expected rejection: " << minissd::describe(full.error) << '\n';
    show_lba(ftl, 2);
    show_lba(ftl, 1);
    show_stats(ftl);
    std::cout << "\nWhy GC comes next: Invalid pages still occupy NAND. Move live\n"
              << "pages before block erase to reclaim space safely. GC is not implemented yet.\n";
}

void show_metrics(const minissd::PageFtl& ftl) {
    const auto metrics = ftl.metrics();
    std::cout << "Host writes=" << metrics.host_writes << " NAND programs=" << metrics.nand_programs
              << " GC cycles=" << metrics.gc_cycles << " moved pages=" << metrics.gc_moved_pages
              << " write amplification=" << std::fixed << std::setprecision(3)
              << ftl.write_amplification() << '\n';
}

void run_gc_demo() {
    minissd::PageFtl ftl({3, 4, 16}, 6);
    std::vector<std::vector<std::uint8_t>> expected;
    std::cout << "MiniSSD MVP - writes, updates, garbage collection, Trim, verification\n"
              << "6 logical pages / 12 physical pages; one 4-page block reserved for GC\n\n";
    for (std::size_t lba = 0; lba < 6; ++lba) {
        expected.push_back(payload("KEEP-" + std::to_string(lba), 16));
        expect(ftl.write(lba, expected.back()));
    }
    expected[0] = payload("UPDATE-0", 16); expected[1] = payload("UPDATE-1", 16);
    expect(ftl.write(0, expected[0])); expect(ftl.write(1, expected[1]));
    std::cout << "[1] Active space is full; the reserved block is still empty.\n";
    show_stats(ftl); show_page_states(ftl);
    std::cout << "Available host pages=" << ftl.available_free_pages() << '\n';

    std::cout << "\n[2] Update LBA 2: automatic GC copies live pages, erases the victim,\n"
              << "and swaps the reserved block. The host write then succeeds.\n";
    expected[2] = payload("NEW-2", 16); expect(ftl.write(2, expected[2]));
    show_stats(ftl); show_metrics(ftl); show_page_states(ftl);

    std::cout << "\n[3] Perform 1,000 more updates to LBA 0.\n";
    for (std::size_t update = 0; update < 1000; ++update) {
        expected[0] = payload("HOT-" + std::to_string(update), 16);
        expect(ftl.write(0, expected[0]));
    }
    for (std::size_t lba = 0; lba < expected.size(); ++lba) {
        std::vector<std::uint8_t> bytes;
        expect(ftl.read(lba, bytes));
        if (bytes != expected[lba]) { throw std::runtime_error("GC demo data mismatch"); }
    }
    std::cout << "PASS: newest data and every neighboring LBA match the oracle.\n";
    show_metrics(ftl);

    std::cout << "\n[4] Trim LBA 4, collect, then write it again.\n";
    expect(ftl.trim(4));
    std::vector<std::uint8_t> bytes{0x42};
    expect(ftl.read(4, bytes), minissd::FtlError::UnwrittenLba);
    expect(ftl.collect_garbage());
    expected[4] = payload("REUSED", 16); expect(ftl.write(4, expected[4]));
    show_lba(ftl, 4);

    std::cout << "\n[5] Inject one program failure: reject BAD and preserve the last good value.\n";
    ftl.inject_failure(minissd::NandOperation::Program);
    const auto failure = ftl.write(0, payload("BAD", 16));
    expect(failure, minissd::FtlError::NandFailure);
    std::cout << "Expected failure: " << minissd::describe(failure.nand_error) << '\n';
    for (std::size_t lba = 0; lba < expected.size(); ++lba) {
        expect(ftl.read(lba, bytes));
        if (bytes != expected[lba]) { throw std::runtime_error("failed write changed logical data"); }
    }
    std::cout << "PASS: all logical data preserved. MVP demo completed.\n";
    show_stats(ftl); show_metrics(ftl);
}

struct CliOptions {
    std::uint32_t seed = 20261010U;
    std::size_t count = 0;
    std::string report;
};
CliOptions parse_options(int argc, char* argv[], std::string_view count_option, std::size_t default_count) {
    CliOptions result;
    result.count = default_count;
    for (int index = 2; index < argc; index += 2) {
        if (index + 1 >= argc) { throw std::invalid_argument("option requires a value"); }
        const std::string_view option(argv[index]);
        const std::string_view value(argv[index + 1]);
        if (option == "--report") {
            if (value.empty()) { throw std::invalid_argument("report path is empty"); }
            result.report = value;
            continue;
        }
        if (option != "--seed" && option != count_option) { throw std::invalid_argument("unknown option"); }
        std::uint64_t number = 0;
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
        if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
            throw std::invalid_argument("numeric option must be an unsigned integer");
        }
        if (option == "--seed") {
            if (number > std::numeric_limits<std::uint32_t>::max()) { throw std::invalid_argument("seed exceeds uint32 range"); }
            result.seed = static_cast<std::uint32_t>(number);
        } else {
            if (number == 0 || number > 1000000) { throw std::invalid_argument("count must be 1..1000000"); }
            result.count = static_cast<std::size_t>(number);
        }
    }
    return result;
}
void write_report(const std::string& name, const std::string& json) {
    if (name.empty()) { return; }
    // CLI strings are UTF-8. The char8_t constructor avoids locale-dependent
    // conversion of Chinese file names on Windows.
    std::u8string utf8;
    utf8.reserve(name.size());
    for (const char byte : name) {
        utf8.push_back(static_cast<char8_t>(static_cast<unsigned char>(byte)));
    }
    const std::filesystem::path path(utf8);
    if (!path.parent_path().empty()) { std::filesystem::create_directories(path.parent_path()); }
    std::ofstream output(path, std::ios::binary);
    if (!output) { throw std::runtime_error("cannot create report file"); }
    output << json;
    output.close();
    if (!output) { throw std::runtime_error("cannot write report file"); }
    std::cout << "Report: " << name << '\n';
}
void usage() {
    std::cout << "Usage: minissd [demo|nand-demo|ftl-demo|gc-demo]\n"
              << "       minissd validate [--seed N] [--operations N] [--report PATH]\n"
              << "       minissd benchmark [--seed N] [--writes N] [--report PATH]\n";
}

}  // namespace

int run_application(int argc, char* argv[]) {
    const auto command = argc == 1 ? std::string_view("demo") : std::string_view(argv[1]);
    if (command == "--help" && argc == 2) { usage(); return 0; }
    const bool demo_command = command == "demo" || command == "gc-demo" ||
                              command == "nand-demo" || command == "ftl-demo";
    if ((!demo_command && command != "validate" && command != "benchmark") || (demo_command && argc > 2)) {
        usage();
        return 2;
    }
    CliOptions options;
    try {
        if (command == "nand-demo") { run_nand_demo(); }
        else if (command == "ftl-demo") { run_ftl_demo(); }
        else if (demo_command) { run_gc_demo(); }
        else if (command == "validate") {
            options = parse_options(argc, argv, "--operations", 20000);
            const auto result = minissd::run_validation(options.seed, options.count);
            std::cout << "PASS: " << result.operations << " operations, " << result.logical_checks
                      << " logical checks, " << result.metrics.gc_cycles << " GC cycles.\n";
            const auto json = minissd::validation_json(result);
            write_report(options.report, json);
            if (options.report.empty()) { std::cout << json; }
        } else {
            options = parse_options(argc, argv, "--writes", 5000);
            const auto results = minissd::run_benchmarks(options.seed, options.count);
            std::cout << "RAM simulator benchmark; values do not represent physical SSD performance.\n";
            for (const auto& result : results) {
                std::cout << result.workload << ": " << result.measured_writes << " verified writes, "
                          << std::fixed << std::setprecision(3) << "WAF=" << result.write_amplification
                          << ", GC=" << result.gc_cycles << ", " << result.throughput_mib_s
                          << " MiB/s, p95=" << result.latency_p95_us << " us\n";
            }
            const auto json = minissd::benchmark_json(results);
            write_report(options.report, json);
            if (options.report.empty()) { std::cout << json; }
        }
        return 0;
    } catch (const std::invalid_argument& error) {
        std::cerr << "Invalid argument: " << error.what() << '\n'; usage(); return 2;
    } catch (const std::exception& error) {
        std::cerr << "Run failed: " << error.what() << '\n';
        if (!options.report.empty()) {
            try { write_report(options.report, "{\"status\":\"fail\",\"error\":\"" +
                minissd::json_escape(error.what()) + "\"}\n"); } catch (...) { /* preserve original failure */ }
        }
        return 1;
    }
}

#if defined(_WIN32)
int wmain(int argc, wchar_t* argv[]) {
    try {
        std::vector<std::string> arguments;
        arguments.reserve(static_cast<std::size_t>(argc));
        for (int index = 0; index < argc; ++index) {
            const auto count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                argv[index], -1, nullptr, 0, nullptr, nullptr);
            if (count <= 0) { throw std::runtime_error("invalid Unicode argument"); }
            std::string utf8(static_cast<std::size_t>(count), '\0');
            if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[index], -1,
                    utf8.data(), count, nullptr, nullptr) != count) {
                throw std::runtime_error("cannot decode Unicode argument");
            }
            utf8.pop_back();  // remove the encoded terminator
            arguments.push_back(std::move(utf8));
        }
        std::vector<char*> pointers;
        pointers.reserve(arguments.size());
        for (auto& argument : arguments) { pointers.push_back(argument.data()); }
        return run_application(argc, pointers.data());
    } catch (const std::exception& error) {
        std::cerr << "Argument conversion failed: " << error.what() << '\n';
        return 2;
    }
}
#else
int main(int argc, char* argv[]) { return run_application(argc, argv); }
#endif
