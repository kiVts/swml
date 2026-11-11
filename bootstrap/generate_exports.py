"""Generate ABI-preserving x64 thunks from the system OpenGL export table."""
import argparse
import json
from pathlib import Path
import struct

def exports(path):
    data=Path(path).read_bytes();pe=struct.unpack_from('<I',data,60)[0]
    assert data[:2]==b'MZ' and data[pe:pe+4]==b'PE\0\0'
    assert struct.unpack_from('<H',data,pe+4)[0]==0x8664
    optional=pe+24;size=struct.unpack_from('<H',data,pe+20)[0];sections=[]
    for i in range(struct.unpack_from('<H',data,pe+6)[0]):
        at=optional+size+40*i;vs,rva,rawsize,raw=struct.unpack_from('<IIII',data,at+8);sections.append((rva,rawsize,raw))
    def offset(rva):
        begin,n,raw=next(s for s in sections if s[0]<=rva<s[0]+s[1]);return raw+rva-begin
    rva=struct.unpack_from('<I',data,optional+112)[0];table=offset(rva)
    base,nfunc,nnames,funcs,names,ordinals=struct.unpack_from('<6I',data,table+16)
    assert nfunc==nnames,'unnamed exports need explicit support'
    result=[]
    for i in range(nnames):
        name_rva=struct.unpack_from('<I',data,offset(names)+4*i)[0];at=offset(name_rva);end=data.index(0,at)
        name=data[at:end].decode('ascii');ordinal=base+struct.unpack_from('<H',data,offset(ordinals)+2*i)[0]
        assert name.replace('_','').isalnum();result.append((name,ordinal))
    return result

def main():
    p=argparse.ArgumentParser();p.add_argument('dll');p.add_argument('output');a=p.parse_args();out=Path(a.output);out.mkdir(parents=True,exist_ok=True)
    entries=exports(a.dll);definition=['LIBRARY opengl32','EXPORTS'];asm=['option casemap:none','EXTERN swml_resolve_gl:PROC','.data','ALIGN 8','PUBLIC swml_gl_slots','swml_gl_slots LABEL QWORD']
    assert 'wglMakeCurrent' in dict(entries)
    asm += [f'    DQ OFFSET resolve_{i}' for i in range(len(entries))]
    asm += ['.code']
    for i,(name,ordinal) in enumerate(entries):
        definition.append(f'    {name}={"swml_wglMakeCurrent" if name=="wglMakeCurrent" else "forward_"+str(i)} @{ordinal}')
        asm += [f'forward_{i} PROC',f'    jmp QWORD PTR [swml_gl_slots+{i*8}]',f'forward_{i} ENDP',f'resolve_{i} PROC',f'    mov r11d,{i}','    jmp resolve_dispatch',f'resolve_{i} ENDP']
    definition.append('    swml_bootstrap_version')
    asm += ['resolve_dispatch PROC FRAME','    sub rsp,0A8h','    .allocstack 0A8h','    .endprolog',
        '    mov [rsp+20h],rcx','    mov [rsp+28h],rdx','    mov [rsp+30h],r8','    mov [rsp+38h],r9',
        '    movaps [rsp+40h],xmm0','    movaps [rsp+50h],xmm1','    movaps [rsp+60h],xmm2','    movaps [rsp+70h],xmm3',
        '    mov ecx,r11d','    call swml_resolve_gl','    mov r11,rax',
        '    movaps xmm0,[rsp+40h]','    movaps xmm1,[rsp+50h]','    movaps xmm2,[rsp+60h]','    movaps xmm3,[rsp+70h]',
        '    mov rcx,[rsp+20h]','    mov rdx,[rsp+28h]','    mov r8,[rsp+30h]','    mov r9,[rsp+38h]',
        '    add rsp,0A8h','    jmp r11','resolve_dispatch ENDP','END']
    header='#pragma once\ninline constexpr const char* kGlExports[]={'+','.join(json.dumps(n) for n,_ in entries)+'};\n'
    header+=f'inline constexpr unsigned kMakeCurrent={next(i for i,(n,_) in enumerate(entries) if n=="wglMakeCurrent")};\n'
    (out/'exports.def').write_text('\n'.join(definition)+'\n');(out/'forward.asm').write_text('\n'.join(asm)+'\n');(out/'exports.h').write_text(header)
    print(f'{len(entries)} named OpenGL exports generated')
if __name__=='__main__':main()
