#include "minissd/ftl.hpp"

#include <cmath>
#include <functional>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>

namespace {
using namespace minissd;
using Bytes = std::vector<std::uint8_t>;
using Oracle = std::vector<std::optional<Bytes>>;

void require(bool value, const std::string& message) {
    if (!value) { throw std::runtime_error(message); }
}
void expect(FtlStatus result, FtlError expected = FtlError::None,
            NandError native = NandError::None) {
    require(result.error == expected && result.nand_error == native,
        std::string(describe(result.error)) + ": " + std::string(describe(result.nand_error)));
}
Bytes read(const PageFtl& ftl, std::size_t lba) {
    Bytes bytes;
    expect(ftl.read(lba, bytes));
    return bytes;
}
void verify(const PageFtl& ftl, const Oracle& oracle) {
    const auto& geometry = ftl.nand().geometry();
    std::vector<bool> mapped(ftl.physical_page_count(), false);
    std::size_t valid = 0;
    for (std::size_t lba = 0; lba < oracle.size(); ++lba) {
        Bytes bytes{0xDE, 0xAD};
        if (!oracle[lba]) {
            expect(ftl.read(lba, bytes), FtlError::UnwrittenLba);
            require(bytes == Bytes({0xDE, 0xAD}) && !ftl.physical_address(lba), "unmapped read changed output");
            continue;
        }
        expect(ftl.read(lba, bytes));
        require(bytes == *oracle[lba], "logical data mismatch");
        const auto address = ftl.physical_address(lba).value();
        const auto index = address.block * geometry.pages_per_block + address.page;
        require(!mapped[index], "two LBAs share a page");
        mapped[index] = true;
        require(ftl.page_info(address) == PhysicalPageInfo{PageState::Valid, lba}, "reverse mapping mismatch");
        ++valid;
    }
    FtlStats counted;
    for (std::size_t block = 0; block < geometry.block_count; ++block) {
        for (std::size_t page = 0; page < geometry.pages_per_block; ++page) {
            const auto index = block * geometry.pages_per_block + page;
            const auto info = ftl.page_info({block, page}).value();
            if (info.state == PageState::Free) {
                ++counted.free_pages;
                Bytes raw;
                require(ftl.nand().read_page(block, page, raw) == NandError::None &&
                        raw == Bytes(geometry.bytes_per_page, 0xFF), "free page is not erased");
            } else if (info.state == PageState::Invalid) { ++counted.invalid_pages; }
            else { ++counted.valid_pages; }
            require((info.state == PageState::Valid) == mapped[index], "page state has no corresponding mapping");
            if (info.state != PageState::Valid) { require(!info.lba, "non-valid page has an owner"); }
            if (ftl.reserved_block() == block) { require(info.state != PageState::Valid, "reserve contains live data"); }
        }
    }
    require(counted == ftl.stats() && valid == counted.valid_pages, "page statistics mismatch");
    require(counted.free_pages + counted.valid_pages + counted.invalid_pages == ftl.physical_page_count(),
            "page accounting mismatch");
}
Oracle fill_for_migration(PageFtl& ftl) {
    Oracle oracle(6);
    for (std::size_t lba = 0; lba < 6; ++lba) {
        oracle[lba] = Bytes(8, static_cast<std::uint8_t>(lba + 1));
        expect(ftl.write(lba, *oracle[lba]));
    }
    oracle[0] = Bytes(8, 0xA0); oracle[1] = Bytes(8, 0xA1);
    expect(ftl.write(0, *oracle[0])); expect(ftl.write(1, *oracle[1]));
    require(ftl.available_free_pages() == 0 && ftl.stats() == FtlStats{4, 6, 2}, "bad migration fixture");
    return oracle;
}
void verify_old_mappings(const PageFtl& ftl, const std::vector<std::optional<PhysicalAddress>>& old) {
    for (std::size_t lba = 0; lba < old.size(); ++lba) {
        require(ftl.physical_address(lba) == old[lba], "failed GC changed old mapping");
    }
}
auto mappings(const PageFtl& ftl) {
    std::vector<std::optional<PhysicalAddress>> result;
    for (std::size_t lba = 0; lba < ftl.logical_page_count(); ++lba) { result.push_back(ftl.physical_address(lba)); }
    return result;
}
void reserved_capacity() {
    PageFtl ftl({3, 4, 8}, 6);
    require(ftl.reserved_block() == 2 && ftl.available_free_pages() == 8, "wrong reservation");
    expect(ftl.collect_garbage(), FtlError::NoReclaimableBlock);
    for (const auto geometry : {NandGeometry{1, 4, 8}, NandGeometry{2, 4, 8}}) {
        bool rejected = false;
        try { PageFtl invalid(geometry, 4); } catch (const std::invalid_argument&) { rejected = true; }
        require(rejected, "unsafe GC geometry accepted");
    }
    PageFtl disabled({1, 4, 8}, 2, GcMode::Disabled);
    expect(disabled.collect_garbage(), FtlError::GcDisabled);
}
void automatic_migration_preserves_every_lba() {
    PageFtl ftl({3, 4, 8}, 6);
    auto oracle = fill_for_migration(ftl);
    oracle[2] = Bytes(8, 0xB2);
    expect(ftl.write(2, *oracle[2]));
    verify(ftl, oracle);
    const auto metrics = ftl.metrics();
    require(ftl.reserved_block() == 0 && ftl.stats() == FtlStats{5, 6, 1}, "wrong block swap");
    require(metrics.host_writes == 9 && metrics.nand_programs == 11 && metrics.gc_cycles == 1 &&
            metrics.gc_moved_pages == 2 && metrics.nand_erases == 1 && metrics.gc_reclaimed_pages == 2,
            "incorrect physical operation metrics");
    require(std::abs(ftl.write_amplification() - 11.0 / 9.0) < 1e-12, "incorrect write amplification");
}
void trim_and_all_invalid_block() {
    PageFtl ftl({3, 4, 8}, 6);
    for (std::uint8_t value = 1; value <= 4; ++value) { expect(ftl.write(0, Bytes(8, value))); }
    expect(ftl.trim(0)); expect(ftl.trim(0));
    require(ftl.stats() == FtlStats{8, 0, 4}, "Trim physically freed a page");
    expect(ftl.trim(6), FtlError::LbaOutOfRange);
    expect(ftl.collect_garbage());
    verify(ftl, Oracle(6));
    require(ftl.stats() == FtlStats{12, 0, 0} && ftl.metrics().gc_moved_pages == 0 &&
            ftl.metrics().trim_requests == 2, "all-invalid GC did not reclaim whole block");
    expect(ftl.write(0, Bytes(8, 0xAA)));
    require(read(ftl, 0) == Bytes(8, 0xAA), "trimmed LBA could not be rewritten");
}
void host_program_failure_preserves_data() {
    PageFtl ftl({3, 4, 8}, 6);
    expect(ftl.write(0, Bytes(8, 0xAA)));
    const auto old = mappings(ftl);
    const auto before = ftl.stats();
    ftl.inject_failure(NandOperation::Program);
    expect(ftl.write(0, Bytes(8, 0xBB)), FtlError::NandFailure, NandError::InjectedProgramFailure);
    verify_old_mappings(ftl, old);
    require(read(ftl, 0) == Bytes(8, 0xAA) && ftl.stats() == before, "failed host program changed data");
    expect(ftl.write(0, Bytes(8, 0xBB)));
}
void gc_read_failure_preserves_data() {
    PageFtl ftl({3, 4, 8}, 6);
    const auto oracle = fill_for_migration(ftl);
    const auto old = mappings(ftl);
    ftl.inject_failure(NandOperation::Read);
    expect(ftl.collect_garbage(), FtlError::NandFailure, NandError::InjectedReadFailure);
    verify_old_mappings(ftl, old); verify(ftl, oracle);
    require(ftl.metrics().gc_cycles == 0 && ftl.metrics().gc_failures == 1, "read failure metric mismatch");
    expect(ftl.collect_garbage()); verify(ftl, oracle);
}
void gc_program_failure_rolls_back_staging() {
    PageFtl ftl({3, 4, 8}, 6);
    const auto oracle = fill_for_migration(ftl);
    const auto old = mappings(ftl);
    ftl.inject_failure(NandOperation::Program, 1);
    expect(ftl.collect_garbage(), FtlError::NandFailure, NandError::InjectedProgramFailure);
    verify_old_mappings(ftl, old); verify(ftl, oracle);
    require(ftl.stats() == FtlStats{4, 6, 2} && ftl.metrics().gc_cycles == 0 &&
            ftl.metrics().nand_erases == 1, "staging cleanup failed");
    expect(ftl.collect_garbage()); verify(ftl, oracle);
}
void victim_erase_failure_rolls_back_mapping() {
    PageFtl ftl({3, 4, 8}, 6);
    const auto oracle = fill_for_migration(ftl);
    const auto old = mappings(ftl);
    ftl.inject_failure(NandOperation::Erase);
    expect(ftl.collect_garbage(), FtlError::NandFailure, NandError::InjectedEraseFailure);
    verify_old_mappings(ftl, old); verify(ftl, oracle);
    require(ftl.stats() == FtlStats{4, 6, 2}, "failed victim erase changed metadata");
    expect(ftl.collect_garbage()); verify(ftl, oracle);
}
void failed_cleanup_is_retried() {
    PageFtl ftl({3, 4, 8}, 6);
    const auto oracle = fill_for_migration(ftl);
    const auto old = mappings(ftl);
    ftl.inject_failure(NandOperation::Program, 1);
    ftl.inject_failure(NandOperation::Erase);
    expect(ftl.collect_garbage(), FtlError::NandFailure, NandError::InjectedEraseFailure);
    verify_old_mappings(ftl, old); verify(ftl, oracle);
    require(ftl.stats() == FtlStats{3, 6, 3} && ftl.available_free_pages() == 0,
            "unclean staging pages were exposed as free");
    expect(ftl.collect_garbage()); verify(ftl, oracle);
    require(ftl.metrics().nand_erases == 2, "cleanup retry did not precede successful GC");
}
void host_failure_after_gc_keeps_logical_values() {
    PageFtl ftl({3, 4, 8}, 6);
    const auto oracle = fill_for_migration(ftl);
    ftl.inject_failure(NandOperation::Program, 2);
    expect(ftl.write(2, Bytes(8, 0xBB)), FtlError::NandFailure, NandError::InjectedProgramFailure);
    verify(ftl, oracle);
    require(ftl.metrics().gc_cycles == 1 && ftl.metrics().host_writes == 8, "host failure miscounted as success");
    expect(ftl.write(2, Bytes(8, 0xBB)));
}
void nand_faults_only_consume_valid_operations() {
    NandFlash nand({1, 2, 8});
    Bytes output{0x42};
    nand.inject_failure(NandOperation::Read);
    require(nand.read_page(1, 0, output) == NandError::BlockOutOfRange, "invalid read accepted");
    require(nand.read_page(0, 0, output) == NandError::InjectedReadFailure && output == Bytes({0x42}),
            "injected read changed output");
    require(nand.read_page(0, 0, output) == NandError::None, "read hook was not one-shot");
    nand.inject_failure(NandOperation::Program);
    require(nand.program_page(0, 0, Bytes(7)) == NandError::DataSizeMismatch, "bad payload accepted");
    require(nand.program_page(0, 0, Bytes(8, 0)) == NandError::InjectedProgramFailure, "invalid payload consumed hook");
    require(nand.program_page(0, 0, Bytes(8, 0)) == NandError::None, "retry program failed");
    nand.inject_failure(NandOperation::Erase);
    require(nand.erase_block(1) == NandError::BlockOutOfRange &&
            nand.erase_block(0) == NandError::InjectedEraseFailure, "invalid erase consumed hook");
    require(nand.read_page(0, 0, output) == NandError::None && output == Bytes(8, 0), "failed erase changed bytes");
    nand.inject_failure(NandOperation::Program); nand.clear_failures();
    require(nand.program_page(0, 1, Bytes(8, 0xFF)) == NandError::None, "clear hook failed");
}
void tiny_and_dense_geometries_keep_making_progress() {
    for (const auto geometry : {NandGeometry{3, 1, 8}, NandGeometry{3, 2, 8}}) {
        const auto logical = (geometry.block_count - 1) * geometry.pages_per_block - 1;
        PageFtl ftl(geometry, logical);
        Oracle oracle(logical);
        for (std::size_t step = 0; step < 1000; ++step) {
            const auto lba = step % logical;
            oracle[lba] = Bytes(8, static_cast<std::uint8_t>(step & 0xFFU));
            expect(ftl.write(lba, *oracle[lba])); verify(ftl, oracle);
        }
        require(ftl.metrics().gc_cycles > 0, "tiny geometry did not recycle");
    }
}
void random_updates_trim_and_gc() {
    for (const std::uint32_t seed : {0x474331U, 42U, 20261010U}) {
        PageFtl ftl({4, 4, 32}, 10);
        Oracle oracle(10);
        std::mt19937 random(seed);
        for (std::size_t step = 0; step < 10000; ++step) {
            try {
                const auto lba = static_cast<std::size_t>(random() % oracle.size());
                if (random() % 5 == 0) { expect(ftl.trim(lba)); oracle[lba].reset(); }
                else {
                    Bytes bytes(32);
                    for (auto& byte : bytes) { byte = static_cast<std::uint8_t>(random() & 0xFFU); }
                    expect(ftl.write(lba, bytes)); oracle[lba] = bytes;
                }
                verify(ftl, oracle);
            } catch (const std::exception& error) {
                throw std::runtime_error("seed=" + std::to_string(seed) + " step=" +
                                         std::to_string(step) + ": " + error.what());
            }
        }
        require(ftl.metrics().gc_cycles > 0, "random workload did not exercise GC");
    }
}
}  // namespace

