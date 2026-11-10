import copy
import json
import unittest

from capability_manifest import parse_manifest


class ManifestTests(unittest.TestCase):
    def setUp(self):
        self.records = [
            dict(kind='instance', loader_api=4210688, instance_api=4210688,
                 native_validation_active=False, extension_count=1),
            dict(kind='instance_extension', name='VK_KHR_surface'),
            dict(kind='device', physical_api=4210688, effective_minor=4,
                 timeline=1, synchronization2=1, dynamic_rendering=1, extension_count=1),
            dict(kind='device_extension', name='VK_KHR_swapchain'),
        ]

    def encode(self, records):
        return '\n'.join('[NRI 0] source:1: VK::device NRI_COMPATIBILITY ' + json.dumps(r) for r in records)

    def test_unavailable_validation_is_false_not_unknown(self):
        manifest = parse_manifest(self.encode(self.records))
        self.assertIs(manifest['instance']['native_validation_active'], False)
        self.assertEqual(manifest['device']['extensions'], ['VK_KHR_swapchain'])

    def test_missing_or_truncated_provenance_rejected(self):
        for index in range(len(self.records)):
            with self.subTest(index=index), self.assertRaises(ValueError):
                parse_manifest(self.encode(self.records[:index] + self.records[index + 1:]))

    def test_multiple_devices_cannot_be_misassociated(self):
        with self.assertRaises(ValueError):
            parse_manifest(self.encode(self.records + [self.records[2]]))

    def test_unknown_or_malformed_feature_rejected(self):
        for field, value in [('timeline', None), ('synchronization2', 2), ('physical_api', '1.4')]:
            records = copy.deepcopy(self.records)
            records[2][field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                parse_manifest(self.encode(records))

    def test_string_false_is_not_validation_activation(self):
        self.records[0]['native_validation_active'] = 'false'
        with self.assertRaises(ValueError):
            parse_manifest(self.encode(self.records))


if __name__ == '__main__':
    unittest.main()
