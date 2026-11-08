#!/usr/bin/env python3
"""Independent offline plant/DP verifier; writes build products only to a temp dir.

Run after tools/backtest_optimizer.py with Python 3.10+, NumPy and Pandas;
clang++ must be available. No device access or core changes. Results go to
stdout and, when runner artifacts are ready, their verification.json file.
"""
from __future__ import annotations

import argparse
import ast
from dataclasses import dataclass, replace
import hashlib
import json
from pathlib import Path
import random
import subprocess
import tempfile
from datetime import timedelta
from types import SimpleNamespace

import numpy as np
import pandas as pd

ROOT = Path(__file__).resolve().parent
CODE = ROOT.parent.parent
ARTIFACTS = CODE / '.esphome/optimizer-backtest'
INPUT = ROOT / 'se3-fridge-freezer-input.csv'


@dataclass(frozen=True)
class Config:
    capacity: float = 4.0
    reserve: float = 0.4
    recovery: float = 0.8
    charge: float = 0.4
    eta_c: float = 0.9
    eta_d: float = 0.9
    idle: float = 0.01
    dt: float = 0.25
    full_pause: int = 1

    def wire(self):
        return ' '.join(str(x) for x in self.__dict__.values())


def reference(energy, latch, requested, load, p, paused=False):
    """Independent event-based reference with explicit full/release events."""
    out = dict(energy=energy, grid=0., charged=0., battery_load=0., mains_load=0.,
               idle=0., throughput=0., on_hours=0.)
    remaining=p.dt
    sources=[]
    events=0
    while remaining>1e-12:
        events+=1
        assert events<10000
        if energy<=p.reserve+1e-12: latch=True
        if energy>=p.recovery-1e-12: latch=False
        if not p.full_pause or energy<=.95*p.capacity+1e-12: paused=False
        elif energy>=p.capacity-1e-12: paused=True
        on=bool(latch or (requested and not paused))
        sources.append(on)
        duration=remaining
        if on:
            rate=p.eta_c*p.charge
            if latch: duration=min(duration,(p.recovery-energy)/rate)
            if p.full_pause: duration=min(duration,(p.capacity-energy)/rate)
            delta=min(rate*duration,p.capacity-energy)
            energy+=delta
            out['charged']+=delta/p.eta_c
            out['mains_load']+=load*duration
            out['on_hours']+=duration
            out['throughput']+=delta
            if energy>=p.recovery-1e-12: latch=False
            if p.full_pause and energy>=p.capacity-1e-12: paused=True
        else:
            rate=load/p.eta_d+p.idle
            if rate>0:
                duration=min(duration,(energy-p.reserve)/rate)
                if paused: duration=min(duration,(energy-.95*p.capacity)/rate)
            energy-=rate*duration
            out['battery_load']+=load*duration
            out['idle']+=p.idle*duration
            out['throughput']+=rate*duration
            if energy<=p.reserve+1e-12: energy=p.reserve;latch=True
            if energy<=.95*p.capacity+1e-12: paused=False
        assert duration>0
        remaining-=duration
    out['energy']=energy
    out['grid']=out['mains_load']+out['charged']
    out.update(latched=int(latch),full_paused=int(paused),first_on=int(sources[0]),last_on=int(sources[-1]),
               internal_transfers=sum(a!=b for a,b in zip(sources,sources[1:])))
    return out


def small_step(energy, latch, requested, load, p, tick_seconds):
    """Independent sampled supervisor, converging as timestep tends to zero.

    Threshold decisions occur at step starts, allowing small threshold overshoot;
    no exact crossing-time equations are used. Physical full remains capped.
    """
    total=p.dt
    out=dict(energy=energy, grid=0., charged=0., battery_load=0., mains_load=0.,
             idle=0., throughput=0., on_hours=0.)
    tick=tick_seconds/3600
    elapsed=0.
    paused=False
    while elapsed < total-1e-14:
        duration=min(tick,total-elapsed)
        if energy <= p.reserve+1e-12:
            latch=True
        elif energy >= p.recovery-1e-12:
            latch=False
        if not p.full_pause or energy<=.95*p.capacity+1e-12: paused=False
        elif energy>=p.capacity-1e-12: paused=True
        if (requested and not paused) or latch:
            charge=min(p.charge*duration,max(0.,p.capacity-energy)/p.eta_c)
            energy+=p.eta_c*charge
            out['charged']+=charge
            out['mains_load']+=load*duration
            out['on_hours']+=duration
            out['throughput']+=p.eta_c*charge
        else:
            draw=(load/p.eta_d+p.idle)*duration
            energy-=draw
            out['battery_load']+=load*duration
            out['idle']+=p.idle*duration
            out['throughput']+=draw
        elapsed+=duration
    out['energy']=energy
    out['grid']=out['charged']+out['mains_load']
    return out


