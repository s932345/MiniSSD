#include "minissd/ftl.hpp"

#include <stdexcept>
#include <algorithm>
#include <utility>

namespace minissd {

std::string_view describe(PageState state) noexcept {
    switch (state) {
        case PageState::Free: return "Free";
        case PageState::Valid: return "Valid";
        case PageState::Invalid: return "Invalid";
    }
    return "unknown page state";
}

std::string_view describe(FtlError error) noexcept {
    switch (error) {
        case FtlError::None: return "success";
        case FtlError::LbaOutOfRange: return "LBA out of range";
        case FtlError::UnwrittenLba: return "LBA has not been written";
        case FtlError::DataSizeMismatch: return "data must fill exactly one logical page";
        case FtlError::NoFreePage: return "no available free page";
        case FtlError::NandFailure: return "NAND operation failed";
        case FtlError::GcDisabled: return "garbage collection is disabled";
        case FtlError::NoReclaimableBlock: return "no block contains reclaimable invalid pages";
    }
    return "unknown FTL error";
}

PageFtl::PageFtl(NandGeometry geometry, std::size_t logical_pages, GcMode gc_mode)
    : nand_(geometry) {
    // NAND already checked geometry overflow and allocated the byte storage.
    const auto physical_pages = nand_.capacity_bytes() / geometry.bytes_per_page;
    if (logical_pages == 0 || logical_pages >= physical_pages) {
        throw std::invalid_argument(
            "Logical pages must be positive and smaller than physical pages");
    }
    if (gc_mode == GcMode::Automatic) {
        if (geometry.block_count < 2 ||
            logical_pages >= physical_pages - geometry.pages_per_block) {
            throw std::invalid_argument(
                "GC needs a reserved block and at least one extra update page");
        }
        reserved_block_ = geometry.block_count - 1;
    }
    logical_to_physical_.resize(logical_pages);
    pages_.resize(physical_pages);
}

std::size_t PageFtl::logical_page_count() const noexcept {
    return logical_to_physical_.size();
}

std::size_t PageFtl::logical_capacity_bytes() const noexcept {
    // Logical pages are fewer than physical pages, whose byte size was checked.
    return logical_page_count() * nand_.geometry().bytes_per_page;
}

std::size_t PageFtl::physical_page_count() const noexcept { return pages_.size(); }

PhysicalAddress PageFtl::address_of(std::size_t index) const noexcept {
    return {index / nand_.geometry().pages_per_block,
            index % nand_.geometry().pages_per_block};
}

FtlStatus PageFtl::read(std::size_t lba, std::vector<std::uint8_t>& output) const {
    if (lba >= logical_page_count()) {
        return {FtlError::LbaOutOfRange};
    }
    const auto mapping = logical_to_physical_[lba];
    if (!mapping) {
        return {FtlError::UnwrittenLba};
    }

    const auto address = address_of(*mapping);
    const auto error = nand_.read_page(address.block, address.page, output);
    if (error != NandError::None) {
        return {FtlError::NandFailure, error};
    }
    return {};
}

FtlStatus PageFtl::write(std::size_t lba, const std::vector<std::uint8_t>& data) {
    if (lba >= logical_page_count()) {
        return {FtlError::LbaOutOfRange};
    }
    if (data.size() != nand_.geometry().bytes_per_page) {
        return {FtlError::DataSizeMismatch};
    }
    auto destination = find_free_page();
    if (!destination && reserved_block_) {
        const auto gc_status = collect_garbage();
        if (!gc_status.ok()) { return gc_status; }
        destination = find_free_page();
    }
    if (!destination) { return {FtlError::NoFreePage}; }
    const auto nand_error = program(*destination, data);
    if (nand_error != NandError::None) {
        // The NAND model leaves state unchanged on returned errors.
        return {FtlError::NandFailure, nand_error};
    }

    // All metadata was allocated at construction; these assignments cannot
    // allocate. In synchronous execution, only fully written data is mapped.
    const auto previous = logical_to_physical_[lba];
    pages_[*destination] = {PageState::Valid, lba};
    logical_to_physical_[lba] = *destination;
    if (previous) {
        pages_[*previous] = {PageState::Invalid, std::nullopt};
    }
    allocation_cursor_ = (*destination + 1) % pages_.size();
    ++metrics_.host_writes;
    return {};
}

FtlStatus PageFtl::trim(std::size_t lba) {
    if (lba >= logical_page_count()) { return {FtlError::LbaOutOfRange}; }
    if (const auto mapping = logical_to_physical_[lba]) {
        pages_[*mapping] = {PageState::Invalid, std::nullopt};
        logical_to_physical_[lba].reset();
    }
    ++metrics_.trim_requests;
    return {};
}

std::optional<std::size_t> PageFtl::find_free_page() const noexcept {
    auto index = allocation_cursor_;
    for (std::size_t checked = 0; checked < pages_.size(); ++checked) {
        const auto address = address_of(index);
        if (pages_[index].state == PageState::Free && address.block != reserved_block_) {
            return index;
        }
        index = (index + 1) % pages_.size();
    }
    return std::nullopt;
}

NandError PageFtl::program(std::size_t index, const std::vector<std::uint8_t>& data) {
    const auto address = address_of(index);
    const auto error = nand_.program_page(address.block, address.page, data);
    if (error == NandError::None) { ++metrics_.nand_programs; }
    return error;
}

NandError PageFtl::erase(std::size_t block) {
    const auto error = nand_.erase_block(block);
    if (error == NandError::None) { ++metrics_.nand_erases; }
    return error;
}

void PageFtl::mark_block_free(std::size_t block) noexcept {
    const auto first = block * nand_.geometry().pages_per_block;
    for (std::size_t page = 0; page < nand_.geometry().pages_per_block; ++page) {
        pages_[first + page] = {};
    }
}

NandError PageFtl::clean_reserve() {
    if (!reserve_needs_cleanup_) { return NandError::None; }
    const auto error = erase(*reserved_block_);
    if (error == NandError::None) {
        mark_block_free(*reserved_block_);
        reserve_needs_cleanup_ = false;
    }
    return error;
}

FtlStatus PageFtl::collect_garbage() {
    if (!reserved_block_) { return {FtlError::GcDisabled}; }
    if (const auto cleanup = clean_reserve(); cleanup != NandError::None) {
        ++metrics_.gc_failures;
        return {FtlError::NandFailure, cleanup};
    }

    const auto& geometry = nand_.geometry();
    std::optional<std::size_t> victim;
    std::size_t most_invalid = 0;
    for (std::size_t block = 0; block < geometry.block_count; ++block) {
        if (block == reserved_block_) { continue; }
        std::size_t invalid = 0;
        for (std::size_t page = 0; page < geometry.pages_per_block; ++page) {
            if (pages_[block * geometry.pages_per_block + page].state == PageState::Invalid) {
                ++invalid;
            }
        }
        if (invalid > most_invalid) { victim = block; most_invalid = invalid; }
    }
    if (!victim) { return {FtlError::NoReclaimableBlock}; }

    struct Move {
        std::size_t source;
        std::size_t destination;
        std::size_t lba;
        std::vector<std::uint8_t> data;
    };
    std::vector<Move> moves;
    moves.reserve(geometry.pages_per_block);
    const auto first = *victim * geometry.pages_per_block;
    const auto reserve_first = *reserved_block_ * geometry.pages_per_block;
    // Allocate and read everything before changing physical or logical state.
    for (std::size_t page = 0; page < geometry.pages_per_block; ++page) {
        const auto source = first + page;
        if (pages_[source].state != PageState::Valid) { continue; }
        std::vector<std::uint8_t> data;
        const auto address = address_of(source);
        const auto error = nand_.read_page(address.block, address.page, data);
        if (error != NandError::None) {
            ++metrics_.gc_failures;
            return {FtlError::NandFailure, error};
        }
        moves.push_back({source, reserve_first + moves.size(), *pages_[source].lba, std::move(data)});
    }

    const auto rollback_reserve = [this](NandError original) {
        ++metrics_.gc_failures;
        // If cleanup itself fails, leave unmapped staging pages Invalid.
        // The next GC retries cleanup before using the reserved block.
        const auto cleanup = clean_reserve();
        return FtlStatus{FtlError::NandFailure,
                         cleanup == NandError::None ? original : cleanup};
    };
    for (const auto& move : moves) {
        const auto error = program(move.destination, move.data);
        if (error != NandError::None) { return rollback_reserve(error); }
        pages_[move.destination] = {PageState::Invalid, std::nullopt};
        reserve_needs_cleanup_ = true;
    }

    // Publish all copied pages. Metadata assignments do not allocate.
    for (const auto& move : moves) {
        logical_to_physical_[move.lba] = move.destination;
        pages_[move.destination] = {PageState::Valid, move.lba};
        pages_[move.source] = {PageState::Invalid, std::nullopt};
    }
    const auto erase_error = erase(*victim);
    if (erase_error != NandError::None) {
        // Injected erase errors occur before NAND changes bytes, so restore
        // old mappings first, then clear the unmapped staging copies.
        for (const auto& move : moves) {
            logical_to_physical_[move.lba] = move.source;
            pages_[move.source] = {PageState::Valid, move.lba};
            pages_[move.destination] = {PageState::Invalid, std::nullopt};
        }
        return rollback_reserve(erase_error);
    }
    mark_block_free(*victim);
    reserved_block_ = *victim;
    reserve_needs_cleanup_ = false;
    ++metrics_.gc_cycles;
    metrics_.gc_moved_pages += static_cast<std::uint64_t>(moves.size());
    metrics_.gc_reclaimed_pages += static_cast<std::uint64_t>(most_invalid);
    return {};
}

std::optional<PhysicalAddress> PageFtl::physical_address(std::size_t lba) const noexcept {
    if (lba >= logical_page_count() || !logical_to_physical_[lba]) {
        return std::nullopt;
    }
    return address_of(*logical_to_physical_[lba]);
}

std::optional<PhysicalPageInfo> PageFtl::page_info(PhysicalAddress address) const noexcept {
    const auto& geometry = nand_.geometry();
    if (address.block >= geometry.block_count || address.page >= geometry.pages_per_block) {
        return std::nullopt;
    }
    return pages_[address.block * geometry.pages_per_block + address.page];
}

FtlStats PageFtl::stats() const noexcept {
    FtlStats result;
    for (const auto& page : pages_) {
        switch (page.state) {
            case PageState::Free: ++result.free_pages; break;
            case PageState::Valid: ++result.valid_pages; break;
            case PageState::Invalid: ++result.invalid_pages; break;
        }
    }
    return result;
}

const NandFlash& PageFtl::nand() const noexcept { return nand_; }

std::optional<std::size_t> PageFtl::reserved_block() const noexcept { return reserved_block_; }

std::size_t PageFtl::available_free_pages() const noexcept {
    std::size_t available = 0;
    for (std::size_t index = 0; index < pages_.size(); ++index) {
        if (pages_[index].state == PageState::Free && address_of(index).block != reserved_block_) {
            ++available;
        }
    }
    return available;
}

FtlMetrics PageFtl::metrics() const noexcept { return metrics_; }

double PageFtl::write_amplification() const noexcept {
    return metrics_.host_writes == 0 ? 0.0
        : static_cast<double>(metrics_.nand_programs) / static_cast<double>(metrics_.host_writes);
}

void PageFtl::inject_failure(NandOperation operation, std::size_t skip) { nand_.inject_failure(operation, skip); }
void PageFtl::clear_failures() noexcept { nand_.clear_failures(); }

}  // namespace minissd
