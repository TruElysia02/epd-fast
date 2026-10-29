import unittest

from tools.pack_apple import confirm_two_frames


def filtered(values):
    return [frame[0] for frame in confirm_two_frames(bytes([value]) for value in values)]


class TemporalFilterTest(unittest.TestCase):
    def test_single_frame_excursions_in_both_directions(self):
        self.assertEqual(filtered([0, 1, 0]), [0, 0, 0])
        self.assertEqual(filtered([255, 254, 255]), [255, 255, 255])

    def test_persistent_edges_keep_their_original_frame(self):
        for frames in ([0, 0, 255, 255, 255], [255, 255, 0, 0, 0]):
            self.assertEqual(filtered(frames), frames)

    def test_pixels_are_confirmed_independently(self):
        self.assertEqual(filtered([0, 3, 2, 2]), [0, 2, 2, 2])

    def test_repeated_one_frame_flicker_does_not_escape_confirmation(self):
        self.assertEqual(filtered([0, 1, 0, 1, 0]), [0, 0, 0, 0, 0])

    def test_empty_single_and_boundary_frames_keep_their_count(self):
        for frames in ([], [73], [0, 1], [1, 0], [0, 1, 0, 255]):
            result = filtered(frames)
            self.assertEqual(len(result), len(frames))
            if frames:
                self.assertEqual(result[0], frames[0])
                self.assertEqual(result[-1], frames[-1])


if __name__ == '__main__':
    unittest.main()
