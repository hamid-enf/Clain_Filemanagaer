import os, shutil, subprocess, sys, json
from concurrent.futures import ThreadPoolExecutor
SRC='/home/user/final7/Clain_FileManager/FileManager_lib'
def build(m):
    name=m['name']; d='/home/user/mut5_'+name
    shutil.rmtree(d, ignore_errors=True)
    subprocess.run(f"cd {SRC} && tar cf - . | (mkdir -p {d} && cd {d} && tar xf -)", shell=True, check=True)
    p=os.path.join(d,m['path'])
    s=open(p,'rb').read()
    o=m['old'].replace('\n','\r\n').encode(); n=m['new'].replace('\n','\r\n').encode()
    occ=m.get('occ',0)
    idx=-1
    for _ in range(occ+1):
        idx=s.find(o, idx+1)
        if idx<0: break
    if idx<0:
        shutil.rmtree(d); return f"{name}: SITE NOT FOUND"
    s=s[:idx]+n+s[idx+len(o):]
    open(p,'wb').write(s)
    subprocess.run("find . -name '*.o' -delete; rm -f libfilemanager.a", shell=True, cwd=d)
    r=subprocess.run("make -j1 libfilemanager.a", shell=True, cwd=d, capture_output=True, text=True)
    if r.returncode!=0:
        shutil.rmtree(d); return f"{name}: BUILD FAILED {r.stdout[-300:]} {r.stderr[-300:]}"
    r=subprocess.run("make test 2>&1", shell=True, cwd=d, capture_output=True, text=True)
    out=r.stdout
    res=[l.strip() for l in out.splitlines() if 'Results:' in l]
    fails=[l.strip() for l in out.splitlines() if 'FAIL' in l][:2]
    extra=''
    if m.get('extra'):
        r2=subprocess.run(m['extra'], shell=True, cwd=d, capture_output=True, text=True)
        ls=[l.strip() for l in (r2.stdout+r2.stderr).splitlines() if l.strip()]
        extra=' | restart: '+(ls[-1] if ls else 'no output')
        ff=[l.strip() for l in ls if 'FAIL' in l][:2]
        if ff: extra+=' ('+'; '.join(ff)+')'
    shutil.rmtree(d)
    return f"{name}: {res[-1] if res else 'NO RESULT'}{extra} | {'; '.join(fails) if fails else '-'}"
if __name__=='__main__':
    S=json.load(open(sys.argv[1]))
    with ThreadPoolExecutor(max_workers=2) as ex:
        for line in ex.map(build, S):
            print(line, flush=True)
