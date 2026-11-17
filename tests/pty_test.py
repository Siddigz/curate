"""End-to-end tests: runs curate in a pseudo-terminal, types keys, and checks the saved files.

Build first with `make`, then run `python3 tests/pty_test.py` from the repo root.
"""
import pty, os, time, fcntl, termios, struct, select, sys, tempfile, signal
APP=os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'curate')
WORK=tempfile.mkdtemp(prefix='curate-test-')
UP,DOWN,RIGHT,LEFT=b'\x1b[A',b'\x1b[B',b'\x1b[C',b'\x1b[D'
CQ,CS,CF,CZ,CY=b'\x11',b'\x13',b'\x06',b'\x1a',b'\x19'
def session(args, keys, rows=10, cols=40):
    pid, fd = pty.fork()
    if pid == 0:
        os.chdir(WORK)
        os.execv(APP, [APP]+args)
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack('HHHH', rows, cols, 0, 0))
    out=b''
    def drain(t):
        nonlocal out; end=time.time()+t
        while time.time()<end:
            if select.select([fd],[],[],0.03)[0]:
                try: out+=os.read(fd,65536)
                except OSError: return
    drain(0.4)
    alive_log=[]
    for k in keys:
        if k=='ALIVE?': alive_log.append(os.waitpid(pid,os.WNOHANG)==(0,0)); continue
        if isinstance(k,tuple) and k[0]=='resize':
            fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack('HHHH', k[1], k[2], 0, 0))
            os.kill(pid, signal.SIGWINCH); drain(0.4); continue
        os.write(fd,k); drain(0.15)
    status=None; end=time.time()+3
    while time.time()<end:
        drain(0.05); r=os.waitpid(pid,os.WNOHANG)
        if r!=(0,0): status=os.WEXITSTATUS(r[1]); break
    if status is None: os.kill(pid,9)
    return status, out, alive_log
P=lambda n: os.path.join(WORK,n)
results=[]
def check(name, cond): results.append((name, bool(cond)))

# 1. open, edit, newline, backspace-join, tab render, save
open(P('a.txt'),'w').write('hello world\n\tindented\nlast line\n')
st,out,_=session(['a.txt'],[RIGHT*5, b',', b'\r', b'\x7f', DOWN, b'X', CS, CQ])
check('opens file and draws its text', b'hello world' in out)
check('tab rendered as spaces', b'    indented' in out)
check('status bar shows filename and line count', b'a.txt - 3 lines' in out)
check('save message', b'bytes written to disk' in out)
check('edit+newline+backspace saved correctly', open(P('a.txt')).read()=='hello, world\n\tindenXted\nlast line\n')
check('quits with 0 after saving', st==0)

# 2. unsaved changes: first Ctrl-Q warns, second quits, file untouched
open(P('b.txt'),'w').write('keep me\n')
st,out,alive=session(['b.txt'],[b'Z', CQ, 'ALIVE?', CQ])
check('warns on unsaved quit', b'Unsaved changes' in out and alive==[True])
check('second Ctrl-Q quits without saving', st==0 and open(P('b.txt')).read()=='keep me\n')

# 3. new file: Save as prompt
st,out,_=session([],[b'new text', CS, b'new.txt', b'\r', CQ])
check('welcome message on empty editor', b'curate -- version' in out)
check('save-as prompt creates file', os.path.exists(P('new.txt')) and open(P('new.txt')).read()=='new text\n')

# 4. scrolling: 50 lines on a 10-row terminal
open(P('c.txt'),'w').write(''.join(f'line {i}\n' for i in range(1,51)))
st,out,_=session(['c.txt'],[DOWN*30, b'#', CS, CQ])
check('scrolled view shows line 31', b'line 31' in out)
check('edit lands on line 31 after scrolling', open(P('c.txt')).read().splitlines()[30]=='#line 31')

# 5. search: jump to a match, then type there
open(P('d.txt'),'w').write('alpha\nbeta\ngamma needle here\ndelta\n')
st,out,_=session(['d.txt'],[CF, b'needle', b'\r', b'>', CS, CQ])
check('search moves cursor to match', open(P('d.txt')).read().splitlines()[2]=='gamma >needle here')

# 6. search cancelled with ESC returns to start
open(P('e.txt'),'w').write('one\ntwo needle\n')
st,out,_=session(['e.txt'],[CF, b'needle', b'\x1b', b'!', CS, CQ])
check('ESC cancels search and restores cursor', open(P('e.txt')).read().splitlines()[0]=='!one')


# 7. syntax highlighting for C, including a comment spanning lines
open(P('f.c'),'w').write('int main(void){\n    /* start\n    still comment */\n    return 42; // done\n    char *s = "hi";\n}\n')
st,out,_=session(['f.c'],[CQ], rows=12, cols=60)
check('status bar shows the c filetype', b'c | 1/6' in out)
check('type keyword coloured green', b'\x1b[32mint' in out)
check('keyword coloured yellow', b'\x1b[33mreturn' in out)
check('number coloured red', b'\x1b[31m42' in out)
check('string coloured magenta', b'\x1b[35m"hi"' in out)
check('multi-line comment coloured on its second line', b'\x1b[36m    still comment */' in out)

# 8. search match is highlighted blue
open(P('g.txt'),'w').write('find the needle here\n')
st,out,_=session(['g.txt'],[CF, b'needle', CQ])
check('search match highlighted', b'\x1b[34mneedle' in out)

# 9. resizing the terminal redraws to the new size
open(P('h.txt'),'w').write(''.join(f'line {i}\n' for i in range(1,51)))
st,out,_=session(['h.txt'],[('resize',20,40), CQ])
check('more lines drawn after resize', b'line 18' in out)

# 10. undo and redo
open(P('u.txt'),'w').write('')
st,out,_=session(['u.txt'],[b'abc def', CZ, CS, CQ])
check('undo removes the last word', open(P('u.txt')).read()=='abc\n')
st,out,_=session(['u.txt'],[CZ, CS, CQ])
check('nothing to undo in a fresh session', b'Nothing to undo' in out)
open(P('v.txt'),'w').write('')
st,out,_=session(['v.txt'],[b'abc def', CZ, CZ, CY, CS, CQ])
check('undo twice then redo once', open(P('v.txt')).read()=='abc\n')
open(P('w.txt'),'w').write('one\ntwo\n')
st,out,_=session(['w.txt'],[DOWN, b'\x7f', CZ, CS, CQ])
check('undo a line join', open(P('w.txt')).read()=='one\ntwo\n')

for n,ok in results: print(('PASS ' if ok else 'FAIL ')+n)
print(f"{sum(o for _,o in results)}/{len(results)} passed")
sys.exit(0 if all(o for _,o in results) else 1)
