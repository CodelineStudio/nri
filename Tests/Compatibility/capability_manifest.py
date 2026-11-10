"""Validate provenance emitted by the test-instrumented NRI device itself."""

import json


def parse_manifest(stderr):
    records = []
    marker = 'NRI_COMPATIBILITY '
    for line in stderr.splitlines():
        if marker in line:
            records.append(json.loads(line.split(marker, 1)[1]))
    result = {}
    for kind in ('instance', 'device'):
        selected = [entry for entry in records if entry.get('kind') == kind]
        if len(selected) != 1:
            raise ValueError(f'Expected exactly one {kind} capability record')
        result[kind] = selected[0]
        extensions = [entry['name'] for entry in records if entry.get('kind') == kind + '_extension']
        if len(extensions) != selected[0].get('extension_count'):
            raise ValueError(f'Incomplete {kind} extension manifest')
        if any(not isinstance(name, str) or not name.startswith('VK_') for name in extensions):
            raise ValueError(f'Invalid {kind} extension name')
        result[kind]['extensions'] = extensions
    active = result['instance'].get('native_validation_active')
    if not isinstance(active, bool):
        raise ValueError('Native validation activation must be observed explicitly')
    for key in ('loader_api', 'instance_api'):
        if type(result['instance'].get(key)) is not int or result['instance'][key] <= 0:
            raise ValueError(f'Missing {key}')
    for key in ('physical_api', 'effective_minor'):
        if type(result['device'].get(key)) is not int or result['device'][key] <= 0:
            raise ValueError(f'Missing {key}')
    for key in ('timeline', 'synchronization2', 'dynamic_rendering'):
        if type(result['device'].get(key)) is not int or result['device'][key] not in (0, 1):
            raise ValueError(f'Invalid enabled {key} feature')
    return result
