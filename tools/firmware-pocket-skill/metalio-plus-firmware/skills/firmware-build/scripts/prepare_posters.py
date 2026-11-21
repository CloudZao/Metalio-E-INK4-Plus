"""Prepare Chinese native EPD posters with the same checked NTF glyph metrics."""
import hashlib, json, sys
from pathlib import Path
sys.dont_write_bytecode=True

def prepare(root, project):
    fonts=root/'skills/font-support'
    sys.path.insert(0,str(fonts/'scripts'))
    from prepare_font import read_font
    _,spec,entries,_,_=read_font(fonts/'assets/fonts',48,500)
    glyphs={g[0]:g for g in entries}
    assets=project/'main/assets';assets.mkdir(exist_ok=True)
    generated=project/'main/generated';generated.mkdir(exist_ok=True)
    manifest=[]
    for name,gray,lines in [('app_lockscreen',True,['已锁屏','短按电源键返回']),
                            ('app_deep_sleep',False,['已休眠','长按电源键五秒唤醒'])]:
        frame=bytearray([255]*415872) if gray else bytearray(103968)
        def pixel(x,y,value=0):
            if not(0<=x<684 and 0<=y<1216):return
            px,py=y,683-x
            index=py*1216+px
            if gray:
                shift=4*(px&1);frame[index//2]=(frame[index//2]&~(15<<shift))|(value<<shift)
            elif value<8:frame[index//8]|=128>>(index&7)
        for line,baseline in zip(lines,[500,620]):
            selected=[glyphs[ord(c)] for c in line]
            x=(684-sum(g[5] for g in selected))//2
            for cp,w,h,left,top,advance,bits in selected:
                for y in range(h):
                    for xx in range(w):
                        bit=y*w+xx
                        if bits[bit//8]&(128>>(bit&7)):pixel(x+left+xx,baseline+top+y)
                x+=advance
        if gray:
            for y in range(720,726):
                for x in range(120,564):pixel(x,y,8)
        (assets/(name+'.bin')).write_bytes(frame)
        manifest.append(dict(name=name,format='gray4' if gray else 'mono1',width=1216,height=684,
                             bytes=len(frame),sha256=hashlib.sha256(frame).hexdigest(),texts=lines))
    (assets/'posters.json').write_text(json.dumps(manifest,ensure_ascii=False,indent=2)+'\n','utf-8')
    header='#pragma once\n#include "asset_contract.h"\nnamespace app_assets {\n'
    for symbol,item in zip(['Lock','Deep'],manifest):
        fmt='Gray4' if item['format']=='gray4' else 'Mono1'
        header+=f'constexpr plus_assets::Descriptor {symbol}{{plus_assets::Format::{fmt},1216,684,{item["bytes"]}}};\n'
    (generated/'app_assets.h').write_text(header+'}\n','ascii')
    return manifest
