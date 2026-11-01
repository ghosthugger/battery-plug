"""Read-only browser verification against the installed plug."""
from pathlib import Path
from playwright.sync_api import sync_playwright
root=Path(__file__).resolve().parents[1]
errors=[]
with sync_playwright() as p:
 browser=p.chromium.launch(executable_path='/Applications/Google Chrome.app/Contents/MacOS/Google Chrome',headless=True)
 for width in [1200,390]:
  page=browser.new_page(viewport={'width':width,'height':1000})
  page.on('pageerror',lambda e:errors.append(str(e)))
  page.goto('http://192.168.1.254/charging')
  page.wait_for_function("document.querySelector('#soc').textContent.includes('%')",timeout=20000)
  page.wait_for_function("document.querySelectorAll('svg').length===5",timeout=20000)
  assert not page.locator('#error').inner_text()
  assert page.evaluate('document.documentElement.scrollWidth<=innerWidth')
  print(width,'px:',page.locator('#soc').inner_text(),page.locator('#reason').inner_text())
  page.screenshot(path=str(root/'.esphome'/f'live-dashboard-{width}.png'),full_page=True)
  page.close()
 browser.close()
assert not errors,errors
print('PASS: live desktop/mobile dashboard, all charts, no JS errors or horizontal overflow')
