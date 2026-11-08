"""Headless UI acceptance test with bounded, deterministic API fixtures."""
import json
import time
from pathlib import Path
from playwright.sync_api import sync_playwright

ROOT = Path(__file__).resolve().parents[1]
NOW = 1793520000
state = dict(now=NOW, mode='auto', reason='Waiting for cheaper electricity', relay=False, soc=72.3, voltage=13.2,
             current=-3.1, power=0, price=75, temp_min=18, temp_max=20, cell_min=3.29,
             charge_allowed=True, bluetooth_connected=True, bluetooth_handoff='idle', bluetooth_remaining=0,
             cell_max=3.31, alarms='0000000000000000', bms_fresh=True, bms_age=1,
             meter_fresh=True, clock_ok=True, target=90, ceiling=100, finish='07:00',
             capacity_kwh=4, charger_w=400, efficiency=.85, deadline=NOW+72000,
             horizon=NOW+40000, estimated_finish=NOW+36000, achievable=90, unlikely=False,
             provisional=True, load_w=42, fetching=False, price_error='', storage_ok=True,
             free_heap=100000, largest_block=50000, min_heap=90000, min_block=45000,
             uptime=3600, history_days=180, reserve_threshold=5, reserve_recovery=10, reserve_active=False, algorithm='compact-mpc-v3', optimizer_ready=True, decision_until=NOW+900, forecast_price=80, continuation_price=45, predicted_load_w=65, marginal_adder=70, charge_efficiency=.9, discharge_efficiency=.9, idle_w=10, fallback_load_w=60, optimizer_ms=90, optimizer_bytes=7680)
history = [dict(time=NOW-86400+i*300, soc=50+i/10, soc_min=50+i/10, soc_max=51+i/10,
                power=400 if i%20<10 else 0, on_seconds=300 if i%20<10 else 0, coverage=300,
                price=50+i%80, ceiling=100, energy_kwh=.033 if i%20<10 else 0,
                unknown_kwh=0,cost_kr=.03,flags=0) for i in range(288) if i not in range(50,65)]
days = [dict(day=20000+i, energy_kwh=2.5, unknown_kwh=.1, cost_kr=2, on_seconds=22000,
             coverage=86400,soc_min=30,soc_max=90,met=1,missed=0) for i in range(180)]
schedule=[dict(time=NOW+i*900,price=40+i%80,start=NOW+i*900,seconds=900 if i==0 else 0,current=i==0,projection_valid=True,projected_seconds=900 if i in range(8,16) else 0,generated_at=NOW) for i in range(80)]
failures=[]
from urllib.parse import parse_qs

