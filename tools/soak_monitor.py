"""Finite acceptance observer, not a logging server or controller dependency."""
import argparse
import json
import time
import urllib.request
from pathlib import Path

p=argparse.ArgumentParser()
p.add_argument('--host',default='192.168.1.254')
p.add_argument('--hours',type=float,default=24)
p.add_argument('--output',default='.esphome/soak')
a=p.parse_args();out=Path(a.output);out.mkdir(parents=True,exist_ok=True)
start=time.time();end=start+a.hours*3600
summary=dict(start=start,end_planned=end,samples=0,errors=0,restarts=0,min_heap=None,min_block=None,completed=False)
last_uptime=None
with (out/'samples.jsonl').open('a') as log:
 while time.time()<end:
  record={'observed_at':time.time()}
  try:
   with urllib.request.urlopen('http://'+a.host+'/charging/api/status',timeout=8) as r:s=json.load(r)
   record['status']=s;summary['samples']+=1
   for name,field in [('min_heap','min_heap'),('min_block','min_block')]:
    value=s[field];summary[name]=value if summary[name] is None else min(summary[name],value)
   if last_uptime is not None and s['uptime']<last_uptime:summary['restarts']+=1
   last_uptime=s['uptime']
  except Exception as e:record['error']=str(e);summary['errors']+=1
  log.write(json.dumps(record)+'\n');log.flush()
  summary['last_observation']=time.time()
  (out/'summary.json').write_text(json.dumps(summary,indent=2))
  time.sleep(min(30,max(0,end-time.time())))
summary['completed']=True;summary['finished']=time.time()
summary['memory_pass']=(summary['min_heap'] or 0)>=60*1024 and (summary['min_block'] or 0)>=32*1024
(out/'summary.json').write_text(json.dumps(summary,indent=2))
print(json.dumps(summary,indent=2))
