#pragma once

#include "minissd/nand_flash.hpp"

#include <optional>

namespace minissd {

struct PhysicalAddress {
    std::size_t block = 0;
    std::size_t page = 0;
    bool operator==(const PhysicalAddress&) const = default;
};

enum class PageState { Free, Valid, Invalid };
[[nodiscard]] std::string_view describe(PageState state) noexcept;

struct PhysicalPageInfo {
    PageState state = PageState::Free;
    // Only a Valid page has a logical owner; stale data is not mapped.
    std::optional<std::size_t> lba;
    bool operator==(const PhysicalPageInfo&) const = default;
};

struct FtlStats {
    std::size_t free_pages = 0;
    std::size_t valid_pages = 0;
    std::size_t invalid_pages = 0;
    bool operator==(const FtlStats&) const = default;
};

enum class FtlError {
    None,
    LbaOutOfRange,
    UnwrittenLba,
    DataSizeMismatch,
    NoFreePage,
    NandFailure,
    GcDisabled,
    NoReclaimableBlock,
};

enum class GcMode { Automatic, Disabled };

struct FtlMetrics {
    std::uint64_t host_writes = 0;
    std::uint64_t nand_programs = 0;
    std::uint64_t nand_erases = 0;
    std::uint64_t gc_cycles = 0;
    std::uint64_t gc_moved_pages = 0;
    std::uint64_t gc_reclaimed_pages = 0;
    std::uint64_t gc_failures = 0;
    std::uint64_t trim_requests = 0;
};

[[nodiscard]] std::string_view describe(FtlError error) noexcept;

struct FtlStatus {
    FtlError error = FtlError::None;
    NandError nand_error = NandError::None;
    [[nodiscard]] bool ok() const noexcept { return error == FtlError::None; }
};

// Synchronous, in-memory page-mapped FTL. Automatic GC reserves one entire
// block; Disabled preserves the append-only model for comparison/teaching.
class PageFtl {
public:
    // Automatic GC requires >= 2 blocks and logical_pages < non-reserved pages.
    // Disabled only requires 0 < logical_pages < physical pages.
    // The default is 192 logical pages on 256 physical pages (768 KiB / 1 MiB).
    explicit PageFtl(NandGeometry geometry = {}, std::size_t logical_pages = 192,
                     GcMode gc_mode = GcMode::Automatic);
    PageFtl(const PageFtl&) = delete;
    PageFtl& operator=(const PageFtl&) = delete;
    PageFtl(PageFtl&&) = delete;
    PageFtl& operator=(PageFtl&&) = delete;

    [[nodiscard]] std::size_t logical_page_count() const noexcept;
    [[nodiscard]] std::size_t logical_capacity_bytes() const noexcept;
    [[nodiscard]] std::size_t physical_page_count() const noexcept;

    // Unwritten LBAs return UnwrittenLba instead of synthesizing data.
    // Every returned read error leaves output unchanged in this model.
    [[nodiscard]] FtlStatus read(
        std::size_t lba, std::vector<std::uint8_t>& output) const;

    // Program a free page FIRST; publish the new mapping only on success.
    // Returned errors preserve logical data. Successful GC may relocate old
    // mappings even if the subsequent host program fails.
    [[nodiscard]] FtlStatus write(
        std::size_t lba, const std::vector<std::uint8_t>& data);
    [[nodiscard]] FtlStatus trim(std::size_t lba);
    [[nodiscard]] FtlStatus collect_garbage();

    // Read-only diagnostics. Null means unwritten/invalid LBA or address.
    [[nodiscard]] std::optional<PhysicalAddress> physical_address(
        std::size_t lba) const noexcept;
    [[nodiscard]] std::optional<PhysicalPageInfo> page_info(
        PhysicalAddress address) const noexcept;
    [[nodiscard]] FtlStats stats() const noexcept;
    [[nodiscard]] const NandFlash& nand() const noexcept;
    [[nodiscard]] std::optional<std::size_t> reserved_block() const noexcept;
    [[nodiscard]] std::size_t available_free_pages() const noexcept;
    [[nodiscard]] FtlMetrics metrics() const noexcept;
    [[nodiscard]] double write_amplification() const noexcept;
    void inject_failure(NandOperation operation, std::size_t skip = 0);
    void clear_failures() noexcept;

private:
    [[nodiscard]] PhysicalAddress address_of(std::size_t index) const noexcept;
    [[nodiscard]] std::optional<std::size_t> find_free_page() const noexcept;
    [[nodiscard]] NandError program(std::size_t index,
        const std::vector<std::uint8_t>& data);
    [[nodiscard]] NandError erase(std::size_t block);
    [[nodiscard]] NandError clean_reserve();
    void mark_block_free(std::size_t block) noexcept;

    // Owning the NAND prevents callers from erasing/programming behind the
    // mapping table. Diagnostics only expose a const reference.
    NandFlash nand_;
    std::vector<std::optional<std::size_t>> logical_to_physical_;
    std::vector<PhysicalPageInfo> pages_;
    std::size_t allocation_cursor_ = 0;
    std::optional<std::size_t> reserved_block_;
    bool reserve_needs_cleanup_ = false;
    FtlMetrics metrics_;
};

}  // namespace minissd
