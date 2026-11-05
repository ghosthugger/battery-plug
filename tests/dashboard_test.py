"""Headless UI acceptance test with bounded, deterministic API fixtures."""
import json
import time
from pathlib import Path
from playwright.sync_api import sync_playwright

ROOT = Path(__file__).resolve().parents[1]
NOW = 1793520000
state = dict(now=NOW, mode='off', reason='Off', relay=False, soc=72.3, voltage=13.2,
             current=-3.1, power=0, price=75, temp_min=18, temp_max=20, cell_min=3.29,
             cell_max=3.31, alarms='0000000000000000', bms_fresh=True, bms_age=1,
             meter_fresh=True, clock_ok=True, target=90, ceiling=100, finish='07:00',
             capacity_kwh=4, charger_w=400, efficiency=.85, deadline=NOW+72000,
             horizon=NOW+40000, estimated_finish=NOW+36000, achievable=90, unlikely=False,
             provisional=True, load_w=42, fetching=False, price_error='', storage_ok=True,
             free_heap=100000, largest_block=50000, min_heap=90000, min_block=45000,
             uptime=3600, history_days=180, reserve_threshold=5, reserve_recovery=10, reserve_active=False, algorithm='compact-mpc-v1', optimizer_ready=True, decision_until=NOW+900, forecast_price=80, continuation_price=45, predicted_load_w=65, marginal_adder=70, charge_efficiency=.9, discharge_efficiency=.9, idle_w=10, fallback_load_w=60, optimizer_ms=90, optimizer_bytes=7680)
history = [dict(time=NOW-86400+i*300, soc=50+i/10, soc_min=50+i/10, soc_max=51+i/10,
                power=400 if i%20<10 else 0, on_seconds=300 if i%20<10 else 0, coverage=300,
                price=50+i%80, ceiling=100, energy_kwh=.033 if i%20<10 else 0,
                unknown_kwh=0,cost_kr=.03,flags=0) for i in range(288) if i not in range(50,65)]
days = [dict(day=20000+i, energy_kwh=2.5, unknown_kwh=.1, cost_kr=2, on_seconds=22000,
             coverage=86400,soc_min=30,soc_max=90,met=1,missed=0) for i in range(180)]
schedule=[dict(time=NOW+i*900,price=40+i%80,start=NOW+i*900,seconds=900 if i<10 else 0) for i in range(80)]
failures=[]
from urllib.parse import parse_qs

def route(request):
    path=request.request.url.split('plug.test')[-1]
    if path=='/charging':return request.fulfill(body=(ROOT/'components/charging_controller/dashboard.html').read_text(),content_type='text/html')
    endpoint=path.split('/')[-1]
    if endpoint=='settings':
        for k, v in parse_qs(request.request.post_data).items():state[k]=v[0] if k=='finish' else float(v[0])
        answer={'ok':True}
    elif endpoint=='mode':
        state['mode']=parse_qs(request.request.post_data)['mode'][0];answer={'ok':True}
    else:answer={'status':state,'history':history,'days':days,'schedule':schedule,'events':[]}.get(endpoint,{})
    request.fulfill(body=json.dumps(answer),content_type='application/json')

with sync_playwright() as p:
    browser=p.chromium.launch(executable_path='/Applications/Google Chrome.app/Contents/MacOS/Google Chrome',headless=True)
    for width in [1200,390]:
        state['history_days']=180 if width==1200 else 0
        page=browser.new_page(viewport={'width':width,'height':1000})
        page.on('pageerror',lambda e:failures.append(str(e)))
        page.route('http://plug.test/**',route)
        page.goto('http://plug.test/charging')
        page.wait_for_function("document.querySelector('#soc').textContent.includes('72.3')")
        page.wait_for_function("document.querySelector('#months').children.length>0")
        assert page.locator('svg').count()==5
        assert page.locator('#longHistory').is_visible()==(state['history_days']>0)
        assert page.evaluate('document.documentElement.scrollWidth<=innerWidth')
        assert str(int(state['reserve_recovery']))+'%' in page.locator('#reserveInfo').inner_text()
        page.locator('#reserve_threshold').fill('7')
        page.locator('#reserve_recovery').fill('30')
        page.locator('#target').fill('85')
        page.locator('#marginal_adder').fill('65')
        assert 'Automatic cost optimisation' in page.locator('#planning').inner_text() if state['mode']=='auto' else True
        assert 'Manual charge target' in page.locator('body').inner_text()
        page.get_by_role('button',name='Save settings',exact=True).click()
        page.wait_for_function("document.querySelector('#saved').textContent.includes('saved')")
        assert state['target']==85 and state['reserve_threshold']==7 and state['reserve_recovery']==30 and state['marginal_adder']==65
        page.locator('#reserve_recovery').fill('7')
        page.get_by_role('button',name='Save settings',exact=True).click()
        page.wait_for_function("document.querySelector('#error').textContent.includes('above')")
        assert state['reserve_recovery']==30
        page.locator('#reserve_recovery').fill('30')
        page.get_by_role('button',name='Save settings',exact=True).click()
        page.wait_for_function("document.querySelector('#error').textContent===''")
        page.get_by_role('button',name='Automatic',exact=True).click()
        page.wait_for_function("document.querySelector('[data-mode=auto]').getAttribute('aria-pressed')==='true'")
        page.get_by_role('button',name='Off',exact=True).click()
        page.screenshot(path=str(ROOT/'.esphome'/f'dashboard-{width}.png'),full_page=True)
        page.close()
    browser.close()
assert not failures,failures
print('PASS: desktop/mobile layout, all five graphs, six months of summaries, settings and mode controls; no JS errors')
