"""Drive Android local Telecom calls while an external serial monitor records.

The monitor API accepts command/tail POSTs and writes host-timestamped JSONL.
Results certify digital session continuity only; WAV/ASR review is separate.
"""
import argparse, json, time, subprocess, sys, urllib.request
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'tools'))
from phone_ui_capture import Snapshot
from phone_ai_validation import assess_session, read_records

ADB=None
SERIAL=None
MONITOR=None
def api(path,body):
    req=urllib.request.Request(MONITOR.rstrip('/')+'/'+path,json.dumps(body).encode(),{'Content-Type':'application/json'})
    return json.load(urllib.request.urlopen(req,timeout=8))
def command(cmd,port='COM3'):
    return api('command',{'command':cmd,'port':port})
def adb(*args):
    # Android dumpsys output contains UTF-8 bytes even on Windows hosts whose
    # locale is GBK.  Decode explicitly so a non-ASCII diagnostic cannot abort
    # the call before the audio worker is started.
    r=subprocess.run([str(ADB),'-s',SERIAL,*args],capture_output=True,text=True,
                     encoding='utf-8',errors='replace',timeout=25)
    if r.returncode: raise RuntimeError(r.stdout+r.stderr)
    return r.stdout
def action(app,cmd,*extra):
    return adb('shell','am','start','-n','dev.gateway.'+app+'/.MainActivity','--es','command',cmd,*extra)

def require_android():
    # Enumerating a USB device does not prove that an ADB shell works. Check
    # before changing the gateway's runtime answer policy or creating a call.
    if adb('shell','echo','PHONE_AI_ADB_READY').strip() != 'PHONE_AI_ADB_READY':
        raise RuntimeError('preflight: Android shell did not return readiness marker')

def set_auto_answer(enabled, seconds=6):
    command('phone auto on' if enabled else 'phone auto off')
    deadline=time.monotonic()+seconds
    while time.monotonic()<deadline:
        phone=phone_status()
        if phone.get('fresh') and bool(phone['flags']&1) == enabled:
            return phone
        time.sleep(.3)
    raise TimeoutError('automatic answer policy not confirmed by fresh phone status')
def screenshot(log,path):
    start=time.time(); shot=None
    with log.open(encoding='utf8') as f:
        f.seek(0,2)
        command('ui screenshot')
        deadline=time.monotonic()+60
        while time.monotonic()<deadline:
            line=f.readline()
            if not line: time.sleep(.1); continue
            record=json.loads(line)
            if record['time']<start: continue
            line=record['line']
            if line.startswith('UI_SNAPSHOT_BEGIN'): shot=Snapshot(line)
            elif line.startswith('UI_PIXELS ') and shot: shot.chunk(line)
            elif line.startswith('UI_SNAPSHOT_END') and shot:
                shot.finish(line); shot.save(path); print('SNAPSHOT_PASS',str(path),flush=True); return
            elif line.startswith('UI_SNAPSHOT_ERROR'): raise RuntimeError(line)
    raise TimeoutError('snapshot')
def last_ui():
    sent=time.time(); command('ui status'); deadline=time.monotonic()+5
    while time.monotonic()<deadline:
        # A screenshot can emit 1600 rows while another test requests status.
        # Read a response to this request, never an old/cropped tail sample.
        lines=api('tail',{'port':'COM3','count':1000})
        for row in reversed(lines):
            if row['time']>=sent and row['line'].startswith('UI_STATUS '):
                return row['line']
        time.sleep(.05)
    raise TimeoutError('fresh UI status not received')

def phone_status(port='COM3'):
    sent=command('phone status',port)['time']
    deadline=time.monotonic()+4
    while time.monotonic()<deadline:
        rows=api('tail',{'count':60,'port':port})
        for row in reversed(rows):
            if row['time']>=sent and row['line'].startswith('PHONE '):
                return json.loads(row['line'].split(' ',1)[1])
        time.sleep(.1)
    raise TimeoutError('fresh phone status not received')

def transcripts():
    start=time.time(); command('ui transcripts')
    deadline=time.monotonic()+4
    while time.monotonic()<deadline:
        lines=api('tail',{'count':100})
        rows=[r for r in lines if r['time']>=start]
        if any(r['line'].startswith('UI_TRANSCRIPTS_END ') for r in rows):
            return [json.loads(r['line'].split(' ',1)[1]) for r in rows
                    if r['line'].startswith('UI_TRANSCRIPT ')]
        time.sleep(.1)
    raise TimeoutError('ui transcripts requires a supporting firmware')
