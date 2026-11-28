"""Record both phone-bridge serial ports without resetting either board.

The command/tail API binds to loopback. Keep this running in a separate terminal
while phone_ai_bench.py drives Android. POST /close releases both serial ports.
"""
import argparse, json, threading, time
from pathlib import Path
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import serial

parser=argparse.ArgumentParser()
parser.add_argument('--output',type=Path,required=True)
args=parser.parse_args()
root=args.output; root.mkdir(parents=True,exist_ok=False)
ports={}; records={}; locks={}
for name in ('COM3','COM5'):
    s=serial.Serial(); s.port=name; s.baudrate=115200; s.timeout=.15
    s.dtr=False; s.rts=False; s.open()
    s.set_buffer_size(rx_size=262144,tx_size=8192)
    ports[name]=s; records[name]=[]; locks[name]=threading.Lock()
def reader(name):
    pending=b''
    with (root/('serial-'+name+'.jsonl')).open('a',encoding='utf8',buffering=1) as f:
        while ports[name].is_open:
            try: data=ports[name].read(max(1,ports[name].in_waiting))
            except Exception as exc:
                if ports[name].is_open:
                    print(json.dumps({'port':name,'reader_error':str(exc)}),flush=True)
                return
            pending+=data
            while b'\n' in pending:
                line,pending=pending.split(b'\n',1)
                item={'time':time.time(),'line':line.decode('utf8',errors='replace').strip('\r')}
                f.write(json.dumps(item,ensure_ascii=False)+'\n')
                with locks[name]:
                    records[name].append(item)
                    if len(records[name])>12000: del records[name][:1000]
for name in ports: threading.Thread(target=reader,args=(name,),daemon=True).start()
class Api(BaseHTTPRequestHandler):
    def log_message(self,*args): pass
    def do_POST(self):
        try:
            v=json.loads(self.rfile.read(int(self.headers['Content-Length'])))
            p=v.get('port','COM3')
            if self.path=='/command':
                ports[p].write((v['command']+'\n').encode()); out={'sent':True,'time':time.time()}
            elif self.path=='/tail':
                with locks[p]: out=records[p][-int(v.get('count',40)):]
            elif self.path=='/close':
                for s in ports.values(): s.close()
                out={'closed':True}
                threading.Thread(target=self.server.shutdown,daemon=True).start()
            else: raise ValueError('unknown path')
            b=json.dumps(out,ensure_ascii=False).encode(); self.send_response(200)
        except Exception as e:
            b=json.dumps({'error':str(e)}).encode(); self.send_response(500)
        self.send_header('Content-Type','application/json'); self.send_header('Content-Length',str(len(b))); self.end_headers(); self.wfile.write(b)
print('monitor listening 127.0.0.1:7330',flush=True)
ThreadingHTTPServer(('127.0.0.1',7330),Api).serve_forever()
