#!/usr/bin/env python3
"""Compile actual standard record/handoff/image core with Windows native MSVC."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import time

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument("--output",type=Path,required=True)
    parser.add_argument("--vsdev",type=Path,default=Path("C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/Tools/VsDevCmd.bat"))
    args=parser.parse_args()
    root=Path(__file__).resolve().parents[1]
    output=args.output.resolve()
    if root.parent.parent==output or root.parent.parent in output.parents:
        raise SystemExit("Output must be outside the installed Skill")
    if os.name!="nt" or not args.vsdev.is_file():
        raise SystemExit("Windows native MSVC required; use --vsdev for its location")
    for path in (root,output,args.vsdev):
        if any(c in str(path) for c in '\r\n%!"&|<>^'):
            raise SystemExit("CMD test paths must not contain shell metacharacters")
    output.mkdir(parents=True,exist_ok=True)
    run=output/time.strftime("protocol-%Y%m%d-%H%M%S");run.mkdir()
    quote=lambda p:'"'+str(p)+'"'
    core=root/"assets"/"pocket-standard";compat=root/"tests"/"compat"
    (run/"pocket").mkdir()
    for header in core.glob("*.h"):
        (run/"pocket"/header.name).write_text('#include "'+header.as_posix()+'"\n',encoding="utf-8")
    compile=("cl /nologo /EHsc /std:c++17 /utf-8 /W4 /WX /wd4267 /wd4244 /wd4996 /wd4127 /wd4505 "
             "/D_CRT_SECURE_NO_WARNINGS /D_CRT_NONSTDC_NO_DEPRECATE /I"+quote(core)+" /I"+quote(compat)+
             " /FI"+quote(compat/"unistd.h")+" "+quote(root/"tests"/"test_standard.cpp")+" "+
             " ".join(quote(core/f) for f in ("pocket_core.cc","pocket_handoff.cc","pocket_image.cc"))+
             " /Fe:standard.exe bcrypt.lib")
    bridge=compile.replace(quote(root/"tests"/"test_standard.cpp"),quote(root/"tests"/"test_bridge.cpp"))
    bridge=bridge.replace("/Fe:standard.exe",quote(root/"assets"/"plus_pocket_bridge.cc")+" /I"+quote(run)+" /I"+quote(root/"assets")+" /Fe:bridge.exe")
    commands=["@echo off","chcp 65001 >nul","call "+quote(args.vsdev)+" -arch=x64 >nul",
              "if errorlevel 1 exit /b 1",compile,"if errorlevel 1 exit /b 1",
              "standard.exe fixtures","if errorlevel 1 exit /b 1",
              bridge,"if errorlevel 1 exit /b 1","bridge.exe","if errorlevel 1 exit /b 1","exit /b 0"]
    batch=run/"run.cmd";batch.write_text("\n".join(commands)+"\n",encoding="utf-8",newline="\r\n")
    result=subprocess.run(["cmd.exe","/d","/c",str(batch)],cwd=run,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=180)
    log=result.stdout.decode("utf-8",errors="replace");(run/"host.log").write_text(log,encoding="utf-8")
    report={"exit_code":result.returncode,"evidence":"actual production RecordStore, JSON codec, raw Wi-Fi offer/result and Flash image reader",
            "host_crypto":"Windows CNG SHA256/MD5 via host-only mbedTLS adapters",
            "not_tested":["real TF power interruption","ESP OTA return execution","screen","USB","device dual-mode acceptance"]}
    (run/"result.json").write_text(json.dumps(report,ensure_ascii=False,indent=2)+"\n",encoding="utf-8")
    print(log);print("Evidence:",run)
    raise SystemExit(result.returncode)
if __name__=="__main__":
    main()
