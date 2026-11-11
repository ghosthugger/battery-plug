# Electricity pricing from the Ellevio invoice

Applied on 11 November 2026 from the invoice dated 4 November 2026, covering
1–31 October 2026. This records the supplied invoice rates, not an assumption
that the tariff never changes. The invoice itself remains outside Git.

| Variable charge | Excluding VAT (öre/kWh) | Including 25% VAT (öre/kWh) |
| --- | ---: | ---: |
| Transmission (Överföringsavgift) | 20.80 | 26.00 |
| Energy tax (Energiskatt) | 36.00 | 45.00 |
| Total add-on | 56.80 | **71.00** |

Both invoice unit-price columns explicitly say “Pris inkl. moms”. Thus the
energy tax is subject to VAT: `(20.80 + 36.00) × 1.25 = 71.00`. Adding another
25% to the invoice's 26 + 45 would double-count VAT. Displayed consumption is
rounded, so use the printed unit rates rather than dividing invoice totals by
the displayed kWh.

The plug converts the price feed to Swedish spot öre/kWh including VAT using
`converted_price()`. The optimisation and savings formula is:

```text
Total variable import price = spot price including VAT + 71.00 öre/kWh
                           = spot price excluding VAT × 1.25 + 71.00
```

The 740 kr/month fixed subscription charge (already including VAT) does not
change when charging is moved between hours and is excluded from marginal
optimisation and operating savings. No demand-based fee is listed on this bill.
Any variable markup from the separate electricity supplier is additional; this
invoice supplies no rate for it. Fixed retailer fees likewise do not belong in
the per-kWh setting.

## Applied configuration

The running plug's persistent `marginal_adder` changed from 70.00 to 71.00 via
`POST /charging/api/settings`, body `marginal_adder=71`. This is a settings update,
not a firmware change. The existing field stores the VAT-inclusive add-on;
capacity, efficiencies, low-battery thresholds and operating mode were preserved.
Readback confirmed the new rate, a ready optimiser and healthy storage.

Both the optimiser and savings recorder read this setting. Historical savings
retain the rates used when recorded; they are not recalculated retroactively.
The fixed stored-battery-energy accounting value is retained, avoiding revaluing
existing inventory/history when a tariff changes.

Source verification: `charging_core.h::converted_price`,
`charging_controller.cpp::update_optimizer_` and `Controller::record_`, and
`savings.h::Savings::sample`. A future factory reset still uses the firmware's
generic 70-öre default; reapply 71.00 or the latest bill's variable rates afterward.
