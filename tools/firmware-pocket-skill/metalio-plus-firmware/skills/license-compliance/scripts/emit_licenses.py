"""Build reachable firmware notices and delivery documents from selected dependencies."""
from pathlib import Path
import argparse, hashlib, json, re, shutil, sys
sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[3]

def record(name, version, source, license_name, paths):
    return dict(name=name, version=version, source=source, license=license_name,
                files=[str(p) for p in paths])

def emit(project, build=None, sdk=None, pinyin=False, extra=None):
    project = project.resolve()
    if project == ROOT or ROOT in project.parents or not (project/'CMakeLists.txt').is_file():
        raise ValueError('Use a generated independent project, not the Skill directory')
    base = ROOT/'skills/license-compliance/licenses'
    font = ROOT/'skills/font-support'
    items = [
        record('EPDiy-derived Plus display', 'CloudZao ad8edf4; derived v0.2', 'https://github.com/vroland/epdiy', 'LGPL-3.0; review combined-work obligations', [base/'LGPL-3.0.txt', base/'GPL-3.0.txt', base/'driver-origin.md']),
        record('CloudZao engineering contributions', 'ad8edf4150a688737c0d7c00cae4413079b262ef', 'https://github.com/CloudZao/Metalio-E-INK4-Plus', 'MIT for applicable contributions', [base/'CloudZao-MIT.txt']),
        record('gray8 waveforms', 'bundled fixed source', 'CloudZao derived waveform; Copyright 2026 mindreset', 'Apache-2.0', [base/'Apache-2.0.txt', ROOT/'skills/firmware-build/assets/plus-app-base/components/epdiy/waveforms/LICENSE.gray8']),
        record('Plus Mono Sans / Noto Sans SC', '1.0.0 / fixed source a3041811', 'https://github.com/google/fonts', 'OFL-1.1', [font/'licenses/OFL.txt', font/'licenses/FONT-NOTICE.txt']),
        record('plus_font runtime', '1.0.0', 'bundled plus_font', 'MIT', [font/'LICENSE']),
        record('ESP-IDF', 'fff9895c82d744c7237be8847347bdd1b07c6643', 'https://github.com/espressif/esp-idf', 'Apache-2.0 plus component-specific licenses', [base/'Apache-2.0.txt']),
    ]
    if pinyin:
        directory = ROOT/'skills/text-input/licenses'
        files = sorted(p for p in directory.rglob('*') if p.is_file())
        if not files: raise ValueError('Missing pinyin license files')
        items.append(record('Pinyin readings and wordfreq-derived candidate data', 'see text-input frozen manifest', 'https://pypi.org/project/wordfreq/3.1.1/', 'MIT readings; CC-BY-SA-4.0 data; Apache-2.0 tooling; source notices', files))
    component_status = 'not_collected_from_actual_build'
    covered_components=[]; missing_component_notices=[]
    if build:
        if not sdk or not (Path(sdk)/'tools/cmake/project.cmake').is_file():
            raise ValueError('--build requires a valid --sdk directory')
        desc = json.loads((Path(build)/'project_description.json').read_text('utf-8'))
        paths = desc.get('build_component_paths', [])
        if not paths: raise ValueError('Build description contains no component paths')
        if not any(Path(x).resolve().is_relative_to(Path(sdk).resolve()) for x in paths):
            raise ValueError('No actual component belongs to the supplied SDK')
        component_status = 'collected_license_files_from_actual_component_paths_review_still_required'
        for directory in map(Path, paths):
            # Only actual components; restrict traversal to known SDK paths.
            if not sdk or not directory.resolve().is_relative_to(Path(sdk).resolve()): continue
            license_files = sorted(p for p in directory.rglob('*') if p.is_file() and
                not any(x in ('tests','test','test_apps','examples') for x in p.relative_to(directory).parts) and
                (p.name.upper().startswith(('LICENSE','COPYING','NOTICE')) or p.name=='ff.c')
                and (p.stat().st_size < 250000 or p.name=='ff.c'))
            if license_files:
                covered_components.append(directory.name)
                items.append(record('ESP-IDF component '+directory.name, 'fixed SDK component',
                    'https://github.com/espressif/esp-idf/tree/fff9895c82d744c7237be8847347bdd1b07c6643/components/'+directory.name,
                    'see collected original component texts', license_files))
                items[-1]['role']='build_tool_only' if directory.name=='esptool_py' else 'build_component_review_link_map_for_runtime_scope'
            else: missing_component_notices.append(directory.name)
        if not covered_components: component_status='no_component_notices_found_review_required'
    if extra: items.extend(json.loads(Path(extra).read_text('utf-8')))
    prepared=[]
    for item in items:
        payload=[]
        for source in map(Path,item['files']):
            data=source.read_bytes();original_hash=hashlib.sha256(data).hexdigest();extracted=False
            if source.suffix=='.c':
                end=data.find(b'*/')
                if not data.startswith(b'/*') or not (0<end<12000) or b'copyright' not in data[:end].lower():
                    raise ValueError('Cannot safely extract copyright notice: '+str(source))
                data=data[:end+2]+b'\n';extracted=True
            data.decode('utf-8-sig',errors='strict')
            payload.append((source,data,original_hash,extracted))
        prepared.append(payload)
    output = project/'third-party-licenses'; output.mkdir(exist_ok=True)
    all_text = ['本清单由 AI 辅助归纳，可能遗漏；商用或分发前请主动复核随附知识产权说明。\n显示副本不支持字符写为 [U+XXXX]；原文见交付许可文件。\n']
    inventory=[]; displayed_hashes={}
    for i,item in enumerate(items):
        info={k:v for k,v in item.items() if k!='files'}; info['files']=[]
        all_text.append('\n=== '+item['name']+' / '+item['license']+' ===\n版本: '+item['version']+'\n来源: '+item['source']+'\n')
        for j,(source,data,original_hash,extracted) in enumerate(prepared[i]):
            dest=output/f'{i:02d}-{j:02d}-{source.name}{".notice.txt" if extracted else ""}'
            dest.write_bytes(data)
            info['files'].append(dict(path=dest.relative_to(project).as_posix(),sha256=hashlib.sha256(data).hexdigest(),
                                     notice_extracted=extracted,original_source_sha256=original_hash))
            digest=hashlib.sha256(data).hexdigest()
            if digest in displayed_hashes:
                all_text.append('相同许可全文已在前项显示：'+displayed_hashes[digest]+'\n')
            else:
                displayed_hashes[digest]=item['name']+' / '+source.name
                all_text.append(data.decode('utf-8-sig',errors='strict')+'\n')
        inventory.append(info)
    report=dict(schema=1, component_license_status=component_status, covered_components=covered_components,
                components_without_separate_notice=missing_component_notices, legal_clearance=False, dependencies=inventory)
    (project/'license-inventory.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n','utf-8')
    document='''# 第三方许可与知识产权说明

本固件供学习参考，清单由AI辅助整理、可能遗漏，不是商业授权或完整法律审查。商用或分发前请主动核对实际依赖、资源来源、署名、源码及再链接等义务；“仅供参考”不免除许可要求。

设备“帮助→开源许可”与本目录third-party-licenses使用同一份清单。英文许可原文应保留；设备无法显示字符的转写不替代原文。详见license-inventory.json中版本、来源与SHA256。构建组件清单含构建时工具，不等于其代码都进入BIN；请结合MAP复核运行时范围。esptool等工具自身许可不自动成为其输出固件的许可。

EPDiy按LGPL-3.0处理，静态链接的分发不能只加版权声明：需要按实际形式准备对应库源码、修改及可重新链接的应用代码/目标文件、构建与必要安装信息。此要求不等于自动公开整个固件口袋。字体OFL与词频数据CC-BY-SA义务各自适用，不把代码许可证代替数据许可证。自研项目的分发授权由权利人确定。

新增网页服务、图片/声音、接口数据、专利或商标等不一定在自动清单里；工程师应补充，发行人应复核。当前清单收集状态：'''+component_status+'\n\n'
    document+='\n'.join('- '+x['name']+' — '+x['version']+' — '+x['license'] for x in inventory)+'\n'
    (project/'第三方许可与知识产权说明.md').write_text(document,'utf-8')
    # Derive supported glyphs from the selected prepared body font; no guessed ranges.
    sys.path.insert(0,str(font/'scripts'))
    from font_format import decode
    _,glyphs=decode((project/'main/generated/body/font.ntf').read_bytes()); allowed={g[0] for g in glyphs}
    text=''.join(all_text).replace('\t','    ').replace('\r','')
    text=''.join(c if ord(c) in allowed or c=='\n' else '[U+%04X]'%ord(c) for c in text)
    (project/'license-display.txt').write_text(text,'utf-8')
    def literal(line): return '"'+''.join(('\\%03o'%b) if b<32 or b>=127 or b in (34,92) else chr(b) for b in line.encode('utf-8'))+'"'
    header='#pragma once\nnamespace plus_licenses { inline const char* Text() { return\n'
    header+='\n'.join(literal(text[i:i+200]) for i in range(0,len(text),200))+'; } }\n'
    (project/'main/generated/license_generated.h').write_text(header,'utf-8')
    return dict(dependencies=len(items),display_text_bytes=len(text.encode()),component_status=component_status)

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--project',required=True,type=Path);p.add_argument('--build',type=Path);p.add_argument('--sdk',type=Path)
    p.add_argument('--pinyin',action='store_true');p.add_argument('--extra',type=Path)
    a=p.parse_args();print(json.dumps(emit(a.project,a.build,a.sdk,a.pinyin,a.extra),ensure_ascii=False))
