"""Cross-build static SDKs and inspect an independently linked Android DSO."""
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
SDK = pathlib.Path(r'C:\Users\Administrator\AppData\Local\Android\Sdk')
NDK = SDK/'ndk/30.0.16248370'
CMAKE = SDK/'cmake/4.1.2/bin/cmake.exe'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--abi', choices=['arm64', 'v7'], required=True)
    args = parser.parse_args()
    preset = 'android-'+args.abi+'-api27'
    android_abi = 'arm64-v8a' if args.abi == 'arm64' else 'armeabi-v7a'
    out = ROOT/'build/reports'/('sdk-'+preset+'-'+str(time.time_ns()))
    out.mkdir(parents=True)
    env = os.environ.copy()
    env['MEGUVIZ_NDK_ROOT'] = str(NDK)
    env['PATH'] = str(CMAKE.parent)+';'+env.get('PATH', '')
    records = []

    def run(name, argv):
        path = out/(name+'.log')
        with path.open('wb') as output:
            result = subprocess.run(list(map(str, argv)), cwd=ROOT, env=env, stdout=output, stderr=subprocess.STDOUT, timeout=1800)
        records.append({'name': name, 'argv': list(map(str, argv)), 'exit_code': result.returncode, 'log': str(path), 'log_sha256': hashlib.sha256(path.read_bytes()).hexdigest()})
        (out/'commands.json').write_text(json.dumps(records, indent=2)+'\n')
        if result.returncode:
            raise RuntimeError(str(path)+'\n'+path.read_text(errors='replace')[-10000:])
        return path

    run('configure', [CMAKE, '--preset', preset])
    run('build', [CMAKE, '--build', '--preset', preset])
    prefix = ROOT/'build/install'/preset
    run('install', [CMAKE, '--install', ROOT/'build'/preset, '--prefix', prefix])
    info = json.loads((prefix/'lib/cmake/MeguvizCompute/build-info.json').read_text())
    src = out/'external-source'
    src.mkdir()
    (src/'entry.cpp').write_text('''#include <meguviz/backend_ncnn_sdk.hpp>
extern "C" __attribute__((visibility("default"))) int mgc_test_entry() {
  MgcBuildInfo info{};info.header=MGC_HEADER_INIT(MgcBuildInfo);
  ncnn::Net net;net.opt.use_vulkan_compute=false;
  return mgc_query_build_info(&info);
}
''')
    (src/'exports.map').write_text('MEGUVIZ_DEV { global: mgc_*; local: *; };\n')
    (src/'CMakeLists.txt').write_text('''cmake_minimum_required(VERSION 3.31)
project(InstalledAndroidComputeConsumer LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
find_package(MeguvizCompute 0.1 CONFIG REQUIRED)
add_library(compute_closure SHARED entry.cpp)
target_link_libraries(compute_closure PRIVATE MeguvizCompute::backend_ncnn_sdk)
target_link_options(compute_closure PRIVATE "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/exports.map" "-Wl,-z,max-page-size=16384" "-Wl,--no-undefined")
''')
    run('external-configure', [CMAKE, '-S', src, '-B', out/'external-build', '-G', 'Ninja',
                              '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_TOOLCHAIN_FILE='+str(NDK/'build/cmake/android.toolchain.cmake'),
                              '-DANDROID_ABI='+android_abi, '-DANDROID_PLATFORM=android-27', '-DANDROID_STL=c++_static',
                              '-DCMAKE_PREFIX_PATH='+str(prefix), '-DCMAKE_FIND_ROOT_PATH='+str(prefix), '-DMeguvizCompute_DIR='+str(prefix/'lib/cmake/MeguvizCompute'), '-DMGC_REQUIRED_PRIVATE_SDK_FINGERPRINT='+info['private_sdk_fingerprint']])
    run('external-build', [CMAKE, '--build', out/'external-build', '--parallel', '4'])
    dso = out/'external-build/libcompute_closure.so'
    readelf = NDK/'toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-readelf.exe'
    inspection = run('elf', [readelf, '--file-header', '--program-headers', '--dynamic', '--dyn-syms', '--wide', dso])
    raw = inspection.read_text()
    loads = [line.split()[-1] for line in raw.splitlines() if line.strip().startswith('LOAD ')]
    if not loads or any(int(value, 16) < 16384 for value in loads):
        raise RuntimeError('LOAD alignment failed')
    exports = []
    for line in raw.splitlines():
        fields = line.split()
        if len(fields) >= 8 and fields[0].endswith(':') and fields[4] in {'GLOBAL', 'WEAK'} and fields[6] not in {'UND', 'ABS'}:
            name = fields[7].split('@')[0]
            exports.append(name)
            if not name.startswith('mgc_'):
                raise RuntimeError('private symbol exported: '+name)
    if 'mgc_test_entry' not in exports or 'mgc_runtime_create' not in exports:
        raise RuntimeError('public C ABI missing from final link')
    files = [{'path': p.relative_to(prefix).as_posix(), 'size': p.stat().st_size, 'sha256': hashlib.sha256(p.read_bytes()).hexdigest()}
             for p in sorted(prefix.rglob('*')) if p.is_file()]
    report = {'result': 'PASS', 'scope': 'cross_compile_install_link_ELF_only', 'abi': android_abi,
              'profile': preset, 'prefix': str(prefix), 'build': info, 'files': files,
              'link_fixture': {'path': str(dso), 'size': dso.stat().st_size, 'sha256': hashlib.sha256(dso.read_bytes()).hexdigest(), 'exports': exports, 'load_alignment': loads},
              'runtime_qualification': 'INCONCLUSIVE - no ARM device execution', 'page_size_runtime': 'INCONCLUSIVE - ELF alignment is not 16KiB runtime evidence'}
    (out/'package-manifest.json').write_text(json.dumps(report, indent=2)+'\n')
    (ROOT/'build/reports'/('latest-'+preset+'.json')).write_text(json.dumps({'directory': str(out), 'manifest': str(out/'package-manifest.json'), 'result': 'PASS'}, indent=2)+'\n')
    print(json.dumps({'result': 'PASS', 'directory': str(out), 'prefix': str(prefix), 'private_sdk_fingerprint': info['private_sdk_fingerprint']}))


if __name__ == '__main__':
    try:
        main()
    except Exception as error:
        print(error, file=sys.stderr)
        sys.exit(1)
