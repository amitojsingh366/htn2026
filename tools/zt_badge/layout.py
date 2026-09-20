"""Pure offline parsers. Never decode NVS, filesystem, wallet, or app payloads."""
import hashlib
import struct
import zlib
from . import FLASH_SIZE, APP_OFFSET, APP_SIZE, TAIL_OFFSET
from .errors import RecoveryError, require

STOCK = [("nvs", 1, 2, 0x9000, 0x4000, 0),
         ("phy_init", 1, 1, 0xD000, 0x1000, 0),
         ("factory", 0, 0, APP_OFFSET, APP_SIZE, 0),
         ("storage", 1, 0x83, 0x2B0000, 0x140000, 0)]
PROTECTED = [(0, APP_OFFSET), (APP_OFFSET + APP_SIZE, TAIL_OFFSET)]


def image_header(data, revision=None, release=False):
    require(len(data) >= 24 and data[0] == 0xE9, "INVALID_IMAGE_MAGIC")
    require(1 <= data[1] <= 16, "INVALID_IMAGE_SEGMENT_COUNT")
    chip = struct.unpack_from("<H", data, 12)[0]
    require(chip == 5, "IMAGE_NOT_ESP32C3")
    minimum, maximum = struct.unpack_from("<HH", data, 15)
    # Legacy C3 min_rev is the minor revision; max_rev_full=0 means the
    # extended revision bounds are not populated (esptool 5.4.0 semantics).
    legacy_revision = maximum == 0 or (maximum == 65535 and minimum == 0 and data[14] != 0)
    if legacy_revision:
        minimum, maximum = data[14], 65535
    require(data[23] in (0, 1), "INVALID_IMAGE_DIGEST_FLAG")
    if revision is not None:
        actual_revision = revision % 100 if legacy_revision else revision
        require(actual_revision >= minimum and (maximum == 65535 or actual_revision <= maximum), "IMAGE_REVISION_INCOMPATIBLE")
    if release:
        require(data[2] == 2 and data[3] == 0x2F, "IMAGE_NOT_DIO_80MHZ_4MIB")
    cursor, checksum = 24, 0xEF
    for _ in range(data[1]):
        require(cursor + 8 <= len(data), "IMAGE_SEGMENT_HEADER_TRUNCATED")
        length = struct.unpack_from("<I", data, cursor + 4)[0]
        cursor += 8
        require(length <= len(data) - cursor, "IMAGE_SEGMENT_OUT_OF_BOUNDS")
        for byte in data[cursor:cursor + length]:
            checksum ^= byte
        cursor += length
    checksum_offset = (cursor + 16) // 16 * 16 - 1
    require(checksum_offset < len(data) and data[checksum_offset] == checksum, "IMAGE_CHECKSUM_MISMATCH")
    end = checksum_offset + 1
    if data[23]:
        require(end + 32 <= len(data) and hashlib.sha256(data[:end]).digest() == data[end:end + 32], "IMAGE_DIGEST_MISMATCH")
        end += 32
    return {"chip_id": chip, "segments": data[1], "flash_mode": data[2],
            "flash_size_frequency": data[3], "min_revision": minimum,
            "max_revision": maximum, "image_length": end,
            "appended_sha256": bool(data[23])}


def partitions(image):
    require(len(image) == FLASH_SIZE, "FULL_IMAGE_SIZE_REQUIRED")
    return partition_sector(image[0x8000:0x9000], image)


