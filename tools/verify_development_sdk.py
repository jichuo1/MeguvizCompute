"""Rebuild, test, install and independently consume the local development SDK."""
import argparse
import hashlib
import json
import os
import pathlib
import shutil
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parents[1]
VS = pathlib.Path(r'C:\Program Files\Microsoft Visual Studio\18\Community')
MSVC = VS / 'VC/Tools/MSVC/14.51.36231'
KITS = pathlib.Path(r'C:\Program Files (x86)\Windows Kits\10')
CMAKE = VS / 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
NINJA = VS / 'Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe'


def environment():
    env = os.environ.copy()
    env['PATH'] = ';'.join(map(str, [MSVC/'bin/Hostx64/x64', KITS/'bin/10.0.26100.0/x64', CMAKE.parent, NINJA.parent])) + ';' + env.get('PATH', '')
    env['INCLUDE'] = ';'.join(map(str, [MSVC/'include'] + [KITS/'Include/10.0.26100.0'/x for x in ['ucrt', 'shared', 'um', 'winrt', 'cppwinrt']]))
    env['LIB'] = ';'.join(map(str, [MSVC/'lib/x64', KITS/'Lib/10.0.26100.0/ucrt/x64', KITS/'Lib/10.0.26100.0/um/x64']))
    return env


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--preset', default='windows-cpu', choices=['windows-cpu', 'windows-reference'])
    args = parser.parse_args()
    out = ROOT/'build/reports'/('sdk-' + args.preset + '-' + str(time.time_ns()))
    out.mkdir(parents=True)
    records = []
    env = environment()

    def run(name, argv, expected=0, cwd=ROOT):
        log = out/(name + '.log')
        with log.open('wb') as output:
            result = subprocess.run(list(map(str, argv)), cwd=cwd, env=env, stdout=output, stderr=subprocess.STDOUT, timeout=1200)
        records.append({'name': name, 'argv': list(map(str, argv)), 'cwd': str(cwd), 'exit_code': result.returncode, 'expected_exit': expected, 'log': str(log), 'log_sha256': hashlib.sha256(log.read_bytes()).hexdigest()})
        (out/'commands.json').write_text(json.dumps(records, indent=2)+'\n')
        ok = result.returncode == expected if expected == 0 else result.returncode != 0
        if not ok:
            raise RuntimeError(str(log) + '\n' + log.read_text(errors='replace')[-10000:])

    run('configure', [CMAKE, '--preset', args.preset])
    run('build', [CMAKE, '--build', '--preset', args.preset])
    run('test', [CMAKE.parent/'ctest.exe', '--preset', args.preset, '--output-junit', out/'ctest.xml'])
    prefix = ROOT/'build/install'/args.preset
    run('install', [CMAKE, '--install', ROOT/'build'/args.preset, '--prefix', prefix])
    src = out/'external-source'
    src.mkdir()
    examples = ['c_consumer.c', 'consumer_common.h', 'tensor_statistics.c']
    if args.preset == 'windows-cpu':
        examples += ['headless.cpp', 'ncnn_c_graph.c']
    for name in examples:
        shutil.copyfile(ROOT/'examples'/name, src/name)
    cmake = '''cmake_minimum_required(VERSION 3.31)
project(InstalledComputeConsumer LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
find_package(MeguvizCompute 0.1 CONFIG REQUIRED)
enable_testing()
add_executable(public_c c_consumer.c)
set_target_properties(public_c PROPERTIES LINKER_LANGUAGE CXX)
target_link_libraries(public_c PRIVATE MeguvizCompute::core)
add_test(NAME public_c COMMAND public_c)
add_executable(statistics tensor_statistics.c)
set_target_properties(statistics PROPERTIES LINKER_LANGUAGE CXX)
target_link_libraries(statistics PRIVATE MeguvizCompute::kernels)
add_test(NAME statistics COMMAND statistics)
'''
    if args.preset == 'windows-cpu':
        cmake += '''add_executable(private_graph headless.cpp)
target_link_libraries(private_graph PRIVATE MeguvizCompute::backend_ncnn_sdk)
add_test(NAME private_graph COMMAND private_graph)
add_executable(public_graph ncnn_c_graph.c)
set_target_properties(public_graph PROPERTIES LINKER_LANGUAGE CXX)
target_link_libraries(public_graph PRIVATE MeguvizCompute::core)
add_test(NAME public_graph COMMAND public_graph)
'''
    (src/'CMakeLists.txt').write_text(cmake)
    identity = json.loads((prefix/'lib/cmake/MeguvizCompute/build-info.json').read_text())
    common = ['-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_PREFIX_PATH='+str(prefix)]
    if args.preset == 'windows-cpu':
        common += ['-DMGC_REQUIRED_PRIVATE_SDK_FINGERPRINT='+identity['private_sdk_fingerprint']]
    run('external-configure', [CMAKE, '-S', src, '-B', out/'external-build']+common)
    run('external-build', [CMAKE, '--build', out/'external-build', '--parallel', '4'])
    run('external-test', [CMAKE.parent/'ctest.exe', '--test-dir', out/'external-build', '--output-on-failure', '--output-junit', out/'external-ctest.xml'])
    if args.preset == 'windows-cpu':
        mismatch = src/'mismatch'
        mismatch.mkdir()
        (mismatch/'CMakeLists.txt').write_text('cmake_minimum_required(VERSION 3.31)\nproject(Mismatch CXX)\nfind_package(MeguvizCompute CONFIG REQUIRED)\n')
        run('fingerprint-negative', [CMAKE, '-S', mismatch, '-B', out/'mismatch-build', '-G', 'Ninja', '-DCMAKE_PREFIX_PATH='+str(prefix), '-DMGC_REQUIRED_PRIVATE_SDK_FINGERPRINT='+'1'*64], expected=1)
        duplicate = src/'duplicate'
        duplicate.mkdir()
        (duplicate/'CMakeLists.txt').write_text('cmake_minimum_required(VERSION 3.31)\nproject(Duplicate CXX)\nadd_library(ncnn INTERFACE)\nfind_package(MeguvizCompute CONFIG REQUIRED)\n')
        run('duplicate-ncnn-negative', [CMAKE, '-S', duplicate, '-B', out/'duplicate-build', '-G', 'Ninja', '-DCMAKE_PREFIX_PATH='+str(prefix)], expected=1)
    files = []
    for path in sorted(prefix.rglob('*')):
        if not path.is_file():
            continue
        if path.suffix.lower() in {'.bin', '.param', '.onnx', '.pth', '.so', '.dll', '.exe'}:
            raise RuntimeError('forbidden development package payload: '+str(path))
        files.append({'path': path.relative_to(prefix).as_posix(), 'size': path.stat().st_size, 'sha256': hashlib.sha256(path.read_bytes()).hexdigest()})
    manifest = {'schema_version': 1, 'kind': 'development-static-sdk', 'release_qualified': False, 'profile': args.preset, 'build': identity, 'files': files, 'evidence': str(out), 'test_result': 'PASS', 'gpu_result': 'INCONCLUSIVE - no device execution'}
    (out/'package-manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
    (ROOT/'build/reports'/('latest-'+args.preset+'.json')).write_text(json.dumps({'directory': str(out), 'manifest': str(out/'package-manifest.json'), 'result': 'PASS'}, indent=2)+'\n')
    print(json.dumps({'result': 'PASS', 'report': str(out), 'prefix': str(prefix), 'private_sdk_fingerprint': identity['private_sdk_fingerprint']}))
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except Exception as error:
        print(error, file=sys.stderr)
        sys.exit(1)
