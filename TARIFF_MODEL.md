# Electricity pricing from invoices and the supplier app

Applied on 11 November 2026 using:

- Ellevio invoice dated 4 November 2026, covering 1–31 October 2026.
- Kärnfull invoice dated 12 October 2026, covering 1–30 September 2026.
- Kärnfull contract and fee screenshots supplied 11 November 2026.

Current supplier fees come from the app screenshots; grid/tax rates come from
the October grid invoice. The original invoices, screenshots and customer
identifiers remain outside Git. Revisit rates when supplier fees or taxes change.

| Variable charge | Excluding VAT (öre/kWh) | Including 25% VAT (öre/kWh) |
| --- | ---: | ---: |
| Transmission (Överföringsavgift) | 20.80 | 26.00 |
| Energy tax (Energiskatt) | 36.00 | 45.00 |
| Supplier fixed per-kWh markup (Fast påslag) | 3.20 | 4.00 |
| Electricity certificates (Elcertifikatsavgift) | 0.30 | 0.3750 |
| Credit and other trading costs | 0.91 | 1.1375 |
| Svenska kraftnät fee | 2.25 | 2.8125 |
| Balance responsibility | 1.87 | 2.3375 |
| Guarantees of origin | 0.80 | 1.0000 |
| Total add-on | 66.13 | **82.6625** |

The Ellevio unit-price columns explicitly say “Pris inkl. moms”. Thus the
energy tax is already subject to VAT: `(20.80 + 36.00) × 1.25 = 71.00`.
Kärnfull's invoice lists line amounts before VAT and adds 25% at subtotal.
The app's 3.20 markup matches that pre-VAT invoice line, so its fee breakdown
is interpreted on the same pre-VAT basis. The five current variable fees sum to
6.13, replacing September's 5.29 rather than adding to it:
`(3.20 + 6.13) × 1.25 = 11.6625`. Total = 71.00 + 11.6625 = 82.6625.

The dashboard's existing setting has 0.1-öre precision, so **82.7 öre/kWh**
is applied. The rounding difference is 0.0375 öre, or 0.000375 kr per imported
kWh. No firmware update is needed for this configuration change.

The plug converts the price feed to Swedish spot öre/kWh including VAT using
`converted_price()`. The optimisation and savings formula is:

```text
Total variable import price = spot price including VAT + 82.7 öre/kWh
                           = spot price excluding VAT × 1.25 + 82.7
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

## Confirmed billing cadence

The 11 November app screenshot identifies an active “Rörligt avtal kvartspris”
contract. This confirms 15-minute spot billing for the customer's consumption,
matching the optimiser's 15-minute price intervals. The app's general explanatory
text still says hourly, but its explicit contract type is quarter-hour pricing.
The earlier uncertainty from the invoice's aggregate spot row is resolved.

## Applied configuration and accounting

The running plug's persistent `marginal_adder` was first updated from 70.00
to 71.00 for grid/tax, then to 81.60 after the September supplier invoice,
and finally **82.70** after the current app fee breakdown, via
`POST /charging/api/settings`, body `marginal_adder=82.7`. Readback confirmed
82.70, a ready optimiser and healthy storage. Capacity, efficiencies,
low-battery thresholds and operating mode were preserved.

Both the optimiser and savings recorder read this setting. Historical savings
retain their recorded rates. The fixed stored-battery-energy accounting value
is retained so a tariff update does not revalue existing inventory/history.

Source verification: `charging_core.h::converted_price`,
`charging_controller.cpp::update_optimizer_` and `Controller::record_`, and
`savings.h::Savings::sample`. A factory reset uses the firmware's generic
70-öre default; reapply 82.70 or the latest invoices' rates afterward.
