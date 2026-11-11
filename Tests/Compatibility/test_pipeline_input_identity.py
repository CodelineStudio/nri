import copy
import unittest

from artifact_cache import identity_key
from cached_pipeline_inputs import pipeline_identity


class RuntimePipelineIdentityTests(unittest.TestCase):
    def test_runtime_shader_changes_cannot_reuse_embedded_or_other_variant(self):
        receipt = {'status': 'passed', 'binary_sha256': 'same-fixture',
                   'fixture': {'fixture': 'nri_vulkan_shader_execution', 'driver_version_raw': 1,
                               'shader_manifest': {'shaders': {'compute': {'binary_sha256': 'embedded'}}}},
                   'pipeline_cache': {'output': {'vendor_id': 1, 'device_id': 2, 'pipeline_cache_uuid': 'uuid', 'header_version': 1}},
                   'capability_manifest': {'device': {'effective_minor': 1}}}
        baseline = identity_key(pipeline_identity(receipt))
        receipt['runtime_compute'] = {'sha256': 'generated-a', 'entry': 'generatedMain', 'xor': 17, 'validator_sha256': 'validator'}
        generated = identity_key(pipeline_identity(receipt))
        self.assertNotEqual(baseline, generated)
        for field, changed in [('sha256', 'generated-b'), ('entry', 'otherMain'), ('validator_sha256', 'new-validator'), ('xor', 0)]:
            variant = copy.deepcopy(receipt)
            variant['runtime_compute'][field] = changed
            self.assertNotEqual(generated, identity_key(pipeline_identity(variant)), field)
        self.assertEqual(generated, identity_key(pipeline_identity(copy.deepcopy(receipt))))


if __name__ == '__main__':
    unittest.main()
