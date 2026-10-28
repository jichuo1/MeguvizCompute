"""Independent host integrity checker; never grants a GPU/device qualification."""
import argparse
import hashlib
import json
import pathlib
import re
import struct
import sys


def canonical(v):
    return json.dumps(v, sort_keys=True, ensure_ascii=False, separators=(",", ":")).encode()


def sha(data):
    return hashlib.sha256(data).hexdigest()


def reject_duplicates(pairs):
    value = {}
    for k, v in pairs:
        if k in value:
            raise ValueError("duplicate JSON key")
        value[k] = v
    return value


def read_json(path):
    data = path.read_bytes()
    if len(data) > 8 * 1024 * 1024:
        raise ValueError("JSON size exceeds bound")
    return json.loads(data, object_pairs_hook=reject_duplicates,
                      parse_constant=lambda x: (_ for _ in ()).throw(ValueError(x)))


def digest_field(value):
    if not isinstance(value, str) or not re.fullmatch(r"[0-9a-f]{64}", value) or value == "0" * 64:
        raise ValueError("invalid digest")


def content(root, relative, expected, size=None):
    digest_field(expected)
    if not isinstance(relative, str) or not relative or ":" in relative or "\\" in relative:
        raise ValueError("invalid artifact path")
    p = pathlib.PurePosixPath(relative)
    if p.is_absolute() or ".." in p.parts:
        raise ValueError("artifact traversal")
    resolved = (root / relative).resolve(strict=True)
    if root not in resolved.parents or not resolved.is_file():
        raise ValueError("artifact escaped pack")
    length = resolved.stat().st_size
    if length > 64 * 1024 * 1024 or size is not None and length != size:
        raise ValueError("artifact size mismatch")
    data = resolved.read_bytes()
    if sha(data) != expected:
        raise ValueError("artifact digest mismatch")
    return data


