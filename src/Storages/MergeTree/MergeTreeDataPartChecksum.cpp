/*
 * Copyright 2016-2023 ClickHouse, Inc.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */


/*
 * This file may have been modified by Bytedance Ltd. and/or its affiliates (“ Bytedance's Modifications”).
 * All Bytedance's Modifications are Copyright (2023) Bytedance Ltd. and/or its affiliates.
 */

#include "MergeTreeDataPartChecksum.h"
#include <Common/CurrentMetrics.h>
#include <Common/SipHash.h>
#include <Common/hex.h>
#include "metric_helper.h"
#include <DataTypes/MapHelpers.h>
#include <IO/ReadHelpers.h>
#include <IO/WriteHelpers.h>
#include <IO/ReadBufferFromString.h>
#include <IO/WriteBufferFromString.h>
#include <Compression/CompressedReadBuffer.h>
#include <Compression/CompressedWriteBuffer.h>


namespace CurrentMetrics
{
    extern const Metric PartChecksums;
}

namespace DB
{


namespace ErrorCodes
{
    extern const int CHECKSUM_DOESNT_MATCH;
    extern const int BAD_SIZE_OF_FILE_IN_DATA_PART;
    extern const int FORMAT_VERSION_TOO_OLD;
    extern const int FILE_DOESNT_EXIST;
    extern const int UNEXPECTED_FILE_IN_DATA_PART;
    extern const int UNKNOWN_FORMAT;
    extern const int NO_FILE_IN_DATA_PART;
}

void MergeTreeDataPartChecksum::checkEqual(const MergeTreeDataPartChecksum & rhs, bool have_uncompressed, const String & name) const
{
    if (is_compressed && have_uncompressed)
    {
        if (!rhs.is_compressed)
            throw Exception("No uncompressed checksum for file " + name, ErrorCodes::CHECKSUM_DOESNT_MATCH);
        if (rhs.uncompressed_size != uncompressed_size)
            throw Exception("Unexpected uncompressed size of file " + name + " in data part", ErrorCodes::BAD_SIZE_OF_FILE_IN_DATA_PART);
        if (rhs.uncompressed_hash != uncompressed_hash)
            throw Exception("Checksum mismatch for uncompressed file " + name + " in data part", ErrorCodes::CHECKSUM_DOESNT_MATCH);
        return;
    }
    if (rhs.file_size != file_size)
        throw Exception("Unexpected size of file " + name + " in data part", ErrorCodes::BAD_SIZE_OF_FILE_IN_DATA_PART);
    if (rhs.file_hash != file_hash)
        throw Exception("Checksum mismatch for file " + name + " in data part", ErrorCodes::CHECKSUM_DOESNT_MATCH);
}

void MergeTreeDataPartChecksum::checkSize(const DiskPtr & disk, const String & path) const
{
    if (!disk->exists(path))
        throw Exception(fullPath(disk, path) + " doesn't exist", ErrorCodes::FILE_DOESNT_EXIST);
    if (disk->isDirectory(path))
        // This is a projection, no need to check its size.
        return;
    UInt64 size = disk->getFileSize(path);
    if (size != file_size)
        throw Exception(fullPath(disk, path) + " has unexpected size: " + toString(size) + " instead of " + toString(file_size),
            ErrorCodes::BAD_SIZE_OF_FILE_IN_DATA_PART);
}


MergeTreeDataPartChecksums::MergeTreeDataPartChecksums()
{
    CurrentMetrics::add(CurrentMetrics::PartChecksums, 1, Metrics::MetricType::Store);
}

MergeTreeDataPartChecksums::MergeTreeDataPartChecksums(const MergeTreeDataPartChecksums & other)
    : files(other.files)
    , storage_type(other.storage_type)
{
    *versions = *(other.versions);
    CurrentMetrics::add(CurrentMetrics::PartChecksums, 1, Metrics::MetricType::Store);
}

MergeTreeDataPartChecksums::MergeTreeDataPartChecksums(MergeTreeDataPartChecksums && other)
    : files(std::move(other.files))
    , storage_type(std::move(other.storage_type))
    , versions(std::move(other.versions))
{
    CurrentMetrics::add(CurrentMetrics::PartChecksums, 1, Metrics::MetricType::Store);
}

MergeTreeDataPartChecksums::~MergeTreeDataPartChecksums()
{
    CurrentMetrics::sub(CurrentMetrics::PartChecksums, 1, Metrics::MetricType::Store);
}

void MergeTreeDataPartChecksums::checkEqual(const MergeTreeDataPartChecksums & rhs, bool have_uncompressed) const
{
    for (const auto & it : rhs.files)
    {
        const String & name = it.first;

        if (!files.count(name))
            throw Exception("Unexpected file " + name + " in data part", ErrorCodes::UNEXPECTED_FILE_IN_DATA_PART);
    }

    for (const auto & it : files)
    {
        const String & name = it.first;

        /// Exclude files written by inverted index from check. No correct checksums are available for them currently.
        if (name.ends_with(".gin_dict") || name.ends_with(".gin_post") || name.ends_with(".gin_seg") || name.ends_with(".gin_sid"))
            continue;

        auto jt = rhs.files.find(name);
        if (jt == rhs.files.end())
            throw Exception("No file " + name + " in data part", ErrorCodes::NO_FILE_IN_DATA_PART);

        it.second.checkEqual(jt->second, have_uncompressed, name);
    }
}

bool MergeTreeDataPartChecksums::adjustDiffImplicitKeyOffset(const MergeTreeDataPartChecksums & rhs)
{
    if (!versions->enable_compact_map_data)
        return false;
    bool has_diff = false;
    for (auto & it: files)
    {
        const String & name = it.first;

        auto jt = rhs.files.find(name);
        if (jt == rhs.files.end())
            throw Exception("No file " + name + " in data part", ErrorCodes::NO_FILE_IN_DATA_PART);

        if (isMapImplicitKey(name) && it.second.file_offset != jt->second.file_offset)
        {
            has_diff = true;
            it.second.file_offset = jt->second.file_offset;
        }
    }
    return has_diff;
}

bool MergeTreeDataPartChecksums::isEqual(const MergeTreeDataPartChecksums & rhs, const String & col_name) const
{
    auto it = files.find(col_name);
    auto jt = rhs.files.find(col_name);
    return it != files.end() && jt != rhs.files.end() && it->second.file_size == jt->second.file_size
        && it->second.file_hash == jt->second.file_hash;
}

void MergeTreeDataPartChecksums::checkSizes(const DiskPtr & disk, const String & path) const
{
    /// Skip inverted index files, these have a default MergeTreeDataPartChecksum with file_size == 0
    if (path.ends_with(".gin_dict") || path.ends_with(".gin_post") || path.ends_with(".gin_seg") || path.ends_with(".gin_sid"))
        return;

    for (const auto & it : files)
    {
        const String & name = it.first;
        if (versions->enable_compact_map_data && isMapImplicitKey(name))
            continue;
        it.second.checkSize(disk, path + name);
    }
}

UInt64 MergeTreeDataPartChecksums::getTotalSizeOnDisk() const
{
    UInt64 res = 0;
    for (const auto & it : files)
        res += it.second.file_size;
    return res;
}

/// Returns names of all the files for the given map column.
/// For compact map, both checksum's filename and disk's filename are returned in order for
/// MergeTreeDataMergerMutator to remove related files from disk and checksums when the column is dropped.
Strings MergeTreeDataPartChecksums::collectImplicitColumnFilesForByteMap(const String & map_column) const
{
    auto map_key_prefix = genMapKeyFilePrefix(map_column);
    auto map_base_prefix = genMapBaseFilePrefix(map_column);

    Strings res;
    /// Collect all compact file names from the implicit key name from checksums
    NameSet compact_file_set;
    for (const auto & [file, _] : files)
    {
        if (startsWith(file, map_key_prefix))
        {
            if (versions->enable_compact_map_data)
            {
                String file_name = getMapFileNameFromImplicitFileName(file);
                compact_file_set.insert(file_name);
            }
            res.emplace_back(file);
        }
        else if (startsWith(file, map_base_prefix))
            res.emplace_back(file);
    }

    for (auto & file : compact_file_set)
        res.emplace_back(file);

    return res;
}

bool MergeTreeDataPartChecksums::read(ReadBuffer & in, size_t format_version)
{
    switch (format_version)
    {
        case 1:
            return false;
        case 2:
            return readV2(in);
        case 3:
            return readV3(in);
        case 4:
            return readV4(in);
        case 5:
            return readV5(in);
        case 6:
            return readV6(in);
        case 7:
            return readV7(in);
        default:
            throw Exception("Bad checksums format version: " + DB::toString(format_version), ErrorCodes::UNKNOWN_FORMAT);
    }
}

bool MergeTreeDataPartChecksums::read(ReadBuffer & in)
{
    files.clear();

    assertString("checksums format version: ", in);
    size_t format_version;
    readText(format_version, in);
    assertChar('\n', in);

    read(in, format_version);
    return true;
}

bool MergeTreeDataPartChecksums::readV2(ReadBuffer & in)
{
    size_t count;

    readText(count, in);
    assertString(" files:\n", in);

    for (size_t i = 0; i < count; ++i)
    {
        String name;
        Checksum sum;

        readString(name, in);
        assertString("\n\tsize: ", in);
        readText(sum.file_size, in);
        assertString("\n\thash: ", in);
        readText(sum.file_hash.first, in);
        assertString(" ", in);
        readText(sum.file_hash.second, in);
        assertString("\n\tcompressed: ", in);
        readText(sum.is_compressed, in);
        if (sum.is_compressed)
        {
            assertString("\n\tuncompressed size: ", in);
            readText(sum.uncompressed_size, in);
            assertString("\n\tuncompressed hash: ", in);
            readText(sum.uncompressed_hash.first, in);
            assertString(" ", in);
            readText(sum.uncompressed_hash.second, in);
        }
        assertChar('\n', in);

        files.insert(std::make_pair(name, sum));
    }

    return true;
}

bool MergeTreeDataPartChecksums::readV3(ReadBuffer & in)
{
    size_t count;

    readVarUInt(count, in);

    for (size_t i = 0; i < count; ++i)
    {
        String name;
        Checksum sum;

        readBinary(name, in);
        if (versions->enable_compact_map_data)
            readVarUInt(sum.file_offset, in);
        readVarUInt(sum.file_size, in);
        readPODBinary(sum.file_hash, in);
        readBinary(sum.is_compressed, in);

        if (sum.is_compressed)
        {
            readVarUInt(sum.uncompressed_size, in);
            readPODBinary(sum.uncompressed_hash, in);
        }

        files.emplace(std::move(name), sum);
    }

    return true;
}

bool MergeTreeDataPartChecksums::readV4(ReadBuffer & from)
{
    CompressedReadBuffer in{from};
    return readV3(in);
}

bool MergeTreeDataPartChecksums::readV5(ReadBuffer & from)
{
    /// v4
    CompressedReadBuffer in{from};

    /// v3
    size_t count;

    readVarUInt(count, in);

    for (size_t i = 0; i < count; ++i)
    {
        String name;
        Checksum sum;

        readBinary(name, in);
        if (storage_type == StorageType::ByteHDFS)
            readVarUInt(sum.file_offset, in);
        readVarUInt(sum.file_size, in);
        readPODBinary(sum.file_hash, in);
        readBinary(sum.is_compressed, in);

        if (sum.is_compressed)
        {
            readVarUInt(sum.uncompressed_size, in);
            readPODBinary(sum.uncompressed_hash, in);
        }

        /// v5
        readBinary(sum.is_deleted, in);

        files.emplace(std::move(name), sum);
    }

    return true;
}

bool MergeTreeDataPartChecksums::readV6(ReadBuffer & from)
{
    /// v4
    CompressedReadBuffer in{from};

    /// v3
    size_t count;

    readVarUInt(count, in);

    for (size_t i = 0; i < count; ++i)
    {
        String name;
        Checksum sum;

        readBinary(name, in);
        if (storage_type == StorageType::ByteHDFS)
        {
            readVarUInt(sum.file_offset, in);
            /// v6 for checksums preloading.
            readVarUInt(sum.mutation, in);
        }
        readVarUInt(sum.file_size, in);
        readPODBinary(sum.file_hash, in);
        readBinary(sum.is_compressed, in);

        if (sum.is_compressed)
        {
            readVarUInt(sum.uncompressed_size, in);
            readPODBinary(sum.uncompressed_hash, in);
        }

        /// v5
        readBinary(sum.is_deleted, in);

        files.emplace(std::move(name), sum);
    }

    return true;
}

/// To be compatible with feature of encryption in the dev branch
bool MergeTreeDataPartChecksums::readV7(ReadBuffer & from)
{
    /// v4
    CompressedReadBuffer in{from};

    /// v3
    size_t count;

    readVarUInt(count, in);

    for (size_t i = 0; i < count; ++i)
    {
        String name;
        Checksum sum;

        readBinary(name, in);
        if (storage_type == StorageType::ByteHDFS)
        {
            readVarUInt(sum.file_offset, in);
            /// v6 for checksums preloading.
            readVarUInt(sum.mutation, in);
        }
        readVarUInt(sum.file_size, in);
        readPODBinary(sum.file_hash, in);
        readBinary(sum.is_compressed, in);

        if (sum.is_compressed)
        {
            readVarUInt(sum.uncompressed_size, in);
            readPODBinary(sum.uncompressed_hash, in);
        }

        /// v7 for encryption
        bool is_encrypted = false;
        readBinary(is_encrypted, in);

        /// v5
        readBinary(sum.is_deleted, in);

        files.emplace(std::move(name), sum);
    }

    return true;
}

void MergeTreeDataPartChecksums::writeLocal(WriteBuffer & to) const
{
    writeString("checksums format version: 4\n", to);

    CompressedWriteBuffer out{to, CompressionCodecFactory::instance().getDefaultCodec(), 1 << 16};

    writeVarUInt(files.size(), out);

    for (const auto & it : files)
    {
        const String & name = it.first;
        const Checksum & sum = it.second;

        writeBinary(name, out);
        if (storage_type != StorageType::Local || versions->enable_compact_map_data)
            writeVarUInt(sum.file_offset, out);
        writeVarUInt(sum.file_size, out);
        writePODBinary(sum.file_hash, out);
        writeBinary(sum.is_compressed, out);

        if (sum.is_compressed)
        {
            writeVarUInt(sum.uncompressed_size, out);
            writePODBinary(sum.uncompressed_hash, out);
        }
    }
    out.finalize();
}

void MergeTreeDataPartChecksums::write(WriteBuffer & to) const
{
    writeString("checksums format version: 6\n", to);

    CompressedWriteBuffer out{to, CompressionCodecFactory::instance().getDefaultCodec(), 1 << 16};

    writeVarUInt(files.size(), out);

    for (const auto & it : files)
    {
        const String & name = it.first;
        const Checksum & sum = it.second;

        writeBinary(name, out);
        if (storage_type == StorageType::ByteHDFS || versions->enable_compact_map_data)
            writeVarUInt(sum.file_offset, out);
        if (storage_type == StorageType::ByteHDFS)
            writeVarUInt(sum.mutation, out);
        writeVarUInt(sum.file_size, out);
        writePODBinary(sum.file_hash, out);
        writeBinary(sum.is_compressed, out);

        if (sum.is_compressed)
        {
            writeVarUInt(sum.uncompressed_size, out);
            writePODBinary(sum.uncompressed_hash, out);
        }

        // writeBinary(sum.is_encrypted, out);
        writeBinary(sum.is_deleted, out);
    }
    out.finalize();
}

void MergeTreeDataPartChecksums::addFile(const String & file_name, UInt64 file_size, MergeTreeDataPartChecksum::uint128 file_hash)
{
    files[file_name] = Checksum(file_size, file_hash);
}

void MergeTreeDataPartChecksums::addFile(
    const String & file_name, UInt64 file_offset, UInt64 file_size, MergeTreeDataPartChecksum::uint128 file_hash)
{
    files[file_name] = Checksum(file_offset, file_size, file_hash);
}

void MergeTreeDataPartChecksums::add(MergeTreeDataPartChecksums && rhs_checksums)
{
    for (auto & checksum : rhs_checksums.files)
        files[std::move(checksum.first)] = std::move(checksum.second);

    rhs_checksums.files.clear();
}

/// Checksum computed from the set of control sums of .bin files.
void MergeTreeDataPartChecksums::computeTotalChecksumDataOnly(SipHash & hash) const
{
    /// We use fact that iteration is in deterministic (lexicographical) order.
    for (const auto & it : files)
    {
        const String & name = it.first;
        const Checksum & sum = it.second;

        if (!endsWith(name, ".bin"))
            continue;

        UInt64 len = name.size();
        hash.update(len);
        hash.update(name.data(), len);
        hash.update(sum.uncompressed_size);
        hash.update(sum.uncompressed_hash);
    }
}

String MergeTreeDataPartChecksums::getSerializedString() const
{
    WriteBufferFromOwnString out;
    write(out);
    return out.str();
}

MergeTreeDataPartChecksums MergeTreeDataPartChecksums::deserializeFrom(const String & s)
{
    ReadBufferFromString in(s);
    MergeTreeDataPartChecksums res;
    if (!res.read(in))
        throw Exception("Checksums format is too old", ErrorCodes::FORMAT_VERSION_TOO_OLD);
    assertEOF(in);
    return res;
}

bool MergeTreeDataPartChecksums::isBadChecksumsErrorCode(int code)
{
    return code == ErrorCodes::CHECKSUM_DOESNT_MATCH
           || code == ErrorCodes::BAD_SIZE_OF_FILE_IN_DATA_PART
           || code == ErrorCodes::NO_FILE_IN_DATA_PART
           || code == ErrorCodes::UNEXPECTED_FILE_IN_DATA_PART;
}

/// Puts into hash "stream" length of the string and its bytes
static void updateHash(SipHash & hash, const std::string & data)
{
    UInt64 len = data.size();
    hash.update(len);
    hash.update(data.data(), len);
}

/// Hash is the same as MinimalisticDataPartChecksums::hash_of_all_files
String MergeTreeDataPartChecksums::getTotalChecksumHex() const
{
    SipHash hash_of_all_files;

    for (const auto & [name, checksum] : files)
    {
        updateHash(hash_of_all_files, name);
        hash_of_all_files.update(checksum.file_hash);
    }

    UInt64 lo;
    UInt64 hi;
    hash_of_all_files.get128(lo, hi);

    return getHexUIntUppercase(hi) + getHexUIntUppercase(lo);
}

MergeTreeDataPartChecksums::Checksum::uint128 MergeTreeDataPartChecksums::getTotalChecksumUInt128() const
{
    SipHash hash_of_all_files;

    for (const auto & elem : files)
    {
        const String & name = elem.first;
        const auto & checksum = elem.second;

        updateHash(hash_of_all_files, name);
        hash_of_all_files.update(checksum.file_hash);
    }

    MergeTreeDataPartChecksums::Checksum::uint128 ret;
    hash_of_all_files.get128(reinterpret_cast<char *>(&ret));
    return ret;
}

void MinimalisticDataPartChecksums::serialize(WriteBuffer & to) const
{
    writeString("checksums format version: 5\n", to);
    serializeWithoutHeader(to);
}

void MinimalisticDataPartChecksums::serializeWithoutHeader(WriteBuffer & to) const
{
    writeVarUInt(num_compressed_files, to);
    writeVarUInt(num_uncompressed_files, to);

    writePODBinary(hash_of_all_files, to);
    writePODBinary(hash_of_uncompressed_files, to);
    writePODBinary(uncompressed_hash_of_compressed_files, to);
}

String MinimalisticDataPartChecksums::getSerializedString() const
{
    WriteBufferFromOwnString wb;
    serialize(wb);
    return wb.str();
}

bool MinimalisticDataPartChecksums::deserialize(ReadBuffer & in)
{
    assertString("checksums format version: ", in);
    size_t format_version;
    readText(format_version, in);
    assertChar('\n', in);

    if (format_version < MINIMAL_VERSION_WITH_MINIMALISTIC_CHECKSUMS)
    {
        MergeTreeDataPartChecksums new_full_checksums;
        if (!new_full_checksums.read(in, format_version))
            return false;

        computeTotalChecksums(new_full_checksums);
        full_checksums = std::move(new_full_checksums);
        return true;
    }

    if (format_version > MINIMAL_VERSION_WITH_MINIMALISTIC_CHECKSUMS)
        throw Exception("Unknown checksums format version: " + DB::toString(format_version), ErrorCodes::UNKNOWN_FORMAT);

    deserializeWithoutHeader(in);

    return true;
}

void MinimalisticDataPartChecksums::deserializeWithoutHeader(ReadBuffer & in)
{
    readVarUInt(num_compressed_files, in);
    readVarUInt(num_uncompressed_files, in);

    readPODBinary(hash_of_all_files, in);
    readPODBinary(hash_of_uncompressed_files, in);
    readPODBinary(uncompressed_hash_of_compressed_files, in);
}

void MinimalisticDataPartChecksums::computeTotalChecksums(const MergeTreeDataPartChecksums & full_checksums_)
{
    num_compressed_files = 0;
    num_uncompressed_files = 0;

    SipHash hash_of_all_files_state;
    SipHash hash_of_uncompressed_files_state;
    SipHash uncompressed_hash_of_compressed_files_state;

    for (const auto & [name, checksum] : full_checksums_.files)
    {
        updateHash(hash_of_all_files_state, name);
        hash_of_all_files_state.update(checksum.file_hash);

        if (!checksum.is_compressed)
        {
            ++num_uncompressed_files;
            updateHash(hash_of_uncompressed_files_state, name);
            hash_of_uncompressed_files_state.update(checksum.file_hash);
        }
        else
        {
            ++num_compressed_files;
            updateHash(uncompressed_hash_of_compressed_files_state, name);
            uncompressed_hash_of_compressed_files_state.update(checksum.uncompressed_hash);
        }
    }

    auto get_hash = [] (SipHash & hash, uint128 & data)
    {
        hash.get128(data);
    };

    get_hash(hash_of_all_files_state, hash_of_all_files);
    get_hash(hash_of_uncompressed_files_state, hash_of_uncompressed_files);
    get_hash(uncompressed_hash_of_compressed_files_state, uncompressed_hash_of_compressed_files);
}

String MinimalisticDataPartChecksums::getSerializedString(const MergeTreeDataPartChecksums & full_checksums, bool minimalistic)
{
    if (!minimalistic)
        return full_checksums.getSerializedString();

    MinimalisticDataPartChecksums checksums;
    checksums.computeTotalChecksums(full_checksums);
    return checksums.getSerializedString();
}

void MinimalisticDataPartChecksums::checkEqual(const MinimalisticDataPartChecksums & rhs, bool check_uncompressed_hash_in_compressed_files) const
{
    if (full_checksums && rhs.full_checksums)
        full_checksums->checkEqual(*rhs.full_checksums, check_uncompressed_hash_in_compressed_files);

    // If full checksums were checked, check total checksums just in case
    checkEqualImpl(rhs, check_uncompressed_hash_in_compressed_files);
}

void MinimalisticDataPartChecksums::checkEqual(const MergeTreeDataPartChecksums & rhs, bool check_uncompressed_hash_in_compressed_files) const
{
    if (full_checksums)
        full_checksums->checkEqual(rhs, check_uncompressed_hash_in_compressed_files);

    // If full checksums were checked, check total checksums just in case
    MinimalisticDataPartChecksums rhs_minimalistic;
    rhs_minimalistic.computeTotalChecksums(rhs);
    checkEqualImpl(rhs_minimalistic, check_uncompressed_hash_in_compressed_files);
}

void MinimalisticDataPartChecksums::checkEqualImpl(const MinimalisticDataPartChecksums & rhs, bool check_uncompressed_hash_in_compressed_files) const
{
    if (num_compressed_files != rhs.num_compressed_files || num_uncompressed_files != rhs.num_uncompressed_files)
    {
        throw Exception(ErrorCodes::CHECKSUM_DOESNT_MATCH,
                        "Different number of files: {} compressed (expected {}) and {} uncompressed ones (expected {})",
                        rhs.num_compressed_files, num_compressed_files, rhs.num_uncompressed_files, num_uncompressed_files);
    }

    Strings errors;

    if (hash_of_uncompressed_files != rhs.hash_of_uncompressed_files)
        errors.emplace_back("hash of uncompressed files doesn't match");

    if (check_uncompressed_hash_in_compressed_files)
    {
        if (uncompressed_hash_of_compressed_files != rhs.uncompressed_hash_of_compressed_files)
            errors.emplace_back("uncompressed hash of compressed files doesn't match");
    }
    else
    {
        if (hash_of_all_files != rhs.hash_of_all_files)
            errors.emplace_back("total hash of all files doesn't match");
    }

    if (!errors.empty())
    {
        String error_msg = "Checksums of parts don't match: " + errors.front();
        for (size_t i = 1; i < errors.size(); ++i)
            error_msg += ", " + errors[i];

        throw Exception(error_msg, ErrorCodes::CHECKSUM_DOESNT_MATCH);
    }
}

MinimalisticDataPartChecksums MinimalisticDataPartChecksums::deserializeFrom(const String & s)
{
    MinimalisticDataPartChecksums res;
    ReadBufferFromString rb(s);
    res.deserialize(rb);
    return res;
}

}