int main() {
    const std::vector<std::pair<std::string, std::function<void()>>> tests{
        {"reserved block and safe logical capacity", reserved_capacity},
        {"automatic GC moves live pages and preserves all LBAs", automatic_migration_preserves_every_lba},
        {"Trim is idempotent and all-invalid block needs no copies", trim_and_all_invalid_block},
        {"host program failure preserves old data", host_program_failure_preserves_data},
        {"GC read failure preserves old data", gc_read_failure_preserves_data},
        {"partial GC program failure cleans staging copies", gc_program_failure_rolls_back_staging},
        {"victim erase failure restores old mappings", victim_erase_failure_rolls_back_mapping},
        {"failed rollback cleanup is retried safely", failed_cleanup_is_retried},
        {"host failure after successful GC preserves logical values", host_failure_after_gc_keeps_logical_values},
        {"NAND one-shot fault hooks preserve bytes and outputs", nand_faults_only_consume_valid_operations},
        {"tiny and dense devices sustain 1,000 updates", tiny_and_dense_geometries_keep_making_progress},
        {"GC oracle: 3 seeds x 10,000 updates and Trim", random_updates_trim_and_gc},
    };
    std::size_t failures = 0;
    for (const auto& [name, test] : tests) {
        try { test(); std::cout << "[PASS] " << name << '\n'; }
        catch (const std::exception& error) { ++failures; std::cerr << "[FAIL] " << name << ": " << error.what() << '\n'; }
    }
    std::cout << '\n' << tests.size() - failures << '/' << tests.size() << " GC tests passed.\n";
    return failures == 0 ? 0 : 1;
}
