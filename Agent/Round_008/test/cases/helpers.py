"""Shared case helpers: frame building, assertions."""


def u16le(v):
    return bytes([v & 0xFF, (v >> 8) & 0xFF])


def u32le(v):
    return bytes([v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF, (v >> 24) & 0xFF])


def u24le(v):
    return bytes([v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF])