class Bridge:
    def __init__(self, exe):
        self.process=subprocess.Popen([str(exe)],stdin=subprocess.PIPE,
                                      stdout=subprocess.PIPE,text=True,bufsize=1)

    def request(self, line):
        self.process.stdin.write(line+'\n')
        self.process.stdin.flush()
        reply=self.process.stdout.readline()
        assert reply, f'bridge exited: {self.process.poll()}'
        return json.loads(reply)

    def plant(self, energy, latch, action, load, p):
        return self.request(f'P {energy} {int(latch)} {action} {load} {p.wire()}')

    def solver(self, prices, load, energy, latch, previous, terminal, p, nodes=101, precision='d', first_hours=-1.):
        return self.request(f'S {nodes} {precision} {len(prices)} {load} {energy} '
                            f'{int(latch)} {previous} {terminal} {first_hours} {p.wire()} '
                            +' '.join(map(str,prices)))

    def close(self):
        self.process.stdin.close()
        self.process.wait(timeout=10)
        assert self.process.returncode == 0
        self.process.stdout.close()


def close(a,b,tolerance=2e-10):
    assert abs(a-b) <= tolerance, (a,b,tolerance)


def check_accounting(o, energy, load, p):
    close(o['energy']-energy, p.eta_c*o['charged']-o['battery_load']/p.eta_d-o['idle'])
    close(o['grid'],o['mains_load']+o['charged'])
    close(o['mains_load']+o['battery_load'],load*p.dt)
    close(o['idle'],p.idle*(p.dt-o['on_hours']))
    close(o['throughput'],p.eta_c*o['charged']+o['battery_load']/p.eta_d+o['idle'])
    assert p.reserve-1e-10 <= o['energy'] <= p.capacity+1e-10
    assert 0 <= o['on_hours'] <= p.dt+1e-12
    assert o['charged'] <= p.charge*o['on_hours']+1e-12


def plant_checks(bridge,rng,count,tick):
    cases=[]
    p=Config()
    # All boundary/latch/request combinations, full charging, threshold crossing.
    for e in (p.reserve,p.reserve+1e-8,p.recovery-1e-8,p.recovery,p.capacity-1e-8,p.capacity):
        for latch in (False,True):
            for action in (0,1):
                for load in (0.,.0625,.4):
                    cases.append((e,latch,action,load,p))
    # Regression: small battery, high charger and narrow reserve hysteresis
    # require far more than the old sixteen event segments per interval.
    narrow=Config(capacity=.1,reserve=.01,recovery=.011,charge=2.5,idle=.01)
    cycle=(narrow.recovery-narrow.reserve)*(1/(narrow.eta_c*narrow.charge)+1/(.6/narrow.eta_d+narrow.idle))
    for cycles in (7.9,8.,8.1,32.,130.25):
        for action in (0,1):
            for e,latch in ((narrow.reserve,True),(narrow.recovery,False),(.0105,False)):
                cases.append((e,latch,action,.6,replace(narrow,dt=cycles*cycle)))
    for _ in range(count):
        p=Config(capacity=rng.choice((4.,16.)),charge=rng.choice((.4,1.8)),
                 eta_c=rng.uniform(.8,.95),eta_d=rng.uniform(.8,.95),
                 idle=rng.choice((0.,.01,.02)),dt=rng.choice((.25,.5)))
        e=rng.uniform(p.reserve,p.capacity)
        if rng.random()<.6:
            e=rng.uniform(p.reserve,min(p.recovery+.05,p.capacity))
        cases.append((e,bool(rng.randrange(2)),rng.randrange(2),rng.uniform(0,.6),p))
    max_sample_error=0.
    for energy,latch,action,load,p in cases:
        got=bridge.plant(energy,latch,action,load,p)
        expected=reference(energy,latch,action,load,p)
        for key in expected:
            close(got[key],expected[key])
        check_accounting(got,energy,load,p)
        sampled=small_step(energy,latch,action,load,p,tick)
        # Delay per sampled threshold bounded by one tick; allow accumulated
        # timing errors over all internal events and charge/discharge rates.
        tol=(got['internal_transfers']+2)*(p.charge*p.eta_c+load/p.eta_d+p.idle+p.charge+load)*tick/3600
        for key in sampled:
            error=abs(sampled[key]-got[key])
            max_sample_error=max(max_sample_error,error)
            assert error <= tol+2e-10, (key,error,tol,energy,latch,action,load,p)
    stress=bridge.plant(narrow.reserve,True,0,.6,narrow)
    assert stress['internal_transfers']>16,stress
    return dict(cases=len(cases),small_step_seconds=tick,max_small_step_field_error=max_sample_error,
                narrow_hysteresis_regression='PASS',narrow_case_internal_transfers=stress['internal_transfers'])


