import subprocess,re,sys,time
S=sys.argv[1]; ids=open(S+'/ids.txt').read().split()
B='/home/joe/projects/glfrontier/glfrontier-extended/build-smoke/GLFrontier'
res={}
i=0
while i<len(ids):
    batch=ids[i:i+40]
    cmd="load asteroid; waitgame; wait 2; "+"; ".join("echo ID %s; enter %s; peek b a6+13353"%(x,x) for x in batch)+"; quit"
    try:
        out=subprocess.run([B,'--nosound','--do',cmd],capture_output=True,cwd=S,timeout=400).stdout.decode(errors='replace')
    except subprocess.TimeoutExpired as e:
        out=(e.stdout or b'').decode(errors='replace')
    cur=None; done=0
    for l in out.splitlines():
        m=re.match(r'script: ID (\w+)',l)
        if m: cur=m.group(1)
        m=re.match(r'script: 00003587 = (\w+)',l)
        if m and cur: res[cur]=int(m.group(1),16); done+=1; cur=None
    if done<len(batch): print('batch stall at',i+done,batch[done],flush=True); res[batch[done]]=-1; done+=1
    i+=done
    open(S+'/caps.txt','w').write('\n'.join('%s %d'%kv for kv in res.items()))
    print(i,flush=True)
