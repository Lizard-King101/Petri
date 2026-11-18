import unittest

from transport import (Camera, CREATURE, DETAIL_ZOOM, HEADER, LEGACY_CREATURE, LEGACY_HEADER, LEGACY_MAGIC,
                       MAGIC, PANEL_BYTES, PREVIOUS_MAGIC, SPRITE_ZOOM, U32, Parser, View, checksum)


def packet(tick, cell=100, zoom=SPRITE_ZOOM, faction=3, legacy=False, version=4, layout=0):
    version = 2 if legacy else version
    fields = (64, 32, 1, tick, 8, 3, 0, View.CLOSE, zoom, 3, 5, 6, 12)
    body = HEADER.pack(*fields, layout) if version == 4 else LEGACY_HEADER.pack(*fields)
    body += bytes((0x00, 0x20)) * (PANEL_BYTES // 2)
    creature = (6, 2, 2, 1, cell, 90, 8, 9, 10, 25, 4, 35, 17)
    body += LEGACY_CREATURE.pack(*creature) if version == 2 else CREATURE.pack(*creature, faction)
    magic = {2: LEGACY_MAGIC, 3: PREVIOUS_MAGIC, 4: MAGIC}[version]
    return magic + U32.pack(len(body)) + body + U32.pack(checksum(body))


class ParserTest(unittest.TestCase):
    def test_fragmented_snapshot_after_metrics(self):
        stream = b"tick=16 tps=1 fps=20\n" + packet(17)
        parser = Parser()
        frames = [frame for i in range(0, len(stream), 13)
                  for frame in parser.feed(stream[i:i + 13])]
        self.assertEqual(len(frames), 1)
        snapshot = frames[0]
        self.assertEqual((snapshot.width, snapshot.height, snapshot.tick), (64, 32, 17))
        self.assertEqual(snapshot.pixels[:2], bytes((0x00, 0x20)))
        self.assertEqual(snapshot.camera, Camera(View.CLOSE, 4, 3, 5, 6))
        self.assertEqual(snapshot.camera.command(snapshot.revision), b"!12 0 4 3 5 6\n")
        self.assertEqual((snapshot.creatures[6].x, snapshot.creatures[6].y), (35, 17))
        self.assertEqual(snapshot.creatures[6].parent, 2)
        self.assertEqual(snapshot.creatures[6].animal_eaten, 4)
        self.assertEqual(snapshot.creatures[6].faction, 3)
        self.assertFalse(snapshot.staggered_rows)

    def test_detail_and_legacy_camera_capabilities(self):
        parser = Parser()
        legacy, previous, detail = list(parser.feed(packet(17, legacy=True) + packet(18, zoom=DETAIL_ZOOM, version=3)
                                                   + packet(19, zoom=DETAIL_ZOOM)))
        self.assertEqual((legacy.max_zoom, legacy.creatures[6].faction), (SPRITE_ZOOM, 0))
        self.assertEqual((detail.max_zoom, detail.camera.zoom), (DETAIL_ZOOM, DETAIL_ZOOM))
        self.assertEqual(detail.camera.command(13), b"!13 0 5 3 5 6\n")
        self.assertFalse(legacy.staggered_rows)
        self.assertFalse(previous.staggered_rows)
        self.assertEqual(previous.max_zoom, DETAIL_ZOOM)
        staggered, = parser.feed(packet(20, layout=1))
        self.assertTrue(staggered.staggered_rows)
        malformed = packet(20, zoom=DETAIL_ZOOM, legacy=True) + packet(21, faction=5)
        malformed += packet(22, layout=2)
        self.assertEqual(list(parser.feed(malformed)), [])

    def test_recovery_after_invalid_snapshots(self):
        damaged = bytearray(packet(17))
        damaged[-1] ^= 0x80
        parser = Parser()
        frames = list(parser.feed(damaged + packet(18, cell=2048) + packet(19)))
        self.assertEqual([frame.tick for frame in frames], [19])


if __name__ == "__main__":
    unittest.main()
