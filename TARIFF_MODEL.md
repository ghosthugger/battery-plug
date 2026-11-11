# Electricity pricing from the supplied invoices

Applied on 11 November 2026 using:

- Ellevio invoice dated 4 November 2026, covering 1–31 October 2026.
- Kärnfull invoice dated 12 October 2026, covering 1–30 September 2026.

These are the most recent rates supplied for each bill, from different months.
The original invoices and customer identifiers remain outside Git. Revisit the
rates when later invoices arrive, particularly the supplier's variable costs.

| Variable charge | Excluding VAT (öre/kWh) | Including 25% VAT (öre/kWh) |
| --- | ---: | ---: |
| Transmission (Överföringsavgift) | 20.80 | 26.00 |
| Energy tax (Energiskatt) | 36.00 | 45.00 |
| Supplier fixed per-kWh markup (Fast påslag) | 3.20 | 4.00 |
| Supplier variable costs (Rörliga kostnader) | 5.29 | 6.6125 |
| Total add-on | 65.29 | **81.6125** |

The Ellevio unit-price columns explicitly say “Pris inkl. moms”. Thus the
energy tax is already subject to VAT: `(20.80 + 36.00) × 1.25 = 71.00`.
Kärnfull lists line amounts before VAT and adds 25% VAT at the subtotal:
`(3.20 + 5.29) × 1.25 = 10.6125`. Total = 71.00 + 10.6125 = 81.6125.

The dashboard's existing setting has 0.1-öre precision, so **81.6 öre/kWh**
is applied. The rounding difference is 0.0125 öre, or 0.000125 kr per imported
kWh. No firmware update is needed for this configuration change.

The plug converts the price feed to Swedish spot öre/kWh including VAT using
`converted_price()`. The optimisation and savings formula is:

```text
Total variable import price = spot price including VAT + 81.6 öre/kWh
                           = spot price excluding VAT × 1.25 + 81.6
```

Do not multiply either VAT-inclusive component again. Do not add the invoice's
90.86-öre spot line to the live spot feed: it summarizes the billed period and
is already represented by the spot component.

## Fixed fees

Ellevio's 740 kr/month is already VAT-inclusive. Kärnfull's 31.20 kr/month
excluding VAT is 39.00 kr including VAT. Neither changes when charging moves
between intervals, so neither belongs in the marginal add-on or operating
savings. “Fast påslag” is different: it is charged per kWh and is included.
No demand-based fee is listed on the supplied grid invoice.

## Billing cadence assumption

The model continues to assume the customer's own consumption is billed at
each spot interval's price. The supplier invoice says “Rörligt inköpspris”
and shows a single aggregated spot-price row; that alone does not establish
hourly/quarterly versus a shared monthly price. The grid invoice confirms
quarter-hour metering, which alone does not prove the retail billing contract.
Confirmation from the customer's contract/app has been requested. The supplier
website did not provide accessible primary contract terms during this check.
If actual billing is monthly rather than interval-specific, the current
spot-shifting economic model would need to change. The added per-kWh fees are
independent of that distinction.

## Applied configuration and accounting

The running plug's persistent `marginal_adder` was first updated from 70.00
to 71.00 for grid/tax, then to **81.60** after the supplier invoice, via
`POST /charging/api/settings`, body `marginal_adder=81.6`. Readback confirmed
81.60, a ready optimiser and healthy storage. Capacity, efficiencies,
low-battery thresholds and operating mode were preserved.

Both the optimiser and savings recorder read this setting. Historical savings
retain their recorded rates. The fixed stored-battery-energy accounting value
is retained so a tariff update does not revalue existing inventory/history.

Source verification: `charging_core.h::converted_price`,
`charging_controller.cpp::update_optimizer_` and `Controller::record_`, and
`savings.h::Savings::sample`. A factory reset uses the firmware's generic
70-öre default; reapply 81.60 or the latest invoices' rates afterward.
