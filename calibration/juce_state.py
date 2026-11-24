"""JUCE plugin-state codec, enough to load a Humanoid .baby preset into the real plugin.

DawDreamer's save_state/load_state file is JUCE's copyXmlToBinary of
<VST3PluginState><IComponent>..</IComponent><IEditController>..</IEditController></VST3PluginState>:
"VC2!" + uint32 LE length + XML (+ NUL). IComponent is a JUCE MemoryBlock::toBase64Encoding:
"<bytes>." followed by 6-bit groups (LSB-first over the little-endian byte stream) in JUCE's own alphabet.
"""
import re, struct

_ALPHA = ".ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+"
_IDX = {c: i for i, c in enumerate(_ALPHA)}


def b64_decode(s: str) -> bytes:
    n, _, body = s.strip().partition(".")
    n = int(n)
    acc = 0
    for i, c in enumerate(body):
        acc |= _IDX[c] << (6 * i)
    return acc.to_bytes((6 * len(body) + 7) // 8, "little")[:n]


def b64_encode(b: bytes) -> str:
    acc = int.from_bytes(b, "little")
    nchars = (len(b) * 8 + 5) // 6
    return f"{len(b)}." + "".join(_ALPHA[(acc >> (6 * i)) & 63] for i in range(nchars))


def xml_from_binary(b: bytes) -> str:
    assert b[:4] == b"VC2!", b[:8]
    n = struct.unpack("<I", b[4:8])[0]
    return b[8:8 + n].rstrip(b"\0").decode("utf-8")


def xml_to_binary(x: str) -> bytes:
    d = x.encode("utf-8") + b"\0"
    return b"VC2!" + struct.pack("<I", len(d)) + d


def split_vst3(state_file_bytes: bytes):
    outer = xml_from_binary(state_file_bytes)
    comp = re.search(r"<IComponent>(.*?)</IComponent>", outer, re.S).group(1)
    return outer, b64_decode(comp)


def replace_component(outer_xml: str, comp: bytes) -> bytes:
    new = re.sub(r"<IComponent>.*?</IComponent>", "<IComponent>" + b64_encode(comp) + "</IComponent>", outer_xml, flags=re.S)
    return xml_to_binary(new)
