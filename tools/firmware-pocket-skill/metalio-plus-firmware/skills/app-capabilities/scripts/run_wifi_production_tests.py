#!/usr/bin/env python3
"""Compile unchanged production Wi-Fi connection .cc against host SDK stubs.

--baseline-wifi is an OPTIONAL explicitly supplied old module directory; installed
Skill tests are self-contained without it. Nothing is written into either source.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time

def main():
    p=argparse.ArgumentParser()
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--baseline-wifi',type=Path)
    p.add_argument('--vsdev',type=Path,default=Path('C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/Tools/VsDevCmd.bat'))
    a=p.parse_args()
    if os.name!='nt': raise SystemExit('Native Windows/MSVC runner required')
    root=Path(__file__).resolve().parents[1]
    assets=root.parent/'pocket-integration'/'assets'
    wifi=assets/'wifi-standard';core=assets/'pocket-standard'
    output=a.output.resolve()
    installed=root.parent.parent
    if output==installed or installed in output.parents: raise SystemExit('Output must be outside installed Skill')
    paths=[root,assets,output,a.vsdev]
    if a.baseline_wifi: paths.append(a.baseline_wifi.resolve())
    if any(any(c in str(path) for c in '\r\n%!"&|<>^') for path in paths):
        raise SystemExit('CMD paths must not contain shell metacharacters')
    run=output/time.strftime('sdk-%Y%m%d-%H%M%S');run.mkdir(parents=True)
    def q(path):return '"'+str(path)+'"'
    def command(source,exe,baseline=False):
        includes=[source,root/'tests'/'wifi_sdk_stubs',root/'tests'/'compat',root/'examples',core,wifi]
        return ('cl /nologo /EHsc /std:c++17 /utf-8 /W4 /WX /wd4267 /wd4244 /wd4996 /wd4127 '
            '/D_CRT_SECURE_NO_WARNINGS /D_CRT_NONSTDC_NO_DEPRECATE '+
            ('/DWIFI_BASELINE ' if baseline else '')+
            ' '.join('/I'+q(i) for i in includes)+' /FI'+q(root/'tests'/'compat'/'unistd.h')+' '+
            q(root/'tests'/'test_wifi_production.cpp')+' '+q(wifi/'pocket_wifi.cc')+' '+q(core/'pocket_core.cc')+' /Fe:'+exe)
    commands=['@echo off','chcp 65001 >nul','call '+q(a.vsdev)+' -arch=x64 >nul','if errorlevel 1 exit /b 1',
              command(wifi,'production.exe'),'if errorlevel 1 exit /b 1',
              'production.exe fixture-production','if errorlevel 1 exit /b 1']
    if a.baseline_wifi:
        commands += [command(a.baseline_wifi.resolve(),'baseline.exe',True),'if errorlevel 1 exit /b 1',
                     'baseline.exe fixture-baseline','if errorlevel 1 exit /b 1']
    commands+=['exit /b 0']
    batch=run/'run.cmd';batch.write_text('\n'.join(commands)+'\n',encoding='utf-8',newline='\r\n')
    result=subprocess.run(['cmd.exe','/d','/c',str(batch)],cwd=run,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=180)
    log=result.stdout.decode('utf-8',errors='replace');(run/'host.log').write_text(log,encoding='utf-8')
    sources={str(path.relative_to(installed)):hashlib.sha256(path.read_bytes()).hexdigest()
             for directory in [wifi,core,root/'tests'/'wifi_sdk_stubs'] for path in directory.rglob('*') if path.is_file()}
    for path in [root/'tests'/'test_wifi_production.cpp',root/'examples'/'network_device_setup.h',Path(__file__).resolve()]:
        sources[str(path.relative_to(installed))]=hashlib.sha256(path.read_bytes()).hexdigest()
    baseline_sources={path.name:hashlib.sha256(path.read_bytes()).hexdigest()
                      for path in a.baseline_wifi.glob('*') if path.is_file()} if a.baseline_wifi else {}
    report={'baseline_sources_sha256':baseline_sources,'exit_code':result.returncode,'scope':'unchanged production connection .cc + real credentials/RecordStore, deterministic SDK seam',
            'passes':[line[5:] for line in log.splitlines() if line.startswith('PASS ')],
            'sources_sha256':sources,'not_tested':['actual SDK scheduler/radio timing','ESP32-S31 target build','real hotspot','device UI or true TF power failure']}
    (run/'result.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
    print(log);print('Evidence:',run);raise SystemExit(result.returncode)
if __name__=='__main__':main()
