import unittest

from check import check_late_draw_order


class LateDrawOrderTests(unittest.TestCase):
    def records(self, api):
        if api == "dx11":
            return [
                "FrameAnalysisContext(class FrameAnalysisContext@1234)::3DMigoto   "
                "[TextureOverride\\Mods\\Mesh\\mod.ini\\Mesh] DrawIndexed(3, 0, 0)",
                "FrameAnalysisContext(class FrameAnalysisContext@1234)::3DMigoto   "
                "[customshader\\mods\\mesh\\mod.ini\\late] Draw = from_caller -> "
                "DrawIndexedInstanced(3, 1, 3, 0, 0)",
            ]
        return [
            "Mod draw: 3 from 0 indexed, vs 1234 ps 5678, the game's index buffer replaced.",
            "Mod draw: 3 from 3 indexed, vs 1234 ps 9abc (the mod's), the game's index buffer replaced.",
        ]

    def test_replacement_then_parent_replay(self):
        for api in ("dx11", "dx12"):
            with self.subTest(api=api):
                records = self.records(api)
                log = "Configuration: draw = from_caller\n" + "\n".join(records)
                self.assertEqual(len(check_late_draw_order(api, log, records)), 2)

    def test_rejects_reversed_draws(self):
        for api in ("dx11", "dx12"):
            with self.subTest(api=api):
                records = self.records(api)[::-1]
                with self.assertRaisesRegex(AssertionError, "replacement draw before parent replay"):
                    check_late_draw_order(api, "\n".join(records), records)

    def test_rejects_missing_or_repeated_draws(self):
        for api in ("dx11", "dx12"):
            records = self.records(api)
            for invalid in ([], records[:1], records[1:], records + records[:1]):
                with self.subTest(api=api, records=invalid):
                    with self.assertRaises(AssertionError):
                        check_late_draw_order(api, "\n".join(invalid), invalid)


if __name__ == "__main__":
    unittest.main()
