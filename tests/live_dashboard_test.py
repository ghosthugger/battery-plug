"""Read-only browser verification against the installed compact-optimiser plug."""
from pathlib import Path
import json
import os
from playwright.sync_api import sync_playwright
root=Path(__file__).resolve().parents[1]
base=os.environ.get('PLUG_URL','http://192.168.1.254').rstrip('/')
errors=[]
with sync_playwright() as p:
 browser=p.chromium.launch(executable_path='/Applications/Google Chrome.app/Contents/MacOS/Google Chrome',headless=True)
 for width in [1200,390]:
  page=browser.new_page(viewport={'width':width,'height':1000})
  page.on('pageerror',lambda e:errors.append(str(e)))
  page.goto(base+'/charging/api/status')
  status=json.loads(page.locator('body').inner_text())
  assert status['algorithm']=='compact-mpc-v2'
  page.goto(base+'/charging')
  page.wait_for_function("document.querySelector('#soc').textContent.includes('%')",timeout=20000)
  page.wait_for_function("document.querySelectorAll('svg').length===6",timeout=20000)
  assert not page.locator('#error').inner_text()
  for key in ['reserve_threshold','reserve_recovery','marginal_adder','charge_efficiency','discharge_efficiency','idle_w']:
   assert float(page.locator('#'+key).input_value())==status[key],key
  assert page.locator('#target').count()==0 and page.locator('#finish').count()==0
  assert page.locator('[data-mode=off]').count()==0 and page.locator('[data-mode=now]').count()==0
  assert page.get_by_role('button',name='Always charging',exact=True).count()==1
  assert page.evaluate('document.documentElement.scrollWidth<=innerWidth')
  print(width,'px:',page.locator('#soc').inner_text(),page.locator('#reason').inner_text())
  page.screenshot(path=str(root/'.esphome'/f'live-dashboard-{width}.png'),full_page=True)
  page.close()
 browser.close()
assert not errors,errors
print('PASS: live desktop/mobile dashboard, installed algorithm/settings, all charts; read-only')
