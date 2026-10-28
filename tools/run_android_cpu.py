"""Run self-owned CPU/native contracts on an explicitly selected x86_64 emulator."""
import argparse
import hashlib
import json
import os
import pathlib
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parents[1]
SDK = pathlib.Path(r'C:\Users\Administrator\AppData\Local\Android\Sdk')
NDK = SDK/'ndk/30.0.16248370'
CMAKE = SDK/'cmake/4.1.2/bin/cmake.exe'
ADB = SDK/'platform-tools/adb.exe'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--serial', choices=['emulator-5700', 'emulator-5702', 'emulator-5732'], required=True)
    args = parser.parse_args()
    run_id = str(time.time_ns())
    out = ROOT/'build/reports'/('android-cpu-'+args.serial+'-'+run_id)
    out.mkdir(parents=True)
    env = os.environ.copy()
    env['PATH'] = str(CMAKE.parent)+';'+env.get('PATH', '')
    records = []

    def run(name, argv):
        path = out/(name+'.log')
        result = subprocess.run(list(map(str, argv)), cwd=ROOT, env=env, capture_output=True, timeout=1800)
        path.write_bytes(result.stdout+result.stderr)
        records.append({'name': name, 'argv': list(map(str, argv)), 'exit_code': result.returncode,
                        'log': str(path), 'log_sha256': hashlib.sha256(path.read_bytes()).hexdigest()})
        (out/'commands.json').write_text(json.dumps(records, indent=2)+'\n')
        if result.returncode:
            raise RuntimeError(str(path)+'\n'+path.read_text(errors='replace')[-10000:])
        return result.stdout.decode('utf-8', errors='replace')

    adb = [ADB, '-s', args.serial]
    run('devices', [ADB, 'devices', '-l'])
    fingerprint = run('fingerprint', adb+['shell', 'getprop', 'ro.build.fingerprint']).strip()
    abi = run('abi', adb+['shell', 'getprop', 'ro.product.cpu.abilist']).strip()
    api = run('api', adb+['shell', 'getprop', 'ro.build.version.sdk']).strip()
    if 'x86_64' not in abi or not fingerprint.startswith(('Android/sdk_phone', 'Android/sdk_gphone', 'google/sdk_gphone')):
        raise RuntimeError('selected device is not the inventoried x86_64 emulator')
    hardware = run('hardware', adb+['shell', 'getprop', 'ro.hardware']).strip()
    if hardware != 'ranchu':
        raise RuntimeError('emulator hardware identity mismatch')
    pagesize = run('page-size', adb+['shell', 'getconf', 'PAGE_SIZE']).strip()
    ps = run('processes', adb+['shell', 'ps', '-A'])
    instrumentation = run('instrumentation', adb+['shell', 'dumpsys', 'activity', 'instrumentation'])
    if 'Active instrumentation' in instrumentation and 'Active instrumentation:' not in instrumentation:
        raise RuntimeError('cannot determine active instrumentation')
    if '* ActiveInstrumentation' in instrumentation or 'instrumentation.watcher' in ps.lower():
        raise RuntimeError('parallel instrumentation detected')
    build = ROOT/'build/android-x86_64-api27'
    configure = [CMAKE, '-S', ROOT, '-B', build, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
                 '-DCMAKE_TOOLCHAIN_FILE='+str(NDK/'build/cmake/android.toolchain.cmake'),
                 '-DANDROID_ABI=x86_64', '-DANDROID_PLATFORM=android-27', '-DANDROID_STL=c++_static',
                 '-DMGC_WITH_NCNN=ON', '-DMGC_VULKAN=OFF', '-DMGC_NCNN_SOURCE='+str(ROOT.parent/'dependencies/ncnn'),
                 '-DNCNN_AVX=OFF', '-DNCNN_AVX2=OFF', '-DNCNN_AVX512=OFF', '-DNCNN_XOP=OFF',
                 '-DNCNN_FMA=OFF', '-DNCNN_F16C=OFF', '-DNCNN_RUNTIME_CPU=OFF']
    run('configure', configure)
    tests = ['mgc_c_consumer', 'mgc_core_conformance', 'mgc_contract_tests', 'mgc_tensor_statistics', 'mgc_ncnn_c_graph', 'mgc_headless']
    run('build', [CMAKE, '--build', build, '--parallel', '4', '--target']+tests)
    remote = '/data/local/tmp/meguviz-01a1814c/run-'+run_id
    run('mkdir', adb+['shell', 'mkdir', '-p', remote])
    binaries = []
    for test in tests:
        binary = build/test
        digest = hashlib.sha256(binary.read_bytes()).hexdigest()
        target = remote+'/'+test
        run('push-'+test, adb+['push', binary, target])
        actual = run('remote-hash-'+test, adb+['shell', 'sha256sum', target]).split()[0]
        if actual != digest:
            raise RuntimeError('device executable hash mismatch')
        run('chmod-'+test, adb+['shell', 'chmod', '700', target])
        run('execute-'+test, adb+['shell', target])
        binaries.append({'name': test, 'host_path': str(binary), 'device_path': target, 'size': binary.stat().st_size, 'sha256': digest, 'exit_code': 0})
    result = {'result': 'PASS', 'scope': 'Android native CPU contracts and real ncnn CPU graphs',
              'device': {'serial': args.serial, 'fingerprint': fingerprint, 'api': int(api), 'abis': abi.split(','), 'hardware': hardware, 'page_size_bytes': int(pagesize)},
              'build_api': 27, 'build_abi': 'x86_64', 'provider': 'CPU_REFERENCE and NCNN_CPU',
              'binaries': binaries, 'gpu_qualification': 'INCONCLUSIVE - CPU-only build, emulator does not qualify ARM/Adreno/Mali',
              'runtime_path': remote, 'instrumentation_preflight': str(out/'instrumentation.log')}
    (out/'result.json').write_text(json.dumps(result, indent=2)+'\n')
    (ROOT/'build/reports/latest-android-cpu.json').write_text(json.dumps({'directory': str(out), 'result': 'PASS'}, indent=2)+'\n')
    print(json.dumps({'result': 'PASS', 'report': str(out), 'scope': result['scope']}))


if __name__ == '__main__':
    try:
        main()
    except Exception as error:
        print(error, file=sys.stderr)
        sys.exit(1)
