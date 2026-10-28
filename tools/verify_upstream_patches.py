"""Verify exact dependency commits and refuse unregistered source patches."""
import argparse
import hashlib
import json
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]


def git(source, *args):
    p = subprocess.run(['git', '-c', 'safe.directory='+source.as_posix(), '-C', str(source), *args], capture_output=True, text=True, encoding='utf-8')
    if p.returncode:
        raise RuntimeError(p.stderr)
    return p.stdout.strip()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--lock', default=str(ROOT/'third_party/locks/ncnn.lock.json'))
    parser.add_argument('--source', default=str(ROOT.parent/'dependencies/ncnn'))
    parser.add_argument('--report', required=True)
    args = parser.parse_args()
    lock = json.loads(pathlib.Path(args.lock).read_text())
    if lock['patches']:
        raise RuntimeError('This conservative profile does not contain an async patch series')
    source = pathlib.Path(args.source).resolve()
    result = {'result': 'PASS', 'scope': 'exact unpatched dependency sources', 'patches': [], 'sources': []}
    for name, path in [('ncnn', source), ('glslang', source/'glslang')]:
        actual = git(path, 'rev-parse', 'HEAD')
        if actual != lock[name]['commit']:
            raise RuntimeError(name+' commit mismatch')
        if git(path, 'status', '--porcelain', '--untracked-files=normal'):
            raise RuntimeError(name+' source has unregistered changes')
        result['sources'].append({'name': name, 'path': str(path), 'commit': actual, 'tree': git(path, 'rev-parse', 'HEAD^{tree}'), 'license_sha256': hashlib.sha256((path/'LICENSE.txt').read_bytes()).hexdigest()})
    pathlib.Path(args.report).write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps(result))


if __name__ == '__main__':
    try:
        main()
    except Exception as error:
        print(error, file=sys.stderr)
        sys.exit(1)