def brute_q(prices,load,energy,latch,terminal,p,first_hours=-1.):
    def recurse(t,e,l,paused):
        if t==len(prices):
            return -terminal*(e-p.reserve)
        costs=[]
        for action in (0,1):
            o=reference(e,l,action,load,p,paused)
            costs.append(prices[t]*o['grid']+recurse(t+1,o['energy'],bool(o['latched']),bool(o['full_paused'])))
        return min(costs)
    q=[]
    for action in (0,1):
        o=reference(energy,latch,action,load,replace(p,dt=first_hours) if first_hours>0 else p)
        q.append(prices[0]*o['grid']+recurse(1,o['energy'],bool(o['latched']),bool(o['full_paused'])))
    return np.array(q)


def grid_q(prices,load,energy,latch,terminal,p,nodes=101,first_hours=-1.):
    """Separate float64 Bellman implementation with Python physical transitions."""
    split=(nodes-1)*4//5
    grid=(np.linspace(p.reserve,p.capacity,nodes) if p.reserve>=.95*p.capacity else
          np.concatenate((np.linspace(p.reserve,.95*p.capacity,split+1),
                          np.linspace(.95*p.capacity,p.capacity,nodes-split)[1:])))
    dest=np.zeros((4,2,nodes));buy=np.zeros_like(dest);ls=np.zeros_like(dest,dtype=int)
    for l in range(4):
        for action in (0,1):
            for i,e in enumerate(grid):
                o=reference(e,bool(l&1),action,load,p,bool(l&2))
                dest[l,action,i]=o['energy'];buy[l,action,i]=o['grid'];ls[l,action,i]=o['latched']+2*o['full_paused']
    v=np.tile(-terminal*(grid-p.reserve),(4,1))
    for price in reversed(prices[1:]):
        q=np.empty((4,2,nodes))
        for l in range(4):
            for a in (0,1):
                continuations=np.array([np.interp(dest[l,a],grid,v[k]) for k in range(4)])
                q[l,a]=price*buy[l,a]+np.take_along_axis(continuations,ls[l,a][None,:],axis=0)[0]
        v=q.min(axis=1)
    q=[]
    for action in (0,1):
        o=reference(energy,latch,action,load,replace(p,dt=first_hours) if first_hours>0 else p)
        q.append(prices[0]*o['grid']+np.interp(o['energy'],grid,v[o['latched']+2*o['full_paused']]))
    return np.array(q)


