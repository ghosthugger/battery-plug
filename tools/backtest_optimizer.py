from pathlib import Path
import sys,json,subprocess,time,hashlib
from concurrent.futures import ThreadPoolExecutor
import numpy as np
import pandas as pd
P=Path(__file__).resolve().parent; CODE=P.parent
import argparse
parser=argparse.ArgumentParser(description='Offline historical backtest; never accesses the plug')
parser.add_argument('--input',type=Path,default=P.parent/'tests/backtest/se3-fridge-freezer-input.csv')
parser.add_argument('--output',type=Path,default=P.parent/'.esphome/optimizer-backtest')
parser.add_argument('--model',choices=['legacy','mismatch','full-pause'],default='full-pause',
                    help='legacy: old model/plant; mismatch: old model with 95%% physical pause; full-pause: corrected model/plant')
parser.add_argument('--case',action='append',help='Run only a named case (repeatable)')
args=parser.parse_args()
source=args.input.resolve(); r=pd.read_csv(source,index_col=0,parse_dates=True)
r.index=pd.to_datetime(r.index,utc=True).as_unit('ns')
class study:
    START=pd.Timestamp('2026-08-01',tz='Europe/Stockholm')
    END=pd.Timestamp('2026-11-01',tz='Europe/Stockholm')
    @staticmethod
    def known_end(index,t,publication):
        z=index[t].tz_convert('Europe/Stockholm')
        advance=2 if z.hour*60+z.minute>=publication else 1
        end=(z.normalize()+pd.DateOffset(days=advance)).tz_convert('UTC')
        return min(len(index),int(index.searchsorted(end)))
P=args.output.resolve();P.mkdir(parents=True,exist_ok=True)
idx=r.index;price=r.spot_sek_kwh.to_numpy()*1.25+.7;loads=r.base_load_kw.to_numpy()
first=int(idx.searchsorted(study.START.tz_convert('UTC')));last=int(idx.searchsorted(study.END.tz_convert('UTC')))
settle=float(np.quantile(price[:first],.25))/.9
# Realistic bounded history: completed hourly prices observed since startup,
# seven-day rolling median and low-quartile energy continuation value.
hour=pd.Series(price,index=idx).resample('h').mean()
def prepare(publication=795,forecast='bounded'):
    path=P/f'input-{publication}-{forecast}.txt'
    with path.open('w') as f:
        for i in range(len(r)):
            known=study.known_end(idx,i,publication);end=min(len(r),known+96)
            load=float(loads[max(0,i-7*96):i].mean()) if i else .06
            if forecast=='original':
                past=price[max(0,i-28*96):i];tail=float(np.median(past)) if len(past) else float(np.median(price[i:known]));terminal=settle
            else:
                h=idx[i].floor('h');start=max(idx[first].floor('h'),h-pd.Timedelta(hours=168))
                past=hour[(hour.index>=start)&(hour.index<h)].to_numpy()
                if not len(past):past=price[i:known]
                tail=float(np.median(past));terminal=max(0.,float(np.quantile(past,.25)))/.9
            f.write(f'{idx[i].value//10**9} {price[i]:.12f} {loads[i]:.12f} {load:.12f} {tail:.12f} {terminal:.12f} {known} {end}\n')
    return path
subprocess.run(['clang++','-O3','-std=c++17',str(CODE/'tools/optimizer_replay.cpp'),'-o',str(P/'replay')],check=True)
prepared={(pub,fc):prepare(pub,fc) for pub,fc in [(795,'bounded'),(900,'bounded'),(795,'original')]}
cases=[('compact_101',101,0,1,.01,795,'bounded',1),
       ('compact_201',201,0,1,.01,795,'bounded',1),('compact_401',401,0,1,.01,795,'bounded',1),
       ('compact_601',601,0,1,.01,795,'bounded',1),('compact_1001',1001,0,1,.01,795,'bounded',1),
       ('compact_101_float64',101,1,1,.01,795,'bounded',1),
       ('compact_101_high_load',101,0,2.4/1.5,.01,795,'bounded',1),
       ('compact_101_idle20W',101,0,1,.02,795,'bounded',1),
       ('compact_101_late_publication',101,0,1,.01,900,'bounded',1),
       ('compact_101_ideal_load_observability',101,0,1,.01,795,'bounded',0),
       ('compact_101_original_forecast',101,0,1,.01,795,'original',1)]
if args.case: cases=[case for case in cases if case[0] in args.case]
if not cases: raise SystemExit("No matching cases")
def run(case):
    name,n,dbl,scale,idle,pub,forecast,observed=case
    cmd=[str(P/'replay'),str(prepared[pub,forecast]),str(first),str(last),str(settle),str(scale),str(observed),str(idle),str(n),str(dbl),str(P/f'{name}.csv'),str({'legacy':0,'mismatch':1,'full-pause':2}[args.model])]
    begun=time.time();result=json.loads(subprocess.check_output(cmd,text=True));result.update(model=args.model,name=name,publication_minutes=pub,forecast=forecast,wall_seconds=time.time()-begun)
    (P/f'{name}.json').write_text(json.dumps(result,indent=2));print(json.dumps(result),flush=True);return result
with ThreadPoolExecutor(max_workers=3) as pool:results=list(pool.map(run,cases))
pd.DataFrame(results).to_csv(P/'summary.csv',index=False)
(P/'metadata.json').write_text(json.dumps({'settlement_sek_per_kwh':settle,'start':str(study.START),'end_exclusive':str(study.END),'rows':last-first,'cases':results,
 'sources_sha256':{x.name:hashlib.sha256(x.read_bytes()).hexdigest() for x in [CODE/'components/charging_controller/compact_optimizer.h',CODE/'tools/optimizer_replay.cpp',CODE/'tools/backtest_optimizer.py',source]}},indent=2))