def partition_sector(sector, image=None):
    require(len(sector) == 4096, "PARTITION_SECTOR_SIZE_INVALID")
    entries, checksum_seen = [], False
    for pos in range(0, 0xC00, 32):
        record = sector[pos:pos + 32]
        if record == b"\xff" * 32:
            require(checksum_seen, "PARTITION_MD5_MISSING")
            require(sector[pos:] == b"\xff" * (4096 - pos), "PARTITION_TRAILING_DATA")
            break
        magic = struct.unpack_from("<H", record)[0]
        if magic == 0xEBEB:
            require(not checksum_seen and record[2:16] == b"\xff" * 14, "INVALID_PARTITION_MD5_RECORD")
            require(hashlib.md5(sector[:pos]).digest() == record[16:], "PARTITION_MD5_MISMATCH")
            checksum_seen = True
            continue
        require(not checksum_seen and magic == 0x50AA, "INVALID_PARTITION_ENTRY")
        kind, subtype, offset, size, raw_label, flags = struct.unpack_from("<BBII16sI", record, 2)
        label = raw_label.split(b"\0", 1)[0].decode("ascii", errors="strict")
        require(size > 0 and offset >= 0x9000 and offset % 0x1000 == 0
                and size % 0x1000 == 0 and offset + size <= FLASH_SIZE, "PARTITION_OUT_OF_BOUNDS")
        require(kind != 0 or offset % 0x10000 == 0, "APP_PARTITION_UNALIGNED")
        require(not any(offset < p["offset"] + p["size"] and p["offset"] < offset + size for p in entries), "PARTITION_OVERLAP")
        require(not any(p["label"] == label for p in entries), "DUPLICATE_PARTITION_LABEL")
        entries.append({"label": label, "type": kind, "subtype": subtype,
                        "offset": offset, "size": size, "flags": flags,
                        "sha256": hashlib.sha256(image[offset:offset + size]).hexdigest() if image is not None else None})
    else:
        require(False, "PARTITION_TERMINATOR_MISSING")
    return entries


def inspect(image):
    result = {"STOCK_IMAGE_VALID": False, "LAYOUT_SUPPORTED": False,
              "partitions": [], "conditions": {}}
    for name, data in (("boot", image[:0x8000]), ("factory", image[APP_OFFSET:APP_OFFSET + APP_SIZE])):
        try:
            result[name] = image_header(data)
        except (RecoveryError, UnicodeError, struct.error) as error:
            result["conditions"][name] = getattr(error, "condition", "INVALID_IMAGE_STRUCTURE")
    try:
        result["partitions"] = partitions(image)
        observed = [(p["label"], p["type"], p["subtype"], p["offset"], p["size"], p["flags"]) for p in result["partitions"]]
        result["LAYOUT_SUPPORTED"] = sorted(observed) == sorted(STOCK)
        if not result["LAYOUT_SUPPORTED"]:
            result["conditions"]["layout"] = "UNSUPPORTED_PARTITION_LAYOUT"
    except (RecoveryError, UnicodeError, struct.error) as error:
        result["conditions"]["partition_table"] = getattr(error, "condition", "INVALID_PARTITION_STRUCTURE")
    result["STOCK_IMAGE_VALID"] = not any(k in result["conditions"] for k in ("boot", "factory", "partition_table"))
    result["tail_blank"] = image[TAIL_OFFSET:] == b"\xff" * (FLASH_SIZE - TAIL_OFFSET)
    return result


def installation_header(image, mac, original_hash, allowed_uuids):
    sector = image[TAIL_OFFSET:TAIL_OFFSET + 4096]
    require(len(sector) == 4096, "INSTALLATION_HEADER_TRUNCATED")
    require(sector[:4] == b"ZTIN" and struct.unpack_from("<HH", sector, 4) == (1, 80), "INVALID_INSTALLATION_HEADER")
    require(sector[8:14] == bytes.fromhex(mac) and sector[14:16] == b"\0\0", "INSTALLATION_MAC_MISMATCH")
    require(sector[32:64].hex() == original_hash, "INSTALLATION_BASELINE_MISMATCH")
    require(struct.unpack_from("<III", sector, 64) == (0x3F1000, 0xF000, 1), "INSTALLATION_RANGE_MISMATCH")
    require(zlib.crc32(sector[:76]) == struct.unpack_from("<I", sector, 76)[0], "INSTALLATION_CRC_MISMATCH")
    require(sector[80:] == b"\xff" * (4096 - 80), "INSTALLATION_SECTOR_NOT_RESERVED")
    import uuid
    installation = str(uuid.UUID(bytes=sector[16:32]))
    require(installation in allowed_uuids, "UNKNOWN_INSTALLATION_UUID")
    return installation


def differing_ranges(first, second, ranges=None):
    """Return offsets/lengths only; never expose differing contents."""
    require(len(first) == len(second), "COMPARISON_SIZE_MISMATCH")
    found = []
    for start, end in ranges or [(0, len(first))]:
        run = None
        for pos in range(start, end):
            if first[pos] != second[pos] and run is None:
                run = pos
            if first[pos] == second[pos] and run is not None:
                found.append([run, pos])
                run = None
        if run is not None:
            found.append([run, end])
    return found