def solver_checks(bridge,rng,count):
    p=Config()
    cases=[([1.],.0625,2.,False,0,1.,p),
           ([-2.,3.,3.,-1.],.1,.405,False,1,1.,p),
           ([3.,-2.,2.,-1.,4.,0.,3.,1.],.4,.79,True,0,.8,p),
           ([0.]*6,0.,4.,False,1,0.,replace(p,idle=0.)),
           ([4.,3.,2.,1.,-1.,-2.],.0625,3.99,False,0,1.,p),
           ([.5]*8,.0625,.4,True,0,1.,p)]
    for _ in range(count):
        n=rng.randint(2,9)
        p=Config(idle=rng.choice((.01,.02)))
        energy=rng.choice((rng.uniform(.4,.85),rng.uniform(3.85,4.),rng.uniform(.4,4.)))
        cases.append(([rng.uniform(-2,4) for _ in range(n)],rng.choice((.8/24,1.5/24,2.4/24,.4)),
                      energy,bool(rng.randrange(2)),rng.randrange(2),rng.choice((0.,.5,1.,2.)),p))
    cases=[(*case,-1.) for case in cases]
    full_cases=cases.copy()
    # Partial quarters also exercise discontinuities adjacent to reserve/latch
    # boundaries; compare decisions to enumeration, not only DP value agreement.
    for case in full_cases:
        for duration in (1/3600,.0625,.125):
            cases.append((*case[:-1],duration))
    for duration in (1/3600,10/3600,.0625,.249999):
        for latch,energy in ((False,.401),(True,.799),(False,3.999)):
            cases.append(([-1.,3.,.2,2.,-.5],.1,energy,latch,0,1.,Config(),duration))
    worst_error={};worst_regret={};max_float_difference=0.;mismatches=0
    for prices,load,energy,latch,prev,terminal,p,first_hours in cases:
        exact=brute_q(prices,load,energy,latch,terminal,p,first_hours)
        nodal=grid_q(prices,load,energy,latch,terminal,p,first_hours=first_hours)
        double101=bridge.solver(prices,load,energy,latch,prev,terminal,p,first_hours=first_hours)
        assert np.max(np.abs(nodal-double101['q']))<2e-10, (nodal,double101)
        for nodes,precision,allowed_regret in ((101,'f',.05),(101,'d',.05),(1001,'d',.01),(10001,'d',.002)):
            result=bridge.solver(prices,load,energy,latch,prev,terminal,p,nodes,precision,first_hours)
            key=f'{nodes}_{precision}'
            error=float(np.max(np.abs(np.array(result['q'])-exact)))
            regret=float(exact[result['action']]-exact.min())
            worst_error[key]=max(error,worst_error.get(key,0.))
            worst_regret[key]=max(regret,worst_regret.get(key,0.))
            assert regret<=allowed_regret, (key,'first-action regret',regret,prices,energy,latch)
            if nodes==101 and precision=='f':
                difference=float(np.max(np.abs(np.array(result['q'])-double101['q'])))
                max_float_difference=max(max_float_difference,difference)
                assert difference<2e-4, ('float arithmetic discrepancy',difference)
                mismatches+=result['action']!=double101['action']
    return dict(cases=len(cases),horizon_max=9,brute_force='all binary action sequences',
                independent_float64_grid_q_check='PASS',worst_q_error_sek=worst_error,
                worst_first_action_regret_sek=worst_regret,max_float32_vs_float64_q_sek=max_float_difference,
                float32_vs_float64_action_differences=mismatches,
                note='Finite-case approximation checks; not a global optimality certificate. Tolerances are predeclared in verifier.')


