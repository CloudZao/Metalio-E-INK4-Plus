"""Hash actual project inputs and build outputs without archiving the whole SDK."""
from pathlib import Path
import argparse, datetime, hashlib, json

def file_entry(path, relative=None):
    data=path.read_bytes()
    return dict(path=relative or str(path),bytes=len(data),sha256=hashlib.sha256(data).hexdigest())

def capture(project, build, output, stage):
    project=project.resolve();build=build.resolve();output=output.resolve()
    if output.exists():raise ValueError('Evidence is append-only; select a new output file')
    sources=[]
    for path in sorted(project.rglob('*')):
        if not path.is_file() or path.is_relative_to(build) or path==output:continue
        rel=path.relative_to(project)
        if any(x in ('.git','__pycache__','.cache','evidence') for x in rel.parts):continue
        if path.name in ('sdkconfig','sdkconfig.old'):continue # CMake may generate these; captured separately after build.
        sources.append(file_entry(path,rel.as_posix()))
    configs=[file_entry(project/x,x) for x in ('sdkconfig','sdkconfig.defaults','sdkconfig.defaults.esp32s31','app-config.cmake') if (project/x).is_file()]
    outputs=[]
    if stage=='after' and build.exists():
        for path in sorted(build.rglob('*')):
            if path.is_file() and (path.suffix.lower() in ('.bin','.elf','.map') or path.name in ('flasher_args.json','project_description.json','compile_commands.json')):
                outputs.append(file_entry(path,path.relative_to(build).as_posix()))
    inputs=[]
    commands=build/'compile_commands.json'
    if stage=='after' and commands.is_file():
        for entry in json.loads(commands.read_text('utf-8')):
            path=Path(entry['file'])
            if not path.is_absolute():path=Path(entry['directory'])/path
            if path.is_file():inputs.append(file_entry(path.resolve()))
    report=dict(schema=1,stage=stage,created_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
                project=str(project),build=str(build),sources=sources,effective_configs=configs,
                actual_compiled_inputs=inputs,outputs=outputs,hardware_verified=False)
    output.parent.mkdir(parents=True,exist_ok=True)
    output.write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n','utf-8')
    return report

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('project','build','output'):p.add_argument('--'+name,required=True,type=Path)
    p.add_argument('--stage',choices=('before','after'),required=True)
    p.add_argument('--before',type=Path)
    a=p.parse_args();report=capture(a.project,a.build,a.output,a.stage)
    changed=[]
    if a.before:
        previous=json.loads(a.before.read_text('utf-8'));now={x['path']:x['sha256'] for x in report['sources']}
        old={x['path']:x['sha256'] for x in previous['sources']}
        changed=sorted(path for path in old.keys()|now.keys() if old.get(path)!=now.get(path))
        prior_configs={x['path']:x['sha256'] for x in previous['effective_configs']}
        after_configs={x['path']:x['sha256'] for x in report['effective_configs']}
        changed+=sorted('config:'+path for path in prior_configs if prior_configs[path]!=after_configs.get(path))
    print(json.dumps(dict(source_files=len(report['sources']),outputs=len(report['outputs']),changed_during_build=changed),ensure_ascii=False))
    raise SystemExit(2 if changed else 0)
