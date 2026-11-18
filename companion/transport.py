"""Panel frames and camera commands for the native demo and MatrixPortal."""

from dataclasses import dataclass
from enum import IntEnum
import os
from queue import Empty
import select
import struct
import subprocess
import termios
import time
import tty


MAGIC = b"PTR4"
PREVIOUS_MAGIC = b"PTR3"
LEGACY_MAGIC = b"PTR2"
PANEL_WIDTH = 64
PANEL_HEIGHT = 32
SPRITE_ZOOM = 4
DETAIL_ZOOM = 5
PANEL_BYTES = PANEL_WIDTH * PANEL_HEIGHT * 2
LEGACY_HEADER = struct.Struct("<HHHQQQQBBHHQI")
HEADER = struct.Struct("<HHHQQQQBBHHQIB")
LEGACY_CREATURE = struct.Struct("<QQQIHHBBBQQbb")
CREATURE = struct.Struct("<QQQIHHBBBQQbbB")
U32 = struct.Struct("<I")
MAX_BODY = HEADER.size + PANEL_BYTES + 160 * CREATURE.size


class View(IntEnum):
    CLOSE = 0
    HUNT = 1
    LIFE = 2
    HABITAT = 3


@dataclass(frozen=True)
class Camera:
    view: View
    zoom: int
    left: int = 0
    top: int = 0
    follow: int = 0

    def command(self, revision):
        return f"!{revision} {int(self.view)} {self.zoom} {self.left} {self.top} {self.follow}\n".encode()


@dataclass(frozen=True)
class Creature:
    id: int
    parent: int
    founder: int
    generation: int
    cell: int
    energy: int
    digestion: int
    thrift: int
    moisture_preference: int
    plants_eaten: int
    animal_eaten: int
    x: int
    y: int
    faction: int = 0


@dataclass(frozen=True)
class Observation:
    width: int
    height: int
    tick: int
    births: int
    deaths: int
    kills: int
    camera: Camera
    revision: int
    pixels: bytes
    creatures: dict[int, Creature]
    max_zoom: int = SPRITE_ZOOM
    staggered_rows: bool = False


def checksum(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


class Parser:
    def __init__(self):
        self.pending = bytearray()

    def feed(self, data):
        self.pending.extend(data)
        while True:
            start = self.pending.find(b"PTR")
            if start < 0:
                del self.pending[:-3]
                return
            if start:
                del self.pending[:start]
            if len(self.pending) < 8:
                return
            magic = self.pending[:4]
            if magic not in (MAGIC, PREVIOUS_MAGIC, LEGACY_MAGIC):
                del self.pending[:3]
                continue
            size = U32.unpack_from(self.pending, 4)[0]
            if size < LEGACY_HEADER.size or size > MAX_BODY:
                del self.pending[:4]
                continue
            end = 8 + size
            if len(self.pending) < end + 4:
                return
            body = bytes(self.pending[8:end])
            expected = U32.unpack_from(self.pending, end)[0]
            if checksum(body) != expected:
                del self.pending[:4]
                continue
            del self.pending[:end + 4]
            observation = self.decode(body, int(chr(magic[-1])))
            if observation:
                yield observation

    @staticmethod
    def decode(data, version):
        header = HEADER if version == 4 else LEGACY_HEADER
        if len(data) < header.size:
            return None
        fields = header.unpack_from(data)
        layout = fields[-1] if version == 4 else 0
        if layout > 1:
            return None
        width, height, count, tick, births, deaths, kills, view, zoom, left, top, follow, revision = (
            fields[:13])
        max_zoom = SPRITE_ZOOM if version == 2 else DETAIL_ZOOM
        if (width, height) not in ((64, 32), (128, 64)) or count > 160 or view > 3 or zoom > max_zoom:
            return None
        offset = header.size + PANEL_BYTES
        record = LEGACY_CREATURE if version == 2 else CREATURE
        if offset + count * record.size != len(data):
            return None
        creatures = {}
        for values in record.iter_unpack(data[offset:]):
            creature = Creature(*values)
            if (creature.id == 0 or creature.id in creatures or creature.cell >= width * height or
                    creature.faction > 4):
                return None
            creatures[creature.id] = creature
        camera = Camera(View(view), zoom, left, top, follow)
        return Observation(width, height, tick, births, deaths, kills, camera, revision,
                           data[header.size:offset], creatures, max_zoom, bool(layout))


def exchange(read_fd, write_fd, commands, stopped, timeout_message):
    parser = Parser()
    outgoing = b""
    queued = None
    requested_at = None
    request_at = time.monotonic()
    last_snapshot = request_at
    while not stopped.is_set():
        now = time.monotonic()
        try:
            while True:
                queued = commands.get_nowait()
        except Empty:
            pass
        if not outgoing and (queued is not None or (requested_at is None and now >= request_at)
                             or (requested_at is not None and now - requested_at > 0.5)):
            outgoing = (queued or b"") + b"?"
            queued = None
        if now - last_snapshot > 7:
            raise OSError(timeout_message)
        readable, writable, _ = select.select([read_fd], [write_fd] if outgoing else [], [], 0.025)
        if writable:
            try:
                outgoing = outgoing[os.write(write_fd, outgoing):]
            except BlockingIOError:
                pass
            if not outgoing:
                requested_at = time.monotonic()
        if not readable:
            continue
        try:
            data = os.read(read_fd, 8192)
        except BlockingIOError:
            continue
        if not data:
            raise OSError("The panel connection closed")
        for snapshot in parser.feed(data):
            last_snapshot = time.monotonic()
            request_at = (requested_at or last_snapshot) + 1 / 12
            requested_at = None
            yield snapshot


def demo_observations(binary, seed, commands, stopped):
    with subprocess.Popen([str(binary), str(seed)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                          stderr=subprocess.DEVNULL) as process:
        try:
            yield from exchange(process.stdout.fileno(), process.stdin.fileno(), commands, stopped,
                                "No response from the demo. Rebuild petri_observer.")
        finally:
            if process.poll() is None:
                process.terminate()


def usb_observations(device, commands, stopped):
    fd = os.open(device, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    try:
        tty.setraw(fd)
        attrs = termios.tcgetattr(fd)
        attrs[4] = attrs[5] = termios.B115200
        termios.tcsetattr(fd, termios.TCSANOW, attrs)
        yield from exchange(fd, fd, commands, stopped,
                            "No panel frames. Flash the Panel Mirror UF2 and reconnect.")
    finally:
        os.close(fd)