def forecast_checks(temp):
    """Exercise actual preparation function and runner in scratch directories.

    AST extraction avoids the backtest module's top-level compilation/result
    writes. Its function body is executed unmodified with synthetic globals.
    """
    source=CODE/'tools/backtest_optimizer.py'
    runner=CODE/'tools/optimizer_replay.cpp'
    if not source.exists() or not runner.exists():
        return dict(status='PENDING',reason='Runner not available')
    prepare_node=next(n for n in ast.parse(source.read_text()).body
                      if isinstance(n,ast.FunctionDef) and n.name=='prepare')
    study_class=next(n for n in ast.parse(source.read_text()).body
                     if isinstance(n,ast.ClassDef) and n.name=='study')
    study_node=next(n for n in study_class.body
                    if isinstance(n,ast.FunctionDef) and n.name=='known_end')
    study_node.decorator_list=[]
    known_ns=dict(pd=pd,timedelta=timedelta,TZ='Europe/Stockholm')
    exec(compile(ast.Module(body=[study_node],type_ignores=[]),'study-known_end','exec'),known_ns)
    idx=pd.date_range('2026-07-28', '2026-08-05',freq='15min',tz='Europe/Stockholm',inclusive='left').tz_convert('UTC')
    first=int(idx.searchsorted(pd.Timestamp('2026-08-01',tz='Europe/Stockholm').tz_convert('UTC')))
    prices=.3+np.sin(np.arange(len(idx))*.17)
    loads=.06+.025*np.cos(np.arange(len(idx))*.31)
    mutation_checks=0
    for pub in (795,900):
        # At startup, before/at publication, and after startup history exists.
        stamps=['2026-08-01 00:00','2026-08-02 13:00','2026-08-02 13:15','2026-08-02 15:00']
        for stamp in stamps:
            t=int(idx.searchsorted(pd.Timestamp(stamp,tz='Europe/Stockholm').tz_convert('UTC')))
            end=known_ns['known_end'](idx,t,pub)
            for kind in ('bounded','original'):
                values=[]
                for mutated in (False,True):
                    directory=Path(temp)/f'forecast-{mutation_checks}-{int(mutated)}'
                    directory.mkdir()
                    p=prices.copy();l=loads.copy()
                    if mutated:
                        p[end:]=9999+np.arange(len(p)-end)
                        l[t:]=.5
                    env=dict(np=np,pd=pd,P=directory,r=np.arange(len(idx)),idx=idx,price=p,loads=l,
                             first=first,settle=1.,study=SimpleNamespace(known_end=known_ns['known_end']))
                    env['hour']=pd.Series(p,index=idx).resample('h').mean()
                    exec(compile(ast.Module(body=[prepare_node],type_ignores=[]),'actual-backtest-prepare','exec'),env)
                    data=np.loadtxt(env['prepare'](pub,kind))
                    values.append(data[t])
                    local=idx[t].tz_convert('Europe/Stockholm')
                    days=2 if local.hour*60+local.minute>=pub else 1
                    cutoff=pd.Timestamp(local.date()+timedelta(days=days),tz='Europe/Stockholm').tz_convert('UTC')
                    assert int(data[t,6])==int(idx.searchsorted(cutoff))
                # Columns actual load/billing are allowed to change. Forecast
                # load, tail, terminal and availability indices must not change.
                assert np.array_equal(values[0][3:],values[1][3:]), (pub,stamp,kind,values)
                mutation_checks+=1
    exe=Path(temp)/'actual-runner'
    subprocess.run(['clang++','-O2','-std=c++17',str(runner),'-o',str(exe)],check=True)
    n=24;t=8
    prepared=np.zeros((n,8))
    prepared[:,0]=1785535200+np.arange(n)*900
    prepared[:,1]=.4+np.sin(np.arange(n)*.7)
    prepared[:,2]=.06+.04*np.cos(np.arange(n)*.3)
    prepared[:,3]=.06
    prepared[:,4]=.35
    prepared[:,5]=.8
    prepared[:,6]=np.minimum(n,np.arange(n)+3)
    prepared[:,7]=np.minimum(n,np.arange(n)+10)
    runner_checks=0
    for observed in (0,1):
        baseline=None
        for variant in (0,1,2):
            data=prepared.copy()
            if variant:
                data[int(data[t,6]):,1]=(-100 if variant==1 else 100)
                data[t:,2]=(.5 if variant==1 else .002)
            input_file=Path(temp)/f'runner-{observed}-{variant}.txt'
            trace=Path(temp)/f'runner-{observed}-{variant}.csv'
            np.savetxt(input_file,data,fmt='%.15g')
            subprocess.run([str(exe),str(input_file),'0',str(n),'1.','1.',str(observed),'.01',
                            '101','0',str(trace)],check=True,capture_output=True,text=True)
            rows=pd.read_csv(trace)
            snapshot=rows.loc[:t,['request','predicted_load_kw']].to_numpy()
            if baseline is None:
                baseline=snapshot
            else:
                assert np.array_equal(snapshot,baseline),('runner unavailable-information leak',observed,variant)
                runner_checks+=1
    return dict(status='PASS',actual_prepare_mutation_pairs=mutation_checks,
                actual_runner_mutations=runner_checks,
                checked='Withheld prices and current/future actual load do not alter decision inputs or requests through mutation time.',
                methodology_note='Verified actual bounded definition: seven-day completed hourly history since startup, published-only startup fallback, rolling terminal valuation and prior off-mode load observations.')


