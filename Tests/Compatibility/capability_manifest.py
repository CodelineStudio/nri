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
    for key in ('host_query_reset', 'multi_draw_indirect', 'draw_indirect_first_instance', 'shader_draw_parameters'):
        if key in result['device']:
            value = result['device'][key]
            if type(value) is not int or value not in (0, 1):
                raise ValueError('Invalid enabled ' + key + ' feature')
    # Added chain records are optional for historical receipts. Strict profile
    # checks below require them for new individual-structure qualification.
    for kind in ('feature', 'property'):
        selected = [entry for entry in records if entry.get('kind') == kind + '_chain']
        structures = [entry.get('type') for entry in records if entry.get('kind') == kind + '_structure']
        if selected or structures:
            if len(selected) != 1 or selected[0].get('structure_count') != len(structures):
                raise ValueError(f'Incomplete {kind} chain manifest')
            if not structures or any(type(value) is not int or value <= 0 for value in structures):
                raise ValueError(f'Invalid {kind} structure type')
            if len(structures) != len(set(structures)):
                raise ValueError(f'Duplicate {kind} structure type')
            if type(selected[0].get('individual')) is not int or selected[0]['individual'] not in (0, 1):
                raise ValueError(f'Invalid {kind} chain selection')
            result[kind + '_chain'] = dict(selected[0], structure_types=structures)
    return result


def require_individual_features(manifest):
    # Vulkan's fixed VkStructureType ABI: aggregate Vulkan11/12 structures
    # are 49..52 and were introduced by Vulkan1.2, despite the names.
    required = {
        # 16-bit storage, multiview, variable pointers, protected memory,
        # sampler YCbCr conversion and shader draw parameters.
        'feature': {1000083000, 1000053001, 1000120000, 1000145001, 1000156004, 1000063000},
        # ID, subgroup, point clipping, multiview, protected memory, maintenance3.
        'property': {1000071004, 1000094000, 1000117000, 1000053002, 1000145002, 1000168000},
    }
    for kind, forbidden in (('feature', {49, 51}), ('property', {50, 52})):
        chain = manifest.get(kind + '_chain')
        if not chain or chain['individual'] != 1:
            raise ValueError(f'Required individual {kind} chain is not active')
        if forbidden.intersection(chain['structure_types']):
            raise ValueError(f'Aggregate structures present in individual {kind} chain')
        if not required[kind].issubset(chain['structure_types']):
            raise ValueError(f'Missing core1.1 individual {kind} structures')