def spirv_interface(data, artifact, profile):
    if len(data) < 20 or len(data) % 4:
        raise ValueError("SPIR-V truncated")
    words = struct.unpack("<" + "I" * (len(data) // 4), data)
    if words[0] != 0x07230203 or not 0 < words[3] <= 1048576 or words[4] != 0:
        raise ValueError("SPIR-V header invalid")
    names, members, decorations, entries = {}, {}, {}, []
    def text(v):
        return struct.pack("<" + "I" * len(v), *v).split(b"\0", 1)[0].decode("utf-8")
    cursor = 5
    while cursor < len(words):
        count, opcode = words[cursor] >> 16, words[cursor] & 65535
        if not count or cursor + count > len(words):
            raise ValueError("SPIR-V instruction bounds")
        v = words[cursor:cursor + count]
        if opcode == 5 and count >= 3:
            names[v[1]] = text(v[2:])
        if opcode == 6 and count >= 4:
            members.setdefault(v[1], set()).add(v[2])
        if opcode == 71 and count >= 4 and v[2] in (1, 33, 34):
            decorations.setdefault(v[1], {})[v[2]] = v[3]
        if opcode == 15 and count >= 4 and v[1] == 5:
            entries.append(text(v[3:]))
        cursor += count
    if artifact['entry_point'] not in entries:
        raise ValueError("compute entry point missing")
    if profile == "ncnn-compat":
        descriptors = artifact['layout_abi']['descriptor_sets']
        bindings = sorted(x.get('binding') for x in descriptors)
        if bindings != list(range(len(bindings))) or len(bindings) > 16:
            raise ValueError("ncnn binding ABI")
        for x in descriptors:
            if x.get('set') != 0 or x.get('count') != 1:
                raise ValueError("ncnn set/array ABI")
        actual = sorted((d.get(34), d[33]) for d in decorations.values() if 33 in d)
        if actual != [(0, x) for x in bindings]:
            raise ValueError("descriptor reflection differs from SPIR-V")
        pushes = artifact['layout_abi']['push_constants']
        if pushes:
            parameter = [i for i, name in names.items() if name == 'parameter']
            if len(parameter) != 1 or len(pushes) != 1:
                raise ValueError("ncnn parameter names missing")
            count = len(members.get(parameter[0], ()))
            if pushes[0].get('size') != count * 4:
                raise ValueError("ncnn push ABI differs from named members")
        specs = sorted(x.get('id') for x in artifact['layout_abi']['specialization_constants'])
        if specs != list(range(len(specs))):
            raise ValueError("ncnn specialization IDs")
    return {'spirv_words': len(words), 'entry_point_found': True,
            'ncnn_named_abi_checked': profile == 'ncnn-compat'}


def verify(pack, variant, profile, effective_hash):
    root = pathlib.Path(pack).resolve(strict=True)
    manifest = read_json(root / 'manifest.json')
    known = {'schema_version', 'abi_version', 'profile_id', 'profile_version', 'required_flags',
             'source_closure_hash', 'toolchain_lock_hash', 'artifacts', 'manifest_hash'}
    if set(manifest) != known or manifest['schema_version'] != 1 or manifest['abi_version']['major'] != 0:
        raise ValueError("manifest schema/ABI mismatch")
    if manifest['required_flags'] or manifest['profile_id'] != profile:
        raise ValueError("required flag or profile mismatch")
    claimed = manifest.pop('manifest_hash')
    digest_field(claimed)
    if sha(canonical(manifest)) != claimed:
        raise ValueError("manifest digest mismatch")
    for key in ('source_closure_hash', 'toolchain_lock_hash'):
        digest_field(manifest[key])
    if not isinstance(manifest['artifacts'], list) or not 1 <= len(manifest['artifacts']) <= 65536:
        raise ValueError("artifact count")
    keys = [a['variant_key'] for a in manifest['artifacts']]
    if len(set(keys)) != len(keys):
        raise ValueError("duplicate variant")
    found = [a for a in manifest['artifacts'] if a['variant_key'] == variant]
    if len(found) != 1:
        raise ValueError("variant miss")
    artifact = found[0]
    if artifact['effective_options_hash'] != effective_hash:
        raise ValueError("effective Options mismatch")
    data = content(root, artifact['spirv_path'], artifact['spirv_sha256'], artifact['spirv_size'])
    if 'reflection_path' not in artifact or 'reflection_sha256' not in artifact:
        raise ValueError("independent reflection evidence missing")
    reflection = json.loads(content(root, artifact['reflection_path'], artifact['reflection_sha256']))
    if reflection.get('layout_abi') != artifact['layout_abi'] or reflection.get('entry_point') != artifact['entry_point']:
        # Reflection document is bound to the full SPIR-V and named entry point.
        raise ValueError("reflection content differs from layout ABI")
    interface = spirv_interface(data, artifact, profile)
    return {'result': 'PASS', 'scope': 'host_artifact_integrity_and_static_ncnn_abi',
            'manifest_sha256': claimed, 'variant_key': variant, 'spirv_sha256': artifact['spirv_sha256'],
            'required_caps': artifact['required_caps'], 'interface': interface,
            'runtime_pipeline': 'INCONCLUSIVE - not executed',
            'device_numeric_qualification': 'INCONCLUSIVE - not executed'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pack', required=True)
    parser.add_argument('--variant', required=True)
    parser.add_argument('--profile', choices=('ncnn-compat', 'native-vulkan'), required=True)
    parser.add_argument('--effective-options-hash', required=True)
    parser.add_argument('--report', required=True)
    args = parser.parse_args()
    try:
        result = verify(args.pack, args.variant, args.profile, args.effective_options_hash)
        code = 0
    except (ValueError, KeyError, TypeError, OSError, struct.error) as error:
        result = {'result': 'FAIL', 'scope': 'host_artifact_integrity', 'reason': str(error)}
        code = 1
    pathlib.Path(args.report).write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(result, ensure_ascii=False))
    return code

if __name__ == '__main__':
    sys.exit(main())