def runner_artifact_checks(artifact_root, input_path):
    """Independent replay of published trace actions, accounting and observations.

    This does not rerun optimization or write artifacts. The Python plant, not
    the core, reconstructs physical state and the available load observations.
    """
    metadata_path=artifact_root/'metadata.json'
    if not metadata_path.exists():
        return dict(status='PENDING',reason='Runner metadata not available')
    names=[case['name'] for case in json.loads(metadata_path.read_text())['cases']]
    if not input_path.exists() or any(not (artifact_root/f'{name}.json').exists() or
                                    not (artifact_root/f'{name}.csv').exists() for name in names):
        return dict(status='PENDING',reason='Complete runner artifacts not available')
    metadata_path=artifact_root/'metadata.json'
    if not metadata_path.exists():
        return dict(status='PENDING',reason='Runner completion metadata not yet available')
    metadata=json.loads(metadata_path.read_text())
    for source in (CODE/'components/charging_controller/compact_optimizer.h',
                   CODE/'tools/optimizer_replay.cpp',CODE/'tools/backtest_optimizer.py',input_path):
        digest=hashlib.sha256(source.read_bytes()).hexdigest()
        assert metadata['sources_sha256'].get(source.name)==digest, ('Stale runner artifacts; rerun backtest',source)
    data=pd.read_csv(input_path,index_col=0)
    idx=pd.to_datetime(data.index,utc=True)
    first=int(idx.searchsorted(pd.Timestamp('2026-08-01',tz='Europe/Stockholm').tz_convert('UTC')))
    last=int(idx.searchsorted(pd.Timestamp('2026-11-01',tz='Europe/Stockholm').tz_convert('UTC')))
    loads=data.base_load_kw.to_numpy();prices=data.spot_sek_kwh.to_numpy()*1.25+.7
    outcomes=[]
    for name in names:
        meta=json.loads((artifact_root/f'{name}.json').read_text())
        trace=pd.read_csv(artifact_root/f'{name}.csv')
        assert len(trace)==last-first
        p=Config(idle=meta['idle_w']/1000,full_pause=int(meta.get('model','legacy')!='legacy'))
        scale=meta['load_scale'];energy=2.;latch=False;relay=False;paused=False
        observations={};bill=grid=charged=throughput=idle=battery=mains=0.
        transfers=internal=0
        max_error=0.
        for t,row in enumerate(trace.itertuples(index=False),start=first):
            assert row.time==idx[t].value//10**9
            hour=int(row.time)//3600
            eligible=[v for h,v in observations.items() if h+168>hour]
            observed_energy=sum(v[0] for v in eligible);observed_hours=sum(v[1] for v in eligible)
            pred=observed_energy/observed_hours if observed_hours>1e-9 else .06
            assert abs(row.predicted_load_kw-pred)<2e-10, (name,t,'load prediction not prior off-only mean')
            o=reference(energy,latch,int(row.request),loads[t]*scale,p,paused)
            check_accounting(o,energy,loads[t]*scale,p)
            assert abs(row.energy_kwh-o['energy'])<2e-8, (name,t,'plant energy')
            assert abs(row.grid_kwh-o['grid'])<2e-8, (name,t,'grid purchase')
            assert abs(row.cost_sek-o['grid']*prices[t])<2e-8, (name,t,'bill')
            assert int(row.latch)==o['latched'] and int(row.relay)==o['last_on']
            max_error=max(max_error,abs(row.energy_kwh-o['energy']))
            transfers+=int(o['first_on']!=relay)+o['internal_transfers']
            internal+=o['internal_transfers']
            energy=o['energy'];latch=bool(o['latched']);paused=bool(o['full_paused']);relay=bool(o['last_on'])
            grid+=o['grid'];charged+=o['charged'];throughput+=o['throughput'];idle+=o['idle']
            battery+=o['battery_load'];mains+=o['mains_load'];bill+=o['grid']*prices[t]
            bucket=observations.setdefault(hour,[0.,0.])
            bucket[0]+=o['battery_load'];bucket[1]+=p.dt-o['on_hours']
            # Keep the same bounded observation window without future samples.
            observations={h:v for h,v in observations.items() if h+168>hour}
        for key,value in [('grid_bill_sek',bill),('grid_kwh',grid),('charge_ac_kwh',charged),
                          ('throughput_dc_kwh',throughput),('end_soc',energy*25)]:
            assert abs(meta[key]-value)<2e-7, (name,key,meta[key],value)
        assert meta['source_transfers']==transfers and meta['internal_transfers']==internal
        baseline=float(np.dot(prices[first:last],loads[first:last]*scale)*.25)
        settlement=float(np.quantile(prices[:first],.25))/.9
        saving=baseline-bill+settlement*(energy-2)
        assert abs(meta['saving_sek']-saving)<2e-7
        outcomes.append(dict(name=name,slots=len(trace),saving_sek=saving,
                             source_transfers=transfers,max_energy_error_kwh=max_error))
    return dict(status='PASS',cases=outcomes,
                checked='All trace slots independently replayed; prior off-only load estimates, physical state, latches, transfers and monetary settlement checked.')


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--random-plant',type=int,default=400)
    ap.add_argument('--random-solver',type=int,default=120)
    ap.add_argument('--tick-seconds',type=float,default=.5)
    ap.add_argument('--artifacts',type=Path,default=ARTIFACTS,help='Existing backtest output directory')
    ap.add_argument('--input',type=Path,default=INPUT,help='Historical fixture passed to tools/backtest_optimizer.py')
    args=ap.parse_args()
    assert args.random_plant>=0 and args.random_solver>=0 and 0<args.tick_seconds<=1
    implementation=CODE/'components/charging_controller/compact_optimizer.h'
    sources=[implementation,CODE/'tools/backtest_optimizer.py',CODE/'tools/optimizer_replay.cpp',
             ROOT/'verify_optimizer.py',ROOT/'verifier_bridge.cpp',args.input]
    def source_hashes():
        return {str(p.relative_to(CODE)) if p.is_relative_to(CODE) else str(p):
                hashlib.sha256(p.read_bytes()).hexdigest() for p in sources}
    before=source_hashes()
    with tempfile.TemporaryDirectory(prefix='compact-independent-verify-') as temp:
        exe=Path(temp)/'bridge'
        subprocess.run(['clang++','-O2','-std=c++17',str(ROOT/'verifier_bridge.cpp'),'-o',str(exe)],check=True)
        bridge=Bridge(exe)
        try:
            plant=plant_checks(bridge,random.Random(1911),args.random_plant,args.tick_seconds)
            solver=solver_checks(bridge,random.Random(7119),args.random_solver)
        finally:
            bridge.close()
        forecasts=forecast_checks(temp)
        artifacts=runner_artifact_checks(args.artifacts,args.input)
    after=source_hashes()
    assert before==after,'source changed during audit: rerun against one stable snapshot'
    result=dict(status='PASS' if artifacts['status']=='PASS' and forecasts['status']=='PASS' else 'PARTIAL',
                core_source=str(implementation.relative_to(CODE)),
                core_sha256=after[str(implementation.relative_to(CODE))],sources_sha256=after,
                plant=plant,solver=solver,
                forecast_causality=forecasts,runner_artifacts=artifacts)
    encoded=json.dumps(result,indent=2)+'\n'
    if result['status']=='PASS':
        (args.artifacts/'verification.json').write_text(encoded)
    print(encoded,end='')


if __name__=='__main__':
    main()
