"""Strict source event-context sidecars for the experimental input clock."""
import struct


def encode(contexts):
    if not 2 <= len(contexts) <= 1000000 or contexts[:2] != [0, 0]:
        raise ValueError('contexts need two normal priming samples')
    if any(x not in (0, 1) for x in contexts):
        raise ValueError('unsupported source context')
    return struct.pack('<8sI', b'PSXCTX1\0', len(contexts)) + bytes(contexts)


def decode(data, expected_count):
    if len(data) < 12:
        raise ValueError('short context header')
    magic, count = struct.unpack_from('<8sI', data)
    if magic != b'PSXCTX1\0' or count != expected_count or len(data) != 12 + count:
        raise ValueError('context identity/count mismatch')
    contexts = list(data[12:])
    if encode(contexts) != data:
        raise ValueError('noncanonical contexts')
    return contexts


def check_protected_effects(expected, actual, words, mask):
    if not 0 < mask <= 65535 or len(expected)!=len(actual) or len(words)!=len(expected):
        return False
    source_hold=native_hold=65535
    source_press=native_press=0
    for source,native,word in zip(expected,actual,words):
        if source not in (0,1) or native not in (0,1):return False
        if source==0:
            source_press=source_hold & (~word & 65535);source_hold=word
        if native==0:
            native_press=native_hold & (~word & 65535);native_hold=word
        if ((source_hold^native_hold)|(source_press^native_press)) & mask:return False
    return True
