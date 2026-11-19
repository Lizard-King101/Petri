#pragma once

// Rotating world checkpoints in the MatrixPortal M4's 2 MiB QSPI flash.
//
// Four slots of 256 KiB sit at the start of the flash: a world with brains
// saves up to 177 KB. A slot holds a 32-byte
// header at its first byte and the checkpoint from byte 256. A save erases each
// sector just before writing it, leaves the header bytes erased, writes all the
// data, reads it back, and only then programs the header. An interrupted save
// therefore leaves a slot without a valid header, and the previous newest slot
// is never chosen for writing.
//
// Header: magic "PSLT" (u32), version (u16), reserved (u16), sequence (u32),
// size (u32), data CRC-32 (u32), tick (u64), header CRC-32 of the preceding
// 28 bytes (u32).

#include <Adafruit_SPIFlashBase.h>

#include <cstring>

#include "link.hpp"

namespace petri {

class CheckpointStore {
public:
    static constexpr std::uint32_t slot_count = 4, slot_bytes = 256 * 1024, sector_bytes = 4096;
    static constexpr std::uint32_t data_offset = 256, header_bytes = 32, max_size = slot_bytes - data_offset;
    static constexpr std::uint32_t magic = 0x544c5350; // "PSLT"
    static constexpr std::uint16_t version = 1;

    struct Slot {
        bool valid = false;
        std::uint32_t sequence = 0, size = 0, crc = 0;
        std::uint64_t tick = 0;
    };

    bool begin() {
        ready_ = flash_.begin() && flash_.size() >= slot_count * slot_bytes;
        return ready_;
    }
    bool ready() const { return ready_; }
    std::uint32_t jedec_id() { return flash_.getJEDECID(); }

    // Reads a slot's header and checks its data against the stored CRC.
    Slot read_slot(std::uint32_t index) {
        Slot slot;
        if (!ready_ || index >= slot_count) {
            return slot;
        }
        std::uint8_t header[header_bytes];
        flash_.readBuffer(base(index), header, sizeof(header));
        if (get32(header) != magic || get16(header + 4) != version ||
            get32(header + 28) != link::crc32(header, 28)) {
            return slot;
        }
        slot.sequence = get32(header + 8);
        slot.size = get32(header + 12);
        slot.crc = get32(header + 16);
        slot.tick = get32(header + 20) | std::uint64_t(get32(header + 24)) << 32;
        if (slot.size == 0 || slot.size > max_size || data_crc(index, slot.size) != slot.crc) {
            return slot;
        }
        slot.valid = true;
        return slot;
    }

    // The valid slot with the highest sequence, or -1.
    int newest(Slot& result) {
        int best = -1;
        for (std::uint32_t index = 0; index < slot_count; ++index) {
            const auto slot = read_slot(index);
            if (slot.valid && (best < 0 || slot.sequence > result.sequence)) {
                best = static_cast<int>(index);
                result = slot;
            }
        }
        return best;
    }

    // Starts a save of `size` bytes in the oldest or unused slot, never the newest valid one.
    bool begin_write(std::uint32_t size) {
        writing_ = false;
        if (!ready_ || size == 0 || size > max_size) {
            return false;
        }
        Slot newest_slot;
        const int newest_index = newest(newest_slot);
        int target = -1;
        std::uint32_t oldest = 0xffffffffu;
        for (std::uint32_t index = 0; index < slot_count; ++index) {
            if (static_cast<int>(index) == newest_index) {
                continue;
            }
            const auto slot = read_slot(index);
            const std::uint32_t age = slot.valid ? slot.sequence : 0;
            if (target < 0 || age < oldest) {
                target = static_cast<int>(index);
                oldest = age;
            }
        }
        write_slot_ = static_cast<std::uint32_t>(target);
        write_sequence_ = newest_index >= 0 ? newest_slot.sequence + 1 : 1;
        write_size_ = size;
        written_ = 0;
        erased_ = 0;
        crc_ = 0;
        writing_ = true;
        return true;
    }

