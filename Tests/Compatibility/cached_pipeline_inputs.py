"""Compatibility identity/admission for the standalone Vulkan pipeline fixture."""
import hashlib
import struct


def pipeline_identity(receipt):
    if receipt.get('status') != 'passed' or receipt['fixture']['fixture'] != 'nri_vulkan_shader_execution':
        raise ValueError('Expected a passing shader fixture receipt')
    fixture = receipt['fixture']
    header = receipt['pipeline_cache']['output']
    return {'kind': 'driver_pipeline', 'backend': 'nri_vulkan', 'schema': 1,
            'vendor_id': header['vendor_id'], 'device_id': header['device_id'],
            'driver_version': fixture['driver_version_raw'], 'native_uuid': header['pipeline_cache_uuid'],
            'header_version': header['header_version'], 'device_profile': receipt['capability_manifest']['device'],
            # The fixture executable contains every pipeline/format/layout state.
            # A shipping engine should serialize its actual PSO descriptions.
            'pipeline_fixture_binary': receipt['binary_sha256'],
            'shaders': {name: shader['binary_sha256'] for name, shader in fixture['shader_manifest']['shaders'].items()}}


def admit_pipeline(data, identity):
    if len(data) < 32:
        return False
    size, version, vendor, device = struct.unpack_from('<4I', data)
    return (size == 32 and version == identity['header_version'] == 1
            and vendor == identity['vendor_id'] and device == identity['device_id']
            and data[16:32].hex() == identity['native_uuid'])


def verified_pipeline_source(receipt, data):
    identity = pipeline_identity(receipt)
    if hashlib.sha256(data).hexdigest() != receipt['pipeline_cache']['output']['sha256'] or not admit_pipeline(data, identity):
        raise ValueError('Pipeline payload does not match its producing receipt')
    return identity