def route(request):
    path=request.request.url.split('plug.test')[-1]
    if path=='/charging':return request.fulfill(body=(ROOT/'components/charging_controller/dashboard.html').read_text(),content_type='text/html')
    endpoint=path.split('/')[-1]
    if endpoint=='settings':
        for k, v in parse_qs(request.request.post_data).items():state[k]=v[0] if k=='finish' else float(v[0])
        answer={'ok':True}
    elif endpoint=='bms-diagnostics':
        answer={'capacity':32,'retained_ram_bytes':1944,'storage_ok':True,'snapshots':[dict(sequence=i+1,time=NOW-i*10,clock_ok=True,uptime_ms=10000,trigger='permission_changed',charge_allowed=bool(i%2),charge_mos=i%2,discharge_mos=1,alarms='0001000200030010',soc=100,voltage=14.4,current=20,cell_min=3.5,cell_max=3.65,temp_min=25,temp_max=26,meter_w=400,relay=True) for i in range(32)]}
    elif endpoint=='schedule':
        answer=[dict(slot,projected_seconds=900 if state['mode']=='always' and slot['projection_valid'] else slot['projected_seconds']) for slot in schedule]
    elif endpoint=='bluetooth':
        action=parse_qs(request.request.post_data)['action'][0]
        state.update(bluetooth_handoff='paused' if action=='disconnect' else 'reconnecting',
                     bluetooth_remaining=300 if action=='disconnect' else 45)
        answer={'ok':True}
    elif endpoint=='mode':
        state['mode']=parse_qs(request.request.post_data)['mode'][0];answer={'ok':True}
    else:answer={'status':state,'history':history,'days':days,'schedule':schedule,'events':[], 'savings':{'started_at':NOW-86400, 'reference_kr':1.5, 'days':[{'day':int(NOW/86400)-1,'known_seconds':86400,'missing_seconds':0,'baseline_kr':5,'grid_kr':3,'inventory_kr':-1,'savings_kr':1}, {'day':int(NOW/86400),'known_seconds':3600,'missing_seconds':600,'baseline_kr':.5,'grid_kr':1,'inventory_kr':.2,'savings_kr':-.3}]}}.get(endpoint,{})
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
        assert page.locator('svg').count()==6
        assert page.locator('#bmsSnapshots tr').count()==32
        assert '0x0001000200030010' in page.locator('#bmsSnapshots').inner_text()
        assert 'Charging prohibited' in page.locator('#bmsSnapshots').inner_text()
        assert '32/32 retained snapshots' in page.locator('#bmsDiagnosticInfo').inner_text()
        assert page.locator('#priceChart [data-projection=true]').count()==8
        assert 'Next projected mains period' in page.locator('#projectionInfo').inner_text()
        assert page.locator('#priceChart [data-projection=false]').count()==1
        assert page.locator('#priceChart [data-now-marker]').text_content()=='Now'
        assert page.locator('#savingsTotal').inner_text()=='0.70 kr'
        assert page.locator('#savingsChart rect[fill="#e58c97"]').count()==1
        assert '0.2 hours excluded' in page.locator('#savingsCoverage').inner_text()
        assert page.locator('#longHistory').is_visible()==(state['history_days']>0)
        assert page.evaluate('document.documentElement.scrollWidth<=innerWidth')
        assert str(int(state['reserve_recovery']))+'%' in page.locator('#reserveInfo').inner_text()
        page.locator('#reserve_threshold').fill('7')
        page.locator('#reserve_recovery').fill('30')
        page.locator('#marginal_adder').fill('65')
        assert 'Automatic cost optimisation' in page.locator('#planning').inner_text() if state['mode']=='auto' else True
        assert page.locator('#target').count()==0 and page.locator('#finish').count()==0
        assert page.locator('[data-mode=off]').count()==0 and page.locator('[data-mode=now]').count()==0
        page.get_by_role('button',name='Save settings',exact=True).click()
        page.wait_for_function("document.querySelector('#saved').textContent.includes('saved')")
        assert state['reserve_threshold']==7 and state['reserve_recovery']==30 and state['marginal_adder']==65
        page.locator('#reserve_recovery').fill('7')
        page.get_by_role('button',name='Save settings',exact=True).click()
        page.wait_for_function("document.querySelector('#error').textContent.includes('above')")
        assert state['reserve_recovery']==30
        page.locator('#reserve_recovery').fill('30')
        page.get_by_role('button',name='Save settings',exact=True).click()
        page.wait_for_function("document.querySelector('#error').textContent===''")
        page.get_by_role('button',name='Automatic',exact=True).click()
        page.wait_for_function("document.querySelector('[data-mode=auto]').getAttribute('aria-pressed')==='true'")
        page.get_by_role('button',name='Always charging',exact=True).click()
        page.wait_for_function("document.querySelector('[data-mode=always]').getAttribute('aria-pressed')==='true'")
        assert 'Always on' in page.locator('#deadline').inner_text()
        page.wait_for_function("document.querySelectorAll('#priceChart [data-projection=true]').length===79")
        state.update(relay=True, bluetooth_handoff='idle', bms_fresh=True)
        page.evaluate('refresh()')
        page.wait_for_function("!document.querySelector('#btDisconnect').disabled")
        page.locator('#btDisconnect').click()
        page.wait_for_function("document.querySelector('#bluetoothInfo').textContent.includes('Phone access active')")
        assert page.locator('#btDisconnect').is_disabled()
        assert not page.locator('#btReconnect').is_disabled()
        assert state['relay'] is True and state['mode']=='always'
        state['bms_fresh']=False
        page.evaluate('refresh()')
        page.locator('#btReconnect').click()
        page.wait_for_function("document.querySelector('#bluetoothInfo').textContent.includes('Reconnecting')")
        state.update(bluetooth_handoff='idle', bms_fresh=True, relay=False)
        page.evaluate('refresh()')
        page.wait_for_function("document.querySelector('#btDisconnect').disabled")
        assert 'throughout' in page.locator('#projectionInfo').inner_text()
        page.screenshot(path=str(ROOT/'.esphome'/f'dashboard-{width}.png'),full_page=True)
        for slot in schedule:slot['projection_valid']=False
        state['mode']='auto'
        page.evaluate('refresh()')
        page.evaluate('refreshHistory()')
        page.wait_for_function("document.querySelector('#projectionInfo').textContent.includes('unavailable')")
        assert page.locator('#priceChart [data-projection=true]').count()==0
        for slot in schedule:slot['projection_valid']=True
        page.close()
    browser.close()
assert not failures,failures
print('PASS: desktop/mobile layout, all six graphs, six months of summaries, settings and mode controls; no JS errors')