    // Appends the next bytes of the checkpoint. Offsets must arrive in order.
    bool write(std::uint32_t offset, const std::uint8_t* data, std::uint32_t size) {
        if (!writing_ || offset != written_ || size > write_size_ - written_) {
            writing_ = false;
            return false;
        }
        const auto start = data_offset + offset;
        while (erased_ < start + size) {
            flash_.eraseSector((base(write_slot_) + erased_) / sector_bytes);
            erased_ += sector_bytes;
        }
        flash_.writeBuffer(base(write_slot_) + start, data, size);
        crc_ = link::crc32(data, size, crc_);
        written_ += size;
        return true;
    }

    bool writing() const { return writing_; }
    bool complete() const { return writing_ && written_ == write_size_; }

    // Verifies the written data and programs the header. Returns the slot, or -1.
    int finish(std::uint64_t tick) {
        if (!complete()) {
            writing_ = false;
            return -1;
        }
        writing_ = false;
        if (data_crc(write_slot_, write_size_) != crc_) {
            return -1;
        }
        std::uint8_t header[header_bytes];
        put32(header, magic);
        header[4] = version & 0xff;
        header[5] = version >> 8;
        header[6] = header[7] = 0;
        put32(header + 8, write_sequence_);
        put32(header + 12, write_size_);
        put32(header + 16, crc_);
        put32(header + 20, static_cast<std::uint32_t>(tick));
        put32(header + 24, static_cast<std::uint32_t>(tick >> 32));
        put32(header + 28, link::crc32(header, 28));
        flash_.writeBuffer(base(write_slot_), header, sizeof(header));
        return read_slot(write_slot_).valid ? static_cast<int>(write_slot_) : -1;
    }

    std::uint32_t sequence() const { return write_sequence_; }

    bool read(std::uint32_t index, std::uint32_t offset, std::uint8_t* data, std::uint32_t size) {
        if (!ready_ || index >= slot_count || offset + size > max_size) {
            return false;
        }
        return flash_.readBuffer(base(index) + data_offset + offset, data, size) == size;
    }

    // Test hook: clears one bit in the slot's data so it fails its CRC.
    // NOR flash can only clear bits without an erase, so it looks for a nonzero byte.
    bool corrupt(std::uint32_t index) {
        for (std::uint32_t at = 1000; at < max_size; ++at) {
            std::uint8_t byte = 0;
            flash_.readBuffer(base(index) + data_offset + at, &byte, 1);
            if (byte) {
                const std::uint8_t cleared = byte & (byte - 1);
                flash_.writeBuffer(base(index) + data_offset + at, &cleared, 1);
                return true;
            }
        }
        return false;
    }

private:
    static std::uint32_t base(std::uint32_t index) { return index * slot_bytes; }
    static std::uint32_t get32(const std::uint8_t* at) {
        return at[0] | at[1] << 8 | at[2] << 16 | std::uint32_t(at[3]) << 24;
    }
    static std::uint16_t get16(const std::uint8_t* at) { return static_cast<std::uint16_t>(at[0] | at[1] << 8); }
    static void put32(std::uint8_t* at, std::uint32_t value) {
        for (int byte = 0; byte < 4; ++byte) {
            at[byte] = static_cast<std::uint8_t>(value >> (8 * byte));
        }
    }

    std::uint32_t data_crc(std::uint32_t index, std::uint32_t size) {
        std::uint8_t chunk[256];
        std::uint32_t crc = 0;
        for (std::uint32_t at = 0; at < size; at += sizeof(chunk)) {
            const auto take = std::min<std::uint32_t>(sizeof(chunk), size - at);
            flash_.readBuffer(base(index) + data_offset + at, chunk, take);
            crc = link::crc32(chunk, take, crc);
        }
        return crc;
    }

    Adafruit_FlashTransport_QSPI transport_;
    Adafruit_SPIFlashBase flash_{&transport_};
    bool ready_ = false, writing_ = false;
    std::uint32_t write_slot_ = 0, write_sequence_ = 0, write_size_ = 0, written_ = 0, erased_ = 0, crc_ = 0;
};

} // namespace petri