def run(args):
    out=args.output; out.mkdir(parents=True,exist_ok=False)
    events=[]
    def event(kind,**kw):
        row={'time':time.time(),'kind':kind,**kw}; events.append(row)
        with (out/'bench.jsonl').open('a',encoding='utf8') as f: f.write(json.dumps(row,ensure_ascii=False)+'\n')
        print(json.dumps(row,ensure_ascii=False),flush=True)
    started=False; owns_call=False; session=out.name; start=time.time(); hangup=None; turn=0; error=None
    restore_auto_off=False; control_failure=None
    try:
        require_android()
        command('ui home'); event('before',ui=last_ui()); command('status','COM5')
        command('wifi status','COM5'); command('gateway status')
        deadline=time.monotonic()+30
        while time.monotonic()<deadline:
            ui=last_ui()
            if ' hfp=3 ' in ui and ' ready=1 ' in ui and ' fresh=1 ' in ui: break
            time.sleep(1)
        else: raise TimeoutError('preflight: HFP and authenticated gateway not ready')
        phone=phone_status()
        if not phone.get('fresh') or phone.get('phase') not in (0,4):
            raise RuntimeError('preflight: phone is stale or another call is in progress')
        if not phone['flags']&1:
            # Automatic answer is part of this scenario. Preserve the previous
            # runtime policy so other manual-answer scenarios remain repeatable.
            restore_auto_off=True
            set_auto_answer(True)
            event('auto_answer_temporarily_enabled')
        start=time.time(); owns_call=True; action('calltest','incoming'); event('incoming')
        # A prior failed call can leave its error visible until this call is
        # actually answered. Do not treat that old page as a new cloud failure
        # while Android is still delivering the asynchronous incoming event.
        incoming_at=time.monotonic(); new_ai_started=False
        deadline=incoming_at+30
        while time.monotonic()<deadline:
            ui=last_ui()
            if ' ai=3 ' in ui: break
            if any(token in ui for token in (' ai=2 ', ' ai=4 ', ' ai=5 ')):
                new_ai_started=True
            if ' ai=6 ' in ui and (new_ai_started or time.monotonic()-incoming_at>10):
                raise RuntimeError('AI failed: '+ui)
            time.sleep(1)
        else: raise TimeoutError('AI not listening')
        event('listening',ui=ui)
        # The app may receive start even if ADB loses its reply; cleanup still
        # owns that attempted media session.
        started=True
        action('hfptest','start','--ez','call_media','true','--ei','seconds',str(args.seconds+65),'--ei','rate','16000','--ei','frequency','0','--es','session',session,'--ei','volume_percent','80')
        time.sleep(2); begin=time.monotonic(); turn=0; next_turn=0
        for service in ('audio','media.audio_policy','telecom'):
            (out/('android-'+service+'.txt')).write_text(adb('shell','dumpsys',service),encoding='utf8')
        while time.monotonic()-begin<args.seconds:
            elapsed=time.monotonic()-begin
            ui=last_ui()
            if any(token not in ui for token in (' ready=1 ', ' fresh=1 ', ' hfp=3 ')):
                message='active call control status unavailable; check gateway HFP separately: '+ui
                if not getattr(args,'collect_after_control_failure',False):
                    raise RuntimeError(message)
                control_failure=control_failure or message
                event('control_failure_collection_continued',elapsed=elapsed,ui=ui)
            if any(token in ui for token in (' ai=1 ', ' ai=2 ', ' ai=6 ')):
                raise RuntimeError('active AI session stopped or restarted: '+ui)
            if elapsed>=next_turn and elapsed < args.seconds-15:
                ui=last_ui()
                if ' ai=6 ' in ui: raise RuntimeError('AI failed: '+ui)
                if ' ai=3 ' in ui:
                    if turn: event('reply_text',turn=turn-1,rows=transcripts())
                    prompt='question-'+str(turn%2+1)+'.wav'
                    action('hfptest','speak','--es','wav',prompt)
                    event('speak',turn=turn,prompt=prompt,elapsed=elapsed,ui=ui)
                    turn+=1; next_turn=elapsed+30
                else: event('waiting_reply',ui=ui,elapsed=elapsed); next_turn=elapsed+3
            command('gateway status'); command('status','COM5')
            time.sleep(4)
        event('load_end',elapsed=time.monotonic()-begin,turns=turn,ui=last_ui())
        deadline=time.monotonic()+35
        while time.monotonic()<deadline:
            ui=last_ui()
            if ' ai=3 ' in ui: break
            if ' ai=6 ' in ui: raise RuntimeError('AI failed: '+ui)
            time.sleep(1)
        else: raise TimeoutError('last reply did not drain')
        event('reply_text',turn=turn-1,rows=transcripts())
        if args.screenshot:
            try: screenshot(args.log,out/'active.png')
            except Exception as exc: event('screenshot_failed',error=str(exc))
        hangup=time.time(); command('ui click hangup'); event('hangup'); time.sleep(8)
        action('hfptest','stop'); started=False; time.sleep(2)
        event('after_hangup',ui=last_ui()); command('gateway status'); command('status','COM5')
    except Exception as exc:
        error=str(exc); event('bench_error',error=error)
        # Preserve the failing call before any cleanup can tear down SCO.
        # A stale gateway snapshot is not proof that Android disconnected HFP.
        if started:
            for diagnostic in range(3):
                try:
                    event('failure_observation',sample=diagnostic,ui=last_ui())
                    command('gateway status'); command('status','COM5')
                except Exception as diagnostic_error:
                    event('diagnostic_failed',service='serial',error=str(diagnostic_error))
                time.sleep(4)
            for service in ('audio','bluetooth_manager','telecom'):
                try:
                    (out/('failure-android-'+service+'.txt')).write_text(
                        adb('shell','dumpsys',service),encoding='utf8')
                except Exception as diagnostic_error:
                    event('diagnostic_failed',service=service,error=str(diagnostic_error))
            try:
                (out/'android-logcat.txt').write_text(
                    adb('logcat','-d','-v','epoch','-t','20000'),encoding='utf8')
            except Exception as diagnostic_error:
                event('diagnostic_failed',service='logcat',error=str(diagnostic_error))
    finally:
        if hangup is None: hangup=time.time()
        for label, operation in [
            ('stop_media',lambda: action('hfptest','stop') if started else None),
            ('end_call',lambda: action('calltest','end') if owns_call else None),
            ('pull_recording',lambda: adb('pull','/sdcard/Android/data/dev.gateway.hfptest/files/'+session,str(out/'phone')) if owns_call else None),
        ]:
            event('cleanup_action',action=label)
            try: operation()
            except Exception as exc: error=error or str(exc); event(label+'_failed',error=str(exc))
        # A completed reply is insufficient: capture the return to normal audio
        # after hangup, where resampler/worker cleanup failures may surface.
        try:
            if owns_call:
                time.sleep(8)
                ui=last_ui()
                event('cleanup_complete',ui=ui)
                if error is None and ' ai=1 ' not in ui:
                    raise RuntimeError('AI did not return to ready after hangup: '+ui)
        except Exception as exc:
            error=error or str(exc); event('cleanup_failed',error=str(exc))
        if restore_auto_off:
            try:
                set_auto_answer(False)
                event('auto_answer_policy_restored',enabled=False)
            except Exception as exc:
                error=error or str(exc); event('restore_auto_answer_failed',error=str(exc))
        error=error or control_failure
        cleanup_end=time.time()
        verdict=assess_session(read_records(args.log),start,hangup,turn,cleanup_end,
                               read_records(args.log.with_name('serial-COM5.jsonl')))
        verdict.update(start=start,hangup=hangup,cleanup_end=cleanup_end,bench_error=error,events=events)
        if error: verdict['digital_session']='FAIL'
        verdict['result']='DIGITAL_PATH_PASS' if verdict['digital_session']=='PASS' else 'FAIL'
        (out/'result.json').write_text(json.dumps(verdict,ensure_ascii=False,indent=2),encoding='utf8')
        print(json.dumps({k:v for k,v in verdict.items() if k!='events'},ensure_ascii=False),flush=True)
    if verdict['result']=='FAIL': raise SystemExit(1)
if __name__=='__main__':
    p=argparse.ArgumentParser()
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--log',type=Path,required=True)
    p.add_argument('--adb',type=Path,required=True)
    p.add_argument('--phone',required=True)
    p.add_argument('--monitor',default='http://127.0.0.1:7330')
    p.add_argument('--seconds',type=int,default=300)
    p.add_argument('--screenshot',action='store_true')
    p.add_argument('--collect-after-control-failure',action='store_true',
        help='Keep collecting the requested duration after stale control status; verdict remains FAIL')
    a=p.parse_args()
    if a.seconds<20: p.error('--seconds must be at least 20')
    if not a.log.is_file(): p.error('--log must be an existing serial-COM3.jsonl file')
    if not a.log.with_name('serial-COM5.jsonl').is_file():
        p.error('the same monitor directory must contain serial-COM5.jsonl')
    ADB=a.adb; SERIAL=a.phone; MONITOR=a.monitor
    run(a)
