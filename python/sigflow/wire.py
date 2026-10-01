"""Python side of the sigflow wire format.

This mirrors include/sigflow/tlv.hpp and frame.hpp closely, so look there
for the byte layouts. Generated modules import this file, and the system
tests use it to talk to the C++ binaries.
"""

from __future__ import annotations

import io
import struct
from contextlib import contextmanager
from dataclasses import dataclass
from enum import IntEnum
from typing import Iterator, TypeVar

FRAME_MAGIC = 0x5346
PROTOCOL_VERSION = 1
MAX_PAYLOAD_SIZE = 64 * 1024
MAX_FIELD_SIZE = 0xFFFF

# magic, version, flags, message id, sequence, payload length
_HEADER = struct.Struct(">HBBHHI")
_TLV_HEADER = struct.Struct(">BH")
_CRC = struct.Struct(">H")


class DecodeError(ValueError):
    """Raised for any malformed frame or message body."""


# TLV --------------------------------------------------------------------


class TlvWriter:
    def __init__(self) -> None:
        self._buffer = io.BytesIO()

    def put_int(self, tag: int, value: int, size: int, *, signed: bool) -> None:
        # to_bytes raises OverflowError on values that do not fit, which is
        # the intended behaviour: better to fail here than to send a wrapped number.
        self._put(tag, int(value).to_bytes(size, "big", signed=signed))

    def put_bool(self, tag: int, value: bool) -> None:
        self._put(tag, b"\x01" if value else b"\x00")

    def put_bytes(self, tag: int, value: bytes) -> None:
        if len(value) > MAX_FIELD_SIZE:
            raise ValueError(f"tag {tag} has {len(value)} bytes, the limit is {MAX_FIELD_SIZE}")
        self._put(tag, value)

    def getvalue(self) -> bytes:
        return self._buffer.getvalue()

    def _put(self, tag: int, value: bytes) -> None:
        self._buffer.write(_TLV_HEADER.pack(tag, len(value)))
        self._buffer.write(value)


def parse_tlv(payload: bytes, where: str = "") -> Iterator[tuple[int, memoryview]]:
    """Yields (tag, value) pairs. Values are views, nothing is copied."""
    view = memoryview(payload)
    offset = 0
    while offset < len(view):
        if len(view) - offset < _TLV_HEADER.size:
            raise DecodeError(f"{where}: incomplete field header at offset {offset}")
        tag, length = _TLV_HEADER.unpack_from(view, offset)
        offset += _TLV_HEADER.size
        if len(view) - offset < length:
            raise DecodeError(f"{where}: field with tag {tag} claims {length} bytes "
                              f"but only {len(view) - offset} are left")
        yield tag, view[offset:offset + length]
        offset += length


def read_int(value: memoryview, size: int, *, signed: bool) -> int:
    if len(value) != size:
        raise DecodeError(f"field has {len(value)} bytes, expected {size}")
    return int.from_bytes(value, "big", signed=signed)


def read_bool(value: memoryview) -> bool:
    if len(value) != 1 or value[0] > 1:
        raise DecodeError("bool must be a single byte, 0 or 1")
    return value[0] == 1


def read_string(value: memoryview) -> str:
    try:
        return bytes(value).decode("utf-8")
    except UnicodeDecodeError as error:
        raise DecodeError(f"string is not valid utf-8: {error}") from None


E = TypeVar("E", bound=IntEnum)


def read_enum(enum_type: type[E], value: memoryview, size: int) -> E:
    raw = read_int(value, size, signed=False)
    try:
        return enum_type(raw)
    except ValueError:
        raise DecodeError(f"unknown {enum_type.__name__} value {raw}") from None


@contextmanager
def field_context(where: str) -> Iterator[None]:
    """Prefixes decode errors with the field they happened in."""
    try:
        yield
    except DecodeError as error:
        raise DecodeError(f"{where}: {error}") from None


def check_duplicate(seen: set[int], tag: int) -> None:
    if tag in seen:
        raise DecodeError("duplicate field")
    seen.add(tag)


def check_required(seen: set[int], required: dict[int, str]) -> None:
    missing = [name for tag, name in required.items() if tag not in seen]
    if missing:
        raise DecodeError(f"missing required field {', '.join(missing)}")


# Frames -----------------------------------------------------------------


def crc16_ccitt(data: bytes, crc: int = 0xFFFF) -> int:
    """CRC-16/CCITT-FALSE, bit by bit. Slow, but these frames are tiny."""
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if crc & 0x8000 else (crc << 1)
            crc &= 0xFFFF
    return crc


@dataclass(frozen=True)
class Frame:
    message_id: int
    sequence: int
    payload: bytes


def encode_frame(message_id: int, sequence: int, payload: bytes) -> bytes:
    if len(payload) > MAX_PAYLOAD_SIZE:
        raise ValueError(f"payload of {len(payload)} bytes is too large")
    body = _HEADER.pack(FRAME_MAGIC, PROTOCOL_VERSION, 0, message_id, sequence & 0xFFFF, len(payload)) + payload
    return body + _CRC.pack(crc16_ccitt(body))


def decode_frame(data: bytes) -> Frame:
    if len(data) < _HEADER.size + _CRC.size:
        raise DecodeError(f"frame has only {len(data)} bytes")
    magic, version, _flags, message_id, sequence, length = _HEADER.unpack_from(data)
    if magic != FRAME_MAGIC:
        raise DecodeError(f"bad magic 0x{magic:04x}")
    if version != PROTOCOL_VERSION:
        raise DecodeError(f"unsupported version {version}")
    if len(data) != _HEADER.size + length + _CRC.size:
        raise DecodeError(f"frame is {len(data)} bytes, header says {_HEADER.size + length + _CRC.size}")
    (received_crc,) = _CRC.unpack_from(data, _HEADER.size + length)
    if received_crc != crc16_ccitt(data[:_HEADER.size + length]):
        raise DecodeError("crc mismatch")
    return Frame(message_id, sequence, bytes(data[_HEADER.size:_HEADER.size + length]))


def encode_message(message, sequence: int) -> bytes:
    """Wraps any generated message object in a frame."""
    return encode_frame(message.MESSAGE_ID, sequence, message.encode())


def decode_message(frame: Frame, registry: dict[int, type]):
    """Turns a frame into the matching generated message object."""
    message_type = registry.get(frame.message_id)
    if message_type is None:
        raise DecodeError(f"no message with id 0x{frame.message_id:04x}")
    return message_type.decode(frame.payload)
